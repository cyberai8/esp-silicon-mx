#include "recording_screen.h"
#include "config.h"
#include "i18n.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <memory>
#include <string>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "application.h"
#include "api_endpoints.h"
#include "audio_codec.h"
#include "audio_service.h"
#include "board.h"
#include "home_screen/home_screen.h"
#include "protocol.h"
#include "SdCardManager.hpp"
#include "screen_util.h"
#include "system_info.h"
#include "wifi_required_dialog.h"

#include <cJSON.h>
#include <opus_decoder.h>
#include <opus_encoder.h>
#include <opus_resampler.h>

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);
// 注意：font_puhui_number_50_4 只有 % 和 0-9，没有冒号，计时不能用它。

namespace {

constexpr const char* TAG = "RecordingScreen";

#if defined(BOARD_ESP_VOCAT) || (DISPLAY_WIDTH == 360 && DISPLAY_HEIGHT == 360)
// 360 圆屏：顶/侧安全边距，录音卡片 ≈280 宽，详情页按钮改竖排。
constexpr bool  kRoundLayout   = true;
constexpr auto kPanelSize = DISPLAY_WIDTH;
constexpr int32_t kHeaderTopInset  = 28;
constexpr int32_t kHeaderContentH  = 36;
constexpr int32_t kHeaderH = kHeaderTopInset + kHeaderContentH;   // 64
constexpr int32_t kBackBtnSize = 36;
constexpr int32_t kHeaderSideInset = 36;
constexpr int32_t kTabBarH = 40;
#else
constexpr bool  kRoundLayout   = false;
constexpr int32_t kPanelSize = 720;
constexpr int32_t kHeaderTopInset  = 0;   // 未使用
constexpr int32_t kHeaderContentH  = 0;   // 未使用
constexpr int32_t kHeaderH = 90;
constexpr int32_t kBackBtnSize = 72;
constexpr int32_t kHeaderSideInset = 16;
constexpr int32_t kTabBarH = 64;
#endif
constexpr int32_t kBodyH = kPanelSize - kHeaderH;

// 「录音」Tab 上的计时卡片 / 按钮尺寸：圆屏收紧到跟随 kPanelSize，避免
// 640px 宽的卡片在 360 面板上直接溢出边界。
constexpr int32_t kRecCardW    = kRoundLayout ? kPanelSize - 80 : 640;
constexpr int32_t kRecCardH    = kRoundLayout ? 104 : 220;
constexpr int32_t kRecCardY    = kRoundLayout ? 10 : 40;
constexpr int32_t kRecBtnW     = kRoundLayout ? kPanelSize - 120 : 360;
constexpr int32_t kRecBtnH     = kRoundLayout ? 52 : 88;
constexpr int32_t kRecBtnY     = kRoundLayout ? (kRecCardY + kRecCardH + 14) : 320;
constexpr int32_t kRecTipY     = kRoundLayout ? (kRecBtnY + kRecBtnH + 10) : 440;
constexpr int32_t kRecStatusW  = kRoundLayout ? kPanelSize - 80 : 560;

// 详情覆盖层：圆屏把「播放」「录音转写」两个按钮从并排改成竖排。
constexpr int32_t kDetailBtnW  = kRoundLayout ? kPanelSize - 80 : 300;
constexpr int32_t kDetailBtnH  = kRoundLayout ? 44 : 72;
constexpr int32_t kDetailBtnGap    = 8;    // 竖排时两按钮间距（圆屏专用）
constexpr int32_t kDetailSideInset = 40;
constexpr int32_t kDetailContentW  = kRoundLayout ? (kPanelSize - 128) : (kPanelSize - 80);

// 圆屏详情页：返回顶中，短标题居中，避开圆弧裁切。
constexpr int32_t kDetailTopY    = kRoundLayout ? 8 : 0;
constexpr int32_t kDetailTitleY  = kRoundLayout ? 48 : 0;
constexpr int32_t kDetailMetaY   = kRoundLayout ? 72 : 0;
constexpr int32_t kDetailPlayY   = kRoundLayout ? 96 : 0;
constexpr int32_t kDetailAsrY    = kRoundLayout ? (kDetailPlayY + kDetailBtnH + kDetailBtnGap) : 0;
constexpr int32_t kDetailDelY    = kHeaderH + 48 + kDetailBtnH + 8;

// 录音列表行：圆屏收窄到跟随 kPanelSize，避免 420px 宽的文件名/删除按钮
// 布局在 360 面板上溢出裁切。
constexpr int32_t kListRowH        = kRoundLayout ? 84 : 88;
constexpr int32_t kListDelBtnW     = kRoundLayout ? 56 : 96;
constexpr int32_t kListDelBtnH     = kRoundLayout ? 44 : 56;
constexpr int32_t kListBottomReserve = kRoundLayout ? 40 : 56;

constexpr uint32_t kColorBg = 0x0E1116;
constexpr uint32_t kColorTabBar = 0x12151C;
constexpr uint32_t kColorText = 0xFFFFFF;
constexpr uint32_t kColorSubtle = 0x9AA3B2;
constexpr uint32_t kColorAccent = 0x3B82F6;
constexpr uint32_t kColorRecord = 0xDC2626;
constexpr uint32_t kColorCard = 0x1B2030;
constexpr uint32_t kColorDanger = 0xEF4444;

constexpr const char* kPosixDir = "/sdcard/recordings";
#ifndef AUDIO_INPUT_SAMPLE_RATE
#define AUDIO_INPUT_SAMPLE_RATE 16000
#endif
constexpr int kSampleRate = AUDIO_INPUT_SAMPLE_RATE;
constexpr int kFrameDurationMs = OPUS_FRAME_DURATION_MS;  // 60
constexpr int kSamplesPerFrame = kSampleRate * kFrameDurationMs / 1000;  // 960
// Opus granule 固定按 48 kHz 计：60ms → 2880
constexpr int64_t kGranulePerFrame = 48000 * kFrameDurationMs / 1000;
constexpr int kMaxRecordSeconds = 30 * 60;
constexpr int kMinRecordMs = 300;
constexpr int kPlayChunkSamples = kSamplesPerFrame;
constexpr int kMaxListItems = 80;
constexpr uint32_t kOggSerial = 0x4F505553;  // 'OPUS'

enum class RecFormat : uint8_t {
    Opus = 0,
    Wav,
};

enum class RecState : uint8_t {
    Idle = 0,
    Recording,
    Saving,
    Playing,
};

struct UiState {
    lv_obj_t* screen = nullptr;
    lv_obj_t* tabview = nullptr;
    lv_obj_t* timer_lbl = nullptr;
    lv_obj_t* status_lbl = nullptr;
    lv_obj_t* record_btn = nullptr;
    lv_obj_t* record_btn_lbl = nullptr;
    lv_obj_t* list_scroll = nullptr;
    lv_obj_t* list_empty = nullptr;
    lv_obj_t* list_status = nullptr;

    // 录音详情覆盖层
    lv_obj_t* detail_panel = nullptr;
    lv_obj_t* detail_title = nullptr;
    lv_obj_t* detail_meta = nullptr;
    lv_obj_t* detail_play_btn = nullptr;
    lv_obj_t* detail_play_lbl = nullptr;
    lv_obj_t* detail_asr_btn = nullptr;
    lv_obj_t* detail_asr_lbl = nullptr;
    lv_obj_t* detail_del_btn = nullptr;
    lv_obj_t* detail_status = nullptr;
    lv_obj_t* detail_result = nullptr;
};

UiState s_ui;
bool s_screen_alive = false;
bool s_sd_ready = false;
bool s_detail_open = false;
int s_detail_idx = -1;

std::atomic<RecState> s_state{RecState::Idle};
std::atomic<bool> s_stop_record{false};
std::atomic<bool> s_stop_play{false};
std::atomic<bool> s_stop_asr{false};
std::atomic<int64_t> s_record_start_us{0};
std::atomic<uint32_t> s_recorded_frames{0};

TaskHandle_t s_record_task = nullptr;
TaskHandle_t s_play_task = nullptr;
TaskHandle_t s_asr_task = nullptr;        // 上传转写
TaskHandle_t s_asr_query_task = nullptr;  // 查询转写结果
TaskHandle_t s_meta_task = nullptr;
StackType_t* s_record_stack = nullptr;
StaticTask_t* s_record_tcb = nullptr;
StackType_t* s_play_stack = nullptr;
StaticTask_t* s_play_tcb = nullptr;
StackType_t* s_meta_stack = nullptr;
StaticTask_t* s_meta_tcb = nullptr;
int s_meta_fill_fail_count = 0;
lv_timer_t* s_tick_timer = nullptr;
bool s_wake_disabled_by_us = false;
std::atomic<bool> s_stop_meta{false};
std::atomic<uint32_t> s_play_gen{0};

struct PlayTaskArgs {
    char path[192];
    uint32_t gen;
};

char s_playing_path[192] = {};
char s_detail_path[192] = {};
char s_detail_name[96] = {};

struct FileEntry {
    char name[96];
    char path[160];
    int duration_sec = 0;
    size_t size_bytes = 0;
    RecFormat format = RecFormat::Opus;
    bool duration_ready = false;  // sidecar / WAV 尺寸已就绪；否则后台补算
};

std::vector<FileEntry> s_files;

inline lv_style_selector_t Sel(lv_part_t part, lv_state_t state) {
    return static_cast<lv_style_selector_t>(part | state);
}

void OnSwipeBack();
void OnBackClicked(lv_event_t* e);
void UpdateRecordButtonUi();
void UpdateTimerLabel();
void RebuildFileList();
void RebuildFileList(bool schedule_fill);
void RefreshListRowMeta();
void StopPlayback();
void ForceStopAll();
void ShowDetail(int idx);
void HideDetail();
bool DeleteRecordingAt(int idx);
void UpdateDetailPlayButton();
void SetDetailStatusText(const char* text);
void SetDetailResultText(const char* text);
void BuildDetailPanel(lv_obj_t* parent);
void ScheduleDurationFill();
void ScheduleAsrQuery();
bool WriteDurationSidecar(const char* opus_path, int duration_sec);
int ReadDurationSidecar(const char* opus_path);
void UnlinkDurationSidecar(const char* opus_path);
int DurationSecFromOpusFileTail(const char* path);
int EnsureOpusDuration(FileEntry& entry);

bool EnsureRecordingsDir() {
    struct stat st;
    if (stat(kPosixDir, &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    if (mkdir(kPosixDir, 0755) == 0) {
        return true;
    }
    ESP_LOGE(TAG, "mkdir(%s) failed", kPosixDir);
    return false;
}

void PutLe16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

void PutLe32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

void PutLe64(uint8_t* p, int64_t v) {
    const uint64_t u = static_cast<uint64_t>(v);
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<uint8_t>((u >> (8 * i)) & 0xFF);
    }
}

uint32_t ReadLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

int64_t ReadLe64(const uint8_t* p) {
    uint64_t u = 0;
    for (int i = 0; i < 8; ++i) {
        u |= static_cast<uint64_t>(p[i]) << (8 * i);
    }
    return static_cast<int64_t>(u);
}

// Ogg CRC（多项式 0x04C11DB7，与 RFC 3533 一致）
uint32_t* OggCrcTable() {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t r = i << 24;
            for (int j = 0; j < 8; ++j) {
                r = (r & 0x80000000u) ? ((r << 1) ^ 0x04c11db7u) : (r << 1);
            }
            table[i] = r;
        }
        ready = true;
    }
    return table;
}

uint32_t OggCrc(const uint8_t* data, size_t len) {
    const uint32_t* table = OggCrcTable();
    uint32_t crc = 0;
    for (size_t i = 0; i < len; ++i) {
        crc = (crc << 8) ^ table[((crc >> 24) & 0xFFu) ^ data[i]];
    }
    return crc;
}

bool WriteOggPage(FILE* f, uint8_t header_type, int64_t granule, uint32_t serial,
                  uint32_t page_seq, const uint8_t* body, size_t body_len) {
    if (f == nullptr) {
        return false;
    }

    std::vector<uint8_t> segs;
    if (body_len == 0) {
        segs.push_back(0);
    } else {
        size_t rem = body_len;
        while (rem >= 255) {
            segs.push_back(255);
            rem -= 255;
        }
        segs.push_back(static_cast<uint8_t>(rem));
    }
    if (segs.size() > 255) {
        ESP_LOGE(TAG, "ogg packet too large: %u", static_cast<unsigned>(body_len));
        return false;
    }

    const size_t header_len = 27 + segs.size();
    std::vector<uint8_t> page(header_len + body_len);
    std::memcpy(page.data(), "OggS", 4);
    page[4] = 0;
    page[5] = header_type;
    PutLe64(page.data() + 6, granule);
    PutLe32(page.data() + 14, serial);
    PutLe32(page.data() + 18, page_seq);
    PutLe32(page.data() + 22, 0);  // checksum placeholder
    page[26] = static_cast<uint8_t>(segs.size());
    std::memcpy(page.data() + 27, segs.data(), segs.size());
    if (body_len > 0) {
        std::memcpy(page.data() + header_len, body, body_len);
    }

    const uint32_t crc = OggCrc(page.data(), page.size());
    PutLe32(page.data() + 22, crc);

    return fwrite(page.data(), 1, page.size(), f) == page.size();
}

bool WriteOpusHeaders(FILE* f, uint32_t serial, uint32_t& page_seq) {
    uint8_t head[19] = {};
    std::memcpy(head, "OpusHead", 8);
    head[8] = 1;   // version
    head[9] = 1;   // mono
    PutLe16(head + 10, 0);  // pre_skip
    PutLe32(head + 12, static_cast<uint32_t>(kSampleRate));
    PutLe16(head + 16, 0);  // output gain
    head[18] = 0;  // mapping family
    if (!WriteOggPage(f, 0x02 /* BOS */, 0, serial, page_seq++, head, sizeof(head))) {
        return false;
    }

    const char* vendor = "xingzhi-ai";
    const uint32_t vendor_len = static_cast<uint32_t>(std::strlen(vendor));
    std::vector<uint8_t> tags(8 + 4 + vendor_len + 4);
    std::memcpy(tags.data(), "OpusTags", 8);
    PutLe32(tags.data() + 8, vendor_len);
    std::memcpy(tags.data() + 12, vendor, vendor_len);
    PutLe32(tags.data() + 12 + vendor_len, 0);  // user comment list count
    return WriteOggPage(f, 0x00, 0, serial, page_seq++, tags.data(), tags.size());
}

void KeepLeftChannelOnly(std::vector<int16_t>& samples) {
    if (samples.size() < 2) {
        return;
    }
    const size_t mono_count = samples.size() / 2;
    for (size_t i = 0, j = 0; i < mono_count; ++i, j += 2) {
        samples[i] = samples[j];
    }
    samples.resize(mono_count);
}

void DisableWakeWordIfNeeded() {
    auto& as = Application::GetInstance().GetAudioService();
    if (!as.IsWakeWordRunning()) {
        return;
    }
    as.EnableWakeWordDetection(false);
    s_wake_disabled_by_us = true;
    // 等 AudioInputTask 退出当前 Read/Feed，避免与开功放竞态。
    vTaskDelay(pdMS_TO_TICKS(150));
}

// 录音结束后 RX 仍占双工 I2S（TDM 4ch），直接开 TX 会失败；与 SD 音乐页一致先关麦再播。
bool PrepareAudioForPlayback(AudioService& as, AudioCodec* codec) {
    DisableWakeWordIfNeeded();
    if (!as.IsStarted()) {
        as.Start();
    }
    if (codec != nullptr) {
        if (codec->input_enabled()) {
            codec->EnableInput(false);
            vTaskDelay(pdMS_TO_TICKS(100));
        } else if (!codec->output_enabled() && codec->duplex()) {
            // 双工板冷 idle：走一遍 RX 开/关，与录音路径一样复位 I2S/TDM。
            codec->EnableInput(true);
            if (codec->input_enabled()) {
                vTaskDelay(pdMS_TO_TICKS(80));
                codec->EnableInput(false);
                vTaskDelay(pdMS_TO_TICKS(80));
            }
        }
        if (codec->output_enabled()) {
            codec->EnableOutput(false);
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    as.SetExternalPlaybackHold(true);
    as.NotifyExternalPlayback();
    if (codec == nullptr) {
        ESP_LOGW(TAG, "playback prepare failed: no codec");
        return false;
    }
    if (!codec->output_enabled()) {
        ESP_LOGW(TAG, "playback prepare failed: output disabled");
        return false;
    }
    return true;
}

void ReleaseAudioAfterPlayback(AudioService& as) {
    as.SetExternalPlaybackHold(false);
}

// I2S DMA 需要较大的连续 internal 块；堆碎片时 esp_codec_dev 可能在失败路径崩溃。
constexpr size_t kMinLargestForWakeWord = 8192;

void RestoreWakeWordIfNeededImpl(bool deferred_retry) {
    if (!s_wake_disabled_by_us) {
        return;
    }
    if (!Application::GetInstance().IsVoiceChatAllowed()) {
        s_wake_disabled_by_us = false;
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(deferred_retry ? 200 : 80));
    const size_t largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (largest < kMinLargestForWakeWord) {
        if (!deferred_retry) {
            ESP_LOGW(TAG, "defer wake word restore: largest=%u",
                     static_cast<unsigned>(largest));
            Application::GetInstance().Schedule([]() {
                RestoreWakeWordIfNeededImpl(true);
            });
        } else {
            ESP_LOGW(TAG, "skip wake word restore: largest=%u",
                     static_cast<unsigned>(largest));
        }
        return;
    }
    Application::GetInstance().GetAudioService().EnableWakeWordDetection(true);
    s_wake_disabled_by_us = false;
}

void RestoreWakeWordIfNeeded() {
    RestoreWakeWordIfNeededImpl(false);
}

// 进入录音 / 点「开始录音」前释放云端监听与唤醒词 AFE，腾出 internal RAM
// 给 rec_save（Opus 编码栈约 28KB）。
void PrepareMicForRecording() {
    auto& app = Application::GetInstance();
    auto& as = app.GetAudioService();

    const DeviceState st = app.GetDeviceState();
    if (st == kDeviceStateSpeaking) {
        app.AbortSpeaking(kAbortReasonNone);
    }
    app.StopListening();
    as.EnableAudioTesting(false);
    as.EnableVoiceProcessing(false);
    as.ResetDecoder();

    if (as.IsWakeWordRunning()) {
        as.ReleaseWakeWordDetection();
        s_wake_disabled_by_us = true;
        vTaskDelay(pdMS_TO_TICKS(150));
    } else {
        DisableWakeWordIfNeeded();
    }

    if (!as.IsStarted()) {
        as.Start();
    }
    if (AudioCodec* codec = Board::GetInstance().GetAudioCodec(); codec != nullptr) {
        codec->EnableOutput(false);
    }

    ESP_LOGI(TAG,
             "mic prepared (state=%d started=%d wake=%d proc=%d int_free=%u largest=%u)",
             static_cast<int>(st), as.IsStarted() ? 1 : 0,
             as.IsWakeWordRunning() ? 1 : 0, as.IsAudioProcessorRunning() ? 1 : 0,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(
                 heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
}

void RecordTask(void* arg);
void PlayTask(void* arg);

struct PlayTaskArgs;

bool SpawnRecordTask() {
    constexpr uint32_t kRecordTaskStack = 2048 * 14;
    s_record_task = nullptr;

    if (xTaskCreatePinnedToCore(RecordTask, "rec_save", kRecordTaskStack, nullptr,
                                tskIDLE_PRIORITY + 3, &s_record_task, 0) == pdPASS) {
        ESP_LOGI(TAG, "record task started (internal stack=%u)",
                 static_cast<unsigned>(kRecordTaskStack));
        return true;
    }

    ESP_LOGW(TAG, "record task internal create failed, trying SPIRAM stack");
    if (s_record_stack == nullptr) {
        s_record_stack = static_cast<StackType_t*>(heap_caps_malloc(
            kRecordTaskStack, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (s_record_tcb == nullptr) {
        s_record_tcb = static_cast<StaticTask_t*>(heap_caps_malloc(
            sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (s_record_stack == nullptr || s_record_tcb == nullptr) {
        ESP_LOGE(TAG, "record task SPIRAM alloc failed");
        return false;
    }

    s_record_task = xTaskCreateStaticPinnedToCore(
        RecordTask, "rec_save", kRecordTaskStack, nullptr, tskIDLE_PRIORITY + 3,
        s_record_stack, s_record_tcb, 0);
    if (s_record_task == nullptr) {
        ESP_LOGE(TAG, "record task static create failed");
        return false;
    }
    ESP_LOGI(TAG, "record task started (SPIRAM stack=%u)",
             static_cast<unsigned>(kRecordTaskStack));
    return true;
}

void FormatTimer(char* buf, size_t buf_size, int total_sec) {
    if (total_sec < 0) {
        total_sec = 0;
    }
    const int mm = total_sec / 60;
    const int ss = total_sec % 60;
    std::snprintf(buf, buf_size, "%02d:%02d", mm, ss);
}

void FormatFileSize(char* buf, size_t buf_size, size_t bytes) {
    if (bytes >= 1024 * 1024) {
        std::snprintf(buf, buf_size, I18n::T("%.2f MB"), bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024) {
        std::snprintf(buf, buf_size, I18n::T("%.1f KB"), bytes / 1024.0);
    } else {
        std::snprintf(buf, buf_size, I18n::T("%u B"), static_cast<unsigned>(bytes));
    }
}

// 列表行标题：圆屏用短日期时间，避免长文件名叠行。
void FormatListTitle(const char* filename, char* buf, size_t buf_size) {
    if (filename == nullptr || buf == nullptr || buf_size == 0) {
        return;
    }
    buf[0] = '\0';
    const char* stem = filename;
    const char* slash = std::strrchr(filename, '/');
    if (slash != nullptr) {
        stem = slash + 1;
    }

    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (std::sscanf(stem, "REC_%*[^_]_%4d%2d%2d_%2d%2d%2d", &year, &month, &day, &hour,
                    &minute, &second) == 6) {
        if (kRoundLayout) {
            std::snprintf(buf, buf_size, "%02d/%02d %02d:%02d", month, day, hour, minute);
        } else {
            std::snprintf(buf, buf_size, "%04d/%02d/%02d %02d:%02d:%02d", year, month, day,
                          hour, minute, second);
        }
        return;
    }

    strlcpy(buf, stem, buf_size);
    char* dot = std::strrchr(buf, '.');
    if (dot != nullptr) {
        *dot = '\0';
    }
}

void SetRecordStatusText(const char* text) {
    if (!s_screen_alive || text == nullptr || s_ui.status_lbl == nullptr) {
        return;
    }
    lv_label_set_text(s_ui.status_lbl, text);
}

void SetIdleRecordHint() {
    char buf[96];
    std::snprintf(buf, sizeof(buf), I18n::T("点击下方按钮开始录音（最长 %d 分钟）"),
                  kMaxRecordSeconds / 60);
    SetRecordStatusText(buf);
}

void SetRecordingStatusHint() {
    char buf[96];
    std::snprintf(buf, sizeof(buf), I18n::T("正在录音…（最长 %d 分钟）"),
                  kMaxRecordSeconds / 60);
    SetRecordStatusText(buf);
}

void SetListStatusText(const char* text) {
    if (!s_screen_alive || text == nullptr || s_ui.list_status == nullptr) {
        return;
    }
    lv_label_set_text(s_ui.list_status, text);
}

void SetDetailStatusText(const char* text) {
    if (!s_screen_alive || text == nullptr || s_ui.detail_status == nullptr) {
        return;
    }
    lv_label_set_text(s_ui.detail_status, text);
}

void SetDetailResultText(const char* text) {
    if (!s_screen_alive || text == nullptr || s_ui.detail_result == nullptr) {
        return;
    }
    lv_label_set_text(s_ui.detail_result, text);
}

void PostPlayStatusFromWorker(const char* text) {
    if (text == nullptr) {
        return;
    }
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    if (s_screen_alive) {
        if (s_detail_open) {
            SetDetailStatusText(text);
        } else {
            SetListStatusText(text);
        }
        UpdateDetailPlayButton();
        UpdateRecordButtonUi();
    }
    esp_lv_adapter_unlock();
}

void FailPlaybackFromWorker(uint32_t my_gen, const char* status) {
    s_play_task = nullptr;
    const bool is_latest = (my_gen == s_play_gen.load());
    if (is_latest) {
        s_playing_path[0] = '\0';
        s_state.store(RecState::Idle);
    }
    if (status != nullptr && is_latest) {
        PostPlayStatusFromWorker(status);
    }
}

void PostDetailStatusFromWorker(const char* text) {
    if (text == nullptr) {
        return;
    }
    auto* copy = static_cast<char*>(heap_caps_malloc(std::strlen(text) + 1, MALLOC_CAP_8BIT));
    if (copy == nullptr) {
        return;
    }
    std::strcpy(copy, text);
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        heap_caps_free(copy);
        return;
    }
    if (s_screen_alive && s_detail_open) {
        SetDetailStatusText(copy);
    }
    esp_lv_adapter_unlock();
    heap_caps_free(copy);
}

void PostDetailResultFromWorker(const char* text) {
    if (text == nullptr) {
        return;
    }
    auto* copy = static_cast<char*>(heap_caps_malloc(std::strlen(text) + 1, MALLOC_CAP_8BIT));
    if (copy == nullptr) {
        return;
    }
    std::strcpy(copy, text);
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        heap_caps_free(copy);
        return;
    }
    if (s_screen_alive && s_detail_open) {
        SetDetailResultText(copy);
    }
    esp_lv_adapter_unlock();
    heap_caps_free(copy);
}

void PostStatusFromWorker(bool to_list_tab, const char* text) {
    if (text == nullptr) {
        return;
    }
    auto* copy = static_cast<char*>(heap_caps_malloc(std::strlen(text) + 1, MALLOC_CAP_8BIT));
    if (copy == nullptr) {
        return;
    }
    std::strcpy(copy, text);
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        heap_caps_free(copy);
        return;
    }
    if (s_screen_alive) {
        if (to_list_tab) {
            SetListStatusText(copy);
        } else {
            SetRecordStatusText(copy);
        }
    }
    esp_lv_adapter_unlock();
    heap_caps_free(copy);
}

void UpdateTimerLabel() {
    if (!s_screen_alive || s_ui.timer_lbl == nullptr) {
        return;
    }
    int sec = 0;
    if (s_state.load() == RecState::Recording) {
        const int64_t start = s_record_start_us.load();
        if (start > 0) {
            sec = static_cast<int>((esp_timer_get_time() - start) / 1000000LL);
        }
    } else if (s_recorded_frames.load() > 0 && s_state.load() == RecState::Idle) {
        // 保留结束时写好的最终时长
        return;
    }
    char buf[16];
    FormatTimer(buf, sizeof(buf), sec);
    lv_label_set_text(s_ui.timer_lbl, buf);
}

void OnTickTimer(lv_timer_t* /*t*/) {
    if (s_state.load() == RecState::Recording) {
        UpdateTimerLabel();
    }
}

void StartTickTimer() {
    if (s_tick_timer != nullptr) {
        return;
    }
    s_tick_timer = lv_timer_create(OnTickTimer, 200, nullptr);
}

void StopTickTimer() {
    if (s_tick_timer != nullptr) {
        lv_timer_delete(s_tick_timer);
        s_tick_timer = nullptr;
    }
}

void UpdateRecordButtonUi() {
    if (!s_screen_alive || s_ui.record_btn == nullptr || s_ui.record_btn_lbl == nullptr) {
        return;
    }
    const RecState st = s_state.load();
    switch (st) {
    case RecState::Idle:
        lv_obj_set_style_bg_color(s_ui.record_btn, lv_color_hex(kColorRecord), LV_PART_MAIN);
        lv_label_set_text(s_ui.record_btn_lbl, I18n::T("开始录音"));
        lv_obj_add_flag(s_ui.record_btn, LV_OBJ_FLAG_CLICKABLE);
        break;
    case RecState::Recording:
        lv_obj_set_style_bg_color(s_ui.record_btn, lv_color_hex(0x4B5563), LV_PART_MAIN);
        lv_label_set_text(s_ui.record_btn_lbl, I18n::T("结束录音"));
        lv_obj_add_flag(s_ui.record_btn, LV_OBJ_FLAG_CLICKABLE);
        break;
    case RecState::Saving:
        lv_obj_set_style_bg_color(s_ui.record_btn, lv_color_hex(0x4B5563), LV_PART_MAIN);
        lv_label_set_text(s_ui.record_btn_lbl, I18n::T("保存中…"));
        lv_obj_remove_flag(s_ui.record_btn, LV_OBJ_FLAG_CLICKABLE);
        break;
    case RecState::Playing:
        lv_obj_set_style_bg_color(s_ui.record_btn, lv_color_hex(kColorRecord), LV_PART_MAIN);
        lv_label_set_text(s_ui.record_btn_lbl, I18n::T("开始录音"));
        lv_obj_remove_flag(s_ui.record_btn, LV_OBJ_FLAG_CLICKABLE);
        break;
    }
}

void MakeNextRecordPath(char* path, size_t path_size) {
    // 设备序列：与配网 AP 相同取 MAC 末两字节 %02X%02X；MAC 来源统一 SystemInfo::GetMacAddress()
    char serial[8] = "0000";
    {
        const std::string mac = SystemInfo::GetMacAddress();
        unsigned b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0;
        if (std::sscanf(mac.c_str(), "%x:%x:%x:%x:%x:%x", &b0, &b1, &b2, &b3, &b4,
                        &b5) == 6) {
            std::snprintf(serial, sizeof(serial), "%02X%02X", b4 & 0xff, b5 & 0xff);
        }
    }

    time_t now = time(nullptr);
    struct tm tm_info = {};
    if (localtime_r(&now, &tm_info) != nullptr && tm_info.tm_year > 70) {
        std::snprintf(path, path_size, "%s/REC_%s_%04d%02d%02d_%02d%02d%02d.opus",
                      kPosixDir, serial, tm_info.tm_year + 1900, tm_info.tm_mon + 1,
                      tm_info.tm_mday, tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
        return;
    }
    const unsigned long ts_ms =
        static_cast<unsigned long>(esp_timer_get_time() / 1000ULL);
    std::snprintf(path, path_size, "%s/REC_%s_%lu.opus", kPosixDir, serial, ts_ms);
}

bool VerifyOpusFileHeader(const char* path);
bool FinalizeRecordingPartFile(const char* final_path, const char* part_path);
void SyncRecordingsDir();
void CleanupStalePartFiles();
void WarnIfSdFsckArtifacts();

void RecordTask(void* /*arg*/) {
    ESP_LOGI(TAG, "record task running");
    auto& as = Application::GetInstance().GetAudioService();
    char path[192] = {};
    char part_path[208] = {};
    FILE* file = nullptr;
    uint32_t frames = 0;
    uint32_t page_seq = 0;
    bool ok = false;
    bool hit_max = false;
    int part_path_len = 0;
    std::unique_ptr<OpusEncoderWrapper> encoder;

    DisableWakeWordIfNeeded();
    as.ResetDecoder();

    if (!EnsureRecordingsDir()) {
        PostStatusFromWorker(false, I18n::T("无法创建录音目录"));
        goto done;
    }

    MakeNextRecordPath(path, sizeof(path));
    part_path_len = std::snprintf(part_path, sizeof(part_path), "%s.part", path);
    if (part_path_len < 0 ||
        static_cast<size_t>(part_path_len) >= sizeof(part_path)) {
        PostStatusFromWorker(false, I18n::T("写入失败，未保存"));
        goto done;
    }
    file = fopen(part_path, "wb");
    if (file == nullptr) {
        ESP_LOGE(TAG, "fopen failed: %s", part_path);
        PostStatusFromWorker(false, I18n::T("写入失败，未保存"));
        goto done;
    }
    // 录音页写入走全缓冲时，异常断电可能只落盘部分 cluster；尽量实时刷盘。
    setvbuf(file, nullptr, _IONBF, 0);

    encoder = std::make_unique<OpusEncoderWrapper>(kSampleRate, 1, kFrameDurationMs);
    encoder->SetComplexity(0);
    encoder->SetDtx(false);

    if (!WriteOpusHeaders(file, kOggSerial, page_seq)) {
        PostStatusFromWorker(false, I18n::T("写入失败，未保存"));
        goto done;
    }

    s_record_start_us.store(esp_timer_get_time());
    s_recorded_frames.store(0);

    {
        std::vector<int16_t> frame;
        frame.reserve(static_cast<size_t>(kSamplesPerFrame) * 2);
        std::vector<uint8_t> opus_pkt;
        opus_pkt.reserve(256);

        while (!s_stop_record.load()) {
            if (!as.ReadAudioData(frame, kSampleRate, kSamplesPerFrame)) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            AudioCodec* codec = Board::GetInstance().GetAudioCodec();
            if (codec != nullptr && codec->input_channels() == 2) {
                KeepLeftChannelOnly(frame);
            }
            if (static_cast<int>(frame.size()) != kSamplesPerFrame) {
                continue;
            }
            if (!encoder->Encode(std::move(frame), opus_pkt)) {
                ESP_LOGW(TAG, "opus encode failed");
                continue;
            }

            ++frames;
            const int64_t granule = static_cast<int64_t>(frames) * kGranulePerFrame;
            const bool force_eos =
                ((esp_timer_get_time() - s_record_start_us.load()) / 1000) >=
                kMaxRecordSeconds * 1000;
            if (!WriteOggPage(file, 0x00, granule, kOggSerial, page_seq++,
                              opus_pkt.data(), opus_pkt.size())) {
                PostStatusFromWorker(false, I18n::T("写入失败，未保存"));
                goto done;
            }
            s_recorded_frames.store(frames);

            if (force_eos) {
                ESP_LOGI(TAG, "max record duration reached");
                hit_max = true;
                break;
            }
        }

        // 若最后一页未标 EOS，补一帧空 EOS 页（或把最后一页重写太麻烦，写空 EOS）
        if (frames > 0) {
            const int64_t granule = static_cast<int64_t>(frames) * kGranulePerFrame;
            if (!WriteOggPage(file, 0x04 /* EOS */, granule, kOggSerial, page_seq++,
                              nullptr, 0)) {
                PostStatusFromWorker(false, I18n::T("写入失败，未保存"));
                goto done;
            }
        }
    }

    {
        const int duration_ms = static_cast<int>(
            (esp_timer_get_time() - s_record_start_us.load()) / 1000);
        if (duration_ms < kMinRecordMs || frames < 5) {
            ESP_LOGI(TAG, "discard short recording: %d ms / %u frames", duration_ms,
                     static_cast<unsigned>(frames));
            fclose(file);
            file = nullptr;
            unlink(part_path);
            UnlinkDurationSidecar(path);
            PostStatusFromWorker(false, I18n::T("录音太短，再试一次"));
            goto done;
        }

        fflush(file);
        if (fsync(fileno(file)) != 0) {
            ESP_LOGW(TAG, "fsync failed: %s", part_path);
        }
        fclose(file);
        file = nullptr;

        if (!FinalizeRecordingPartFile(path, part_path)) {
            UnlinkDurationSidecar(path);
            PostStatusFromWorker(false, I18n::T("写入失败，未保存"));
            goto done;
        }

        ok = true;
        {
            const int duration_sec =
                static_cast<int>(frames * kFrameDurationMs / 1000);
            WriteDurationSidecar(path, duration_sec);
        }
        ESP_LOGI(TAG, "saved %s (%u frames, %d ms)", path,
                 static_cast<unsigned>(frames), duration_ms);
        if (hit_max) {
            char msg[96];
            std::snprintf(msg, sizeof(msg), I18n::T("已达最长 %d 分钟，已自动保存"),
                          kMaxRecordSeconds / 60);
            PostStatusFromWorker(false, msg);
        } else {
            PostStatusFromWorker(false, I18n::T("已保存到 SD 卡"));
        }
    }

done:
    if (file != nullptr) {
        fclose(file);
        file = nullptr;
    }
    if (!ok && part_path[0] != '\0') {
        unlink(part_path);
        UnlinkDurationSidecar(path);
    }
    encoder.reset();

    if (AudioCodec* codec = Board::GetInstance().GetAudioCodec();
        codec != nullptr && codec->input_enabled()) {
        codec->EnableInput(false);
    }

    s_state.store(RecState::Idle);
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        if (s_screen_alive) {
            UpdateRecordButtonUi();
            if (ok) {
                char buf[16];
                FormatTimer(buf, sizeof(buf),
                            static_cast<int>(frames * kFrameDurationMs / 1000));
                if (s_ui.timer_lbl != nullptr) {
                    lv_label_set_text(s_ui.timer_lbl, buf);
                }
                RebuildFileList();
            }
        }
        esp_lv_adapter_unlock();
    }

    s_record_task = nullptr;
    vTaskDelete(nullptr);
}

void StartRecording() {
    if (!s_sd_ready) {
        SetRecordStatusText(I18n::T("请插入 SD 卡"));
        return;
    }
    if (s_state.load() != RecState::Idle || s_record_task != nullptr) {
        return;
    }

    StopPlayback();

    PrepareMicForRecording();

    auto& as = Application::GetInstance().GetAudioService();
    if (!as.IsStarted()) {
        ESP_LOGW(TAG, "StartRecording: audio service not started");
        SetRecordStatusText(I18n::T("音频未就绪"));
        return;
    }
    if (as.IsAudioProcessorRunning()) {
        ESP_LOGW(TAG, "StartRecording: audio processor still busy");
        SetRecordStatusText(I18n::T("音频忙，稍后再试"));
        return;
    }

    s_stop_record.store(false);
    s_state.store(RecState::Recording);
    if (s_ui.timer_lbl != nullptr) {
        lv_label_set_text(s_ui.timer_lbl, "00:00");
    }
    SetRecordingStatusHint();
    UpdateRecordButtonUi();
    StartTickTimer();

    if (!SpawnRecordTask()) {
        s_record_task = nullptr;
        s_state.store(RecState::Idle);
        StopTickTimer();
        UpdateRecordButtonUi();
        SetRecordStatusText(I18n::T("录音任务启动失败"));
    }
}

void StopRecording() {
    if (s_state.load() != RecState::Recording) {
        return;
    }
    s_stop_record.store(true);
    s_state.store(RecState::Saving);
    StopTickTimer();
    UpdateRecordButtonUi();
    SetRecordStatusText(I18n::T("保存中…"));
}

void OnRecordBtnClicked(lv_event_t* /*e*/) {
    const RecState st = s_state.load();
    if (st == RecState::Idle) {
        StartRecording();
    } else if (st == RecState::Recording) {
        StopRecording();
    }
}

bool HasExt(const char* name, const char* ext4) {
    // ext4 like ".wav" / ".opus" (lowercase), case-insensitive match on last 4 or 5 chars
    if (name == nullptr || ext4 == nullptr) {
        return false;
    }
    const size_t len = std::strlen(name);
    const size_t elen = std::strlen(ext4);
    if (len < elen) {
        return false;
    }
    for (size_t i = 0; i < elen; ++i) {
        char a = name[len - elen + i];
        char b = ext4[i];
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<char>(a - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

bool IsRecordingFilename(const char* name) {
    if (name == nullptr) {
        return false;
    }
    const size_t len = std::strlen(name);
    if (len > 5 && std::strcmp(name + len - 5, ".part") == 0) {
        return false;
    }
    return HasExt(name, ".opus") || HasExt(name, ".wav");
}

void SyncRecordingsDir() {
    const int dirfd = open(kPosixDir, O_RDONLY);
    if (dirfd >= 0) {
        fsync(dirfd);
        close(dirfd);
    }
}

bool VerifyOpusFileHeader(const char* path) {
    if (path == nullptr) {
        return false;
    }
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        return false;
    }
    uint8_t magic[4] = {};
    const bool ok =
        (fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "OggS", 4) == 0);
    fclose(f);
    return ok;
}

// 先写 .part，校验 OggS 后 rename，并 fsync 目录项，降低重启/FAT 异常时文件头损坏概率。
bool FinalizeRecordingPartFile(const char* final_path, const char* part_path) {
    if (!VerifyOpusFileHeader(part_path)) {
        ESP_LOGE(TAG, "part file header verify failed: %s", part_path);
        unlink(part_path);
        return false;
    }
    unlink(final_path);
    if (rename(part_path, final_path) != 0) {
        ESP_LOGE(TAG, "rename failed: %s -> %s", part_path, final_path);
        unlink(part_path);
        return false;
    }
    SyncRecordingsDir();
    if (!VerifyOpusFileHeader(final_path)) {
        ESP_LOGE(TAG, "final file header verify failed: %s", final_path);
        unlink(final_path);
        return false;
    }
    return true;
}

void CleanupStalePartFiles() {
    DIR* dir = opendir(kPosixDir);
    if (dir == nullptr) {
        return;
    }
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        const size_t len = std::strlen(ent->d_name);
        if (len <= 5 || std::strcmp(ent->d_name + len - 5, ".part") != 0) {
            continue;
        }
        char full[192];
        if (std::snprintf(full, sizeof(full), "%s/%s", kPosixDir, ent->d_name) < 0) {
            continue;
        }
        ESP_LOGW(TAG, "remove stale part file: %s", full);
        unlink(full);
    }
    closedir(dir);
    SyncRecordingsDir();
}

void WarnIfSdFsckArtifacts() {
    DIR* dir = opendir("/sdcard");
    if (dir == nullptr) {
        return;
    }
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (std::strcmp(ent->d_name, "FOUND.000") == 0) {
            ESP_LOGW(TAG,
                     "SD card has FOUND.000 (FAT was repaired); recordings may "
                     "corrupt after reboot — reformat FAT32 recommended");
            break;
        }
    }
    closedir(dir);
}

void MakeDurationSidecarPath(char* out, size_t out_size, const char* opus_path) {
    std::snprintf(out, out_size, "%s.dur", opus_path);
}

bool WriteDurationSidecar(const char* opus_path, int duration_sec) {
    if (opus_path == nullptr || duration_sec < 0) {
        return false;
    }
    char dur_path[192];
    MakeDurationSidecarPath(dur_path, sizeof(dur_path), opus_path);
    FILE* f = fopen(dur_path, "wb");
    if (f == nullptr) {
        ESP_LOGW(TAG, "write dur sidecar failed: %s", dur_path);
        return false;
    }
    const int n = std::fprintf(f, "%d\n", duration_sec);
    fclose(f);
    return n > 0;
}

int ReadDurationSidecar(const char* opus_path) {
    if (opus_path == nullptr) {
        return -1;
    }
    char dur_path[192];
    MakeDurationSidecarPath(dur_path, sizeof(dur_path), opus_path);
    FILE* f = fopen(dur_path, "rb");
    if (f == nullptr) {
        return -1;
    }
    int sec = -1;
    if (std::fscanf(f, "%d", &sec) != 1 || sec < 0) {
        sec = -1;
    }
    fclose(f);
    return sec;
}

void UnlinkDurationSidecar(const char* opus_path) {
    if (opus_path == nullptr) {
        return;
    }
    char dur_path[192];
    MakeDurationSidecarPath(dur_path, sizeof(dur_path), opus_path);
    unlink(dur_path);
}

// 从文件尾读最后若干 KB，解析 Ogg 页 granule（避免整文件逐页扫描）。
int DurationSecFromOpusFileTail(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    const long file_size = ftell(f);
    if (file_size <= 27) {
        fclose(f);
        return 0;
    }

    constexpr long kTailMax = 64 * 1024;
    const long start = (file_size > kTailMax) ? (file_size - kTailMax) : 0;
    const size_t len = static_cast<size_t>(file_size - start);
    auto* buf = static_cast<uint8_t*>(
        heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buf == nullptr) {
        buf = static_cast<uint8_t*>(heap_caps_malloc(len, MALLOC_CAP_8BIT));
    }
    if (buf == nullptr) {
        fclose(f);
        return 0;
    }
    if (fseek(f, start, SEEK_SET) != 0 || fread(buf, 1, len, f) != len) {
        heap_caps_free(buf);
        fclose(f);
        return 0;
    }
    fclose(f);

    int64_t last_granule = 0;
    for (size_t i = 0; i + 27 <= len; ++i) {
        if (std::memcmp(buf + i, "OggS", 4) != 0 || buf[i + 4] != 0) {
            continue;
        }
        const uint8_t nseg = buf[i + 26];
        if (nseg == 0 || i + 27 + nseg > len) {
            continue;
        }
        size_t body = 0;
        for (uint8_t s = 0; s < nseg; ++s) {
            body += buf[i + 27 + s];
        }
        if (i + 27 + nseg + body > len) {
            continue;  // 不完整页（缓冲区开头可能截断）
        }
        const int64_t granule = ReadLe64(buf + i + 6);
        if (granule > 0) {
            last_granule = granule;
        }
    }
    heap_caps_free(buf);
    if (last_granule <= 0) {
        return 0;
    }
    return static_cast<int>(last_granule / 48000);
}

int EnsureOpusDuration(FileEntry& entry) {
    if (entry.format != RecFormat::Opus) {
        return entry.duration_sec;
    }
    if (entry.duration_ready && entry.duration_sec >= 0) {
        return entry.duration_sec;
    }
    const int from_side = ReadDurationSidecar(entry.path);
    if (from_side >= 0) {
        entry.duration_sec = from_side;
        entry.duration_ready = true;
        return entry.duration_sec;
    }
    const int from_tail = DurationSecFromOpusFileTail(entry.path);
    entry.duration_sec = from_tail;
    entry.duration_ready = true;
    if (from_tail > 0) {
        WriteDurationSidecar(entry.path, from_tail);
    }
    return entry.duration_sec;
}

void DurationFillTask(void* /*arg*/) {
    std::vector<std::string> pending;
    pending.reserve(16);
    for (const auto& e : s_files) {
        if (e.format == RecFormat::Opus && !e.duration_ready) {
            pending.emplace_back(e.path);
        }
    }

    for (const auto& path : pending) {
        if (s_stop_meta.load() || !s_screen_alive) {
            break;
        }
        const int from_side = ReadDurationSidecar(path.c_str());
        int sec = from_side;
        if (sec < 0) {
            sec = DurationSecFromOpusFileTail(path.c_str());
            if (sec > 0) {
                WriteDurationSidecar(path.c_str(), sec);
            } else if (sec == 0) {
                // 仍写入 0，避免反复扫损坏文件
                WriteDurationSidecar(path.c_str(), 0);
            }
        }
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            if (s_screen_alive) {
                for (auto& e : s_files) {
                    if (e.path == path) {
                        e.duration_sec = (sec >= 0) ? sec : 0;
                        e.duration_ready = true;
                        break;
                    }
                }
            }
            esp_lv_adapter_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (!s_stop_meta.load() && s_screen_alive) {
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            if (s_screen_alive && s_ui.list_scroll != nullptr && !s_detail_open) {
                RefreshListRowMeta();
            } else if (s_screen_alive && s_detail_open && s_detail_idx >= 0 &&
                       s_detail_idx < static_cast<int>(s_files.size())) {
                FileEntry& e = s_files[static_cast<size_t>(s_detail_idx)];
                if (s_ui.detail_meta != nullptr) {
                    char dur[16];
                    char size_buf[24];
                    FormatTimer(dur, sizeof(dur), e.duration_sec);
                    FormatFileSize(size_buf, sizeof(size_buf), e.size_bytes);
                    char meta[80];
                    std::snprintf(meta, sizeof(meta), I18n::T("时长 %s · %s"), dur,
                                  size_buf);
                    lv_label_set_text(s_ui.detail_meta, meta);
                }
            }
            esp_lv_adapter_unlock();
        }
    }

    s_meta_task = nullptr;
    bool still_need = false;
    for (const auto& e : s_files) {
        if (e.format == RecFormat::Opus && !e.duration_ready) {
            still_need = true;
            break;
        }
    }
    if (still_need && !s_stop_meta.load() && s_screen_alive) {
        ScheduleDurationFill();
    }
    vTaskDelete(nullptr);
}

void ScheduleDurationFill() {
    if (!s_screen_alive || s_meta_task != nullptr) {
        return;
    }
    bool need = false;
    for (const auto& e : s_files) {
        if (e.format == RecFormat::Opus && !e.duration_ready) {
            need = true;
            break;
        }
    }
    if (!need) {
        s_meta_fill_fail_count = 0;
        return;
    }

    constexpr size_t kMinInternalForMeta = 16384;
    const size_t largest_int =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (largest_int < kMinInternalForMeta) {
        ESP_LOGW(TAG, "defer duration fill: largest_int=%u",
                 static_cast<unsigned>(largest_int));
        for (auto& e : s_files) {
            if (e.format == RecFormat::Opus && !e.duration_ready) {
                e.duration_sec = 0;
                e.duration_ready = true;
            }
        }
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            RefreshListRowMeta();
            esp_lv_adapter_unlock();
        }
        return;
    }

    s_stop_meta.store(false);
    constexpr uint32_t kMetaTaskStack = 6 * 1024;
    if (xTaskCreatePinnedToCore(DurationFillTask, "rec_meta", kMetaTaskStack, nullptr,
                                tskIDLE_PRIORITY + 1, &s_meta_task, 0) == pdPASS) {
        s_meta_fill_fail_count = 0;
        return;
    }
    ESP_LOGW(TAG, "duration fill task internal create failed, trying SPIRAM stack");
    if (s_meta_stack == nullptr) {
        s_meta_stack = static_cast<StackType_t*>(heap_caps_malloc(
            kMetaTaskStack, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (s_meta_tcb == nullptr) {
        s_meta_tcb = static_cast<StaticTask_t*>(heap_caps_malloc(
            sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (s_meta_stack == nullptr || s_meta_tcb == nullptr) {
        s_meta_task = nullptr;
        ESP_LOGW(TAG, "duration fill task start failed");
        ++s_meta_fill_fail_count;
        if (s_meta_fill_fail_count >= 2) {
            for (auto& e : s_files) {
                if (e.format == RecFormat::Opus && !e.duration_ready) {
                    e.duration_sec = 0;
                    e.duration_ready = true;
                }
            }
        }
        return;
    }
    s_meta_task = xTaskCreateStaticPinnedToCore(
        DurationFillTask, "rec_meta", kMetaTaskStack, nullptr,
        tskIDLE_PRIORITY + 1, s_meta_stack, s_meta_tcb, 0);
    if (s_meta_task == nullptr) {
        ESP_LOGW(TAG, "duration fill task start failed");
        ++s_meta_fill_fail_count;
        if (s_meta_fill_fail_count >= 2) {
            for (auto& e : s_files) {
                if (e.format == RecFormat::Opus && !e.duration_ready) {
                    e.duration_sec = 0;
                    e.duration_ready = true;
                }
            }
        }
        return;
    }
    s_meta_fill_fail_count = 0;
}

void CollectRecordings() {
    s_files.clear();
    if (!SdCardManager::GetInstance().IsMounted()) {
        return;
    }
    DIR* dir = opendir(kPosixDir);
    if (dir == nullptr) {
        return;
    }
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        if (ent->d_type == DT_DIR) {
            continue;
        }
        if (!IsRecordingFilename(ent->d_name)) {
            continue;
        }
        // d_name 最长 255，直接 snprintf 会触发 -Werror=format-truncation
        if (std::strlen(ent->d_name) >= sizeof(FileEntry{}.name)) {
            continue;
        }
        FileEntry entry = {};
        strlcpy(entry.name, ent->d_name, sizeof(entry.name));
        const int n = std::snprintf(entry.path, sizeof(entry.path), "%s/%s",
                                    kPosixDir, entry.name);
        if (n < 0 || static_cast<size_t>(n) >= sizeof(entry.path)) {
            continue;
        }
        entry.format = HasExt(entry.name, ".opus") ? RecFormat::Opus : RecFormat::Wav;
        struct stat st;
        if (stat(entry.path, &st) == 0) {
            if (!S_ISREG(st.st_mode)) {
                continue;
            }
            entry.size_bytes = static_cast<size_t>(st.st_size);
            if (entry.format == RecFormat::Opus && st.st_size > 0 &&
                !VerifyOpusFileHeader(entry.path)) {
                ESP_LOGW(TAG, "corrupt opus in list: %s size=%ld",
                         entry.name, static_cast<long>(st.st_size));
            }
            if (st.st_size == 0) {
                entry.duration_sec = 0;
                entry.duration_ready = true;
            } else if (entry.format == RecFormat::Opus) {
                const int side = ReadDurationSidecar(entry.path);
                if (side >= 0) {
                    entry.duration_sec = side;
                    entry.duration_ready = true;
                } else {
                    entry.duration_sec = 0;
                    entry.duration_ready = false;
                }
            } else if (st.st_size > 44) {
                entry.duration_sec =
                    static_cast<int>((st.st_size - 44) / (kSampleRate * 2));
                if (entry.duration_sec < 0) {
                    entry.duration_sec = 0;
                }
                entry.duration_ready = true;
            } else {
                entry.duration_sec = 0;
                entry.duration_ready = true;
            }
        } else {
            entry.size_bytes = 0;
            entry.duration_sec = 0;
            entry.duration_ready = true;
        }
        s_files.push_back(entry);
        if (s_files.size() >= static_cast<size_t>(kMaxListItems)) {
            break;
        }
    }
    closedir(dir);

    std::sort(s_files.begin(), s_files.end(),
              [](const FileEntry& a, const FileEntry& b) {
                  return std::strcmp(a.name, b.name) > 0;
              });
}

// 文件头可能被 SD 脏数据覆盖时，在前 64KB 内搜索 OggS 页同步字。
bool SeekToOggStart(FILE* file, long* out_offset = nullptr) {
    if (file == nullptr) {
        return false;
    }
    rewind(file);
    constexpr long kScanMax = 64 * 1024;
    std::vector<uint8_t> buf(4096);
    long base = 0;
    while (base < kScanMax) {
        const size_t n = fread(buf.data(), 1, buf.size(), file);
        if (n < 4) {
            break;
        }
        for (size_t i = 0; i + 4 <= n; ++i) {
            if (std::memcmp(buf.data() + i, "OggS", 4) == 0) {
                const long off = base + static_cast<long>(i);
                if (fseek(file, off, SEEK_SET) != 0) {
                    return false;
                }
                if (out_offset != nullptr) {
                    *out_offset = off;
                }
                if (off > 0) {
                    ESP_LOGW(TAG, "ogg sync at offset %ld", off);
                }
                return true;
            }
        }
        if (n < buf.size()) {
            break;
        }
        base += static_cast<long>(n) - 3;
        if (fseek(file, base, SEEK_SET) != 0) {
            break;
        }
    }
    rewind(file);
    return false;
}

// 播放前先读首包 Ogg 页，预热 SD 并确认 OpusHead（不占用 I2S）。
bool ProbeOpusHeadRate(FILE* file, int& stream_rate) {
    if (file == nullptr) {
        return false;
    }
    long ogg_off = 0;
    if (!SeekToOggStart(file, &ogg_off)) {
        long fsize = -1;
        if (fseek(file, 0, SEEK_END) == 0) {
            fsize = ftell(file);
        }
        rewind(file);
        uint8_t peek[16] = {};
        fread(peek, 1, sizeof(peek), file);
        ESP_LOGE(TAG,
                 "playback probe: OggS not found size=%ld head=%02x%02x%02x%02x "
                 "%02x%02x%02x%02x",
                 fsize, peek[0], peek[1], peek[2], peek[3], peek[4], peek[5],
                 peek[6], peek[7]);
        return false;
    }
    uint8_t hdr[27];
    const size_t got = fread(hdr, 1, 27, file);
    if (got != 27 || std::memcmp(hdr, "OggS", 4) != 0) {
        ESP_LOGE(TAG, "playback probe: page header read failed off=%ld", ogg_off);
        rewind(file);
        return false;
    }
    const uint8_t nseg = hdr[26];
    uint8_t segs[255];
    if (nseg == 0 || fread(segs, 1, nseg, file) != nseg) {
        ESP_LOGE(TAG, "playback probe: short ogg page");
        rewind(file);
        return false;
    }
    size_t body_size = 0;
    for (uint8_t i = 0; i < nseg; ++i) {
        body_size += segs[i];
    }
    std::vector<uint8_t> body(body_size);
    if (body_size > 0 && fread(body.data(), 1, body_size, file) != body_size) {
        ESP_LOGE(TAG, "playback probe: body read failed");
        rewind(file);
        return false;
    }
    size_t body_off = 0;
    size_t seg_idx = 0;
    while (seg_idx < nseg) {
        size_t pkt_len = 0;
        const size_t pkt_start = body_off;
        bool continued = false;
        do {
            const uint8_t l = segs[seg_idx++];
            pkt_len += l;
            body_off += l;
            continued = (l == 255);
        } while (continued && seg_idx < nseg);
        if (pkt_len == 0) {
            continue;
        }
        const uint8_t* pkt = body.data() + pkt_start;
        if (pkt_len >= 19 && std::memcmp(pkt, "OpusHead", 8) == 0) {
            stream_rate = static_cast<int>(ReadLe32(pkt + 12));
            if (stream_rate <= 0) {
                stream_rate = kSampleRate;
            }
            ESP_LOGI(TAG, "playback probe head rate=%d", stream_rate);
            if (ogg_off > 0) {
                fseek(file, ogg_off, SEEK_SET);
            } else {
                rewind(file);
            }
            return true;
        }
    }
    ESP_LOGE(TAG, "playback probe: OpusHead missing");
    if (ogg_off > 0) {
        fseek(file, ogg_off, SEEK_SET);
    } else {
        rewind(file);
    }
    return false;
}

bool PlayOpusFile(FILE* file, AudioCodec* codec, AudioService& as, uint32_t play_gen) {
    if (codec == nullptr) {
        return false;
    }
    if (!SeekToOggStart(file)) {
        return false;
    }

    const int out_rate = codec->output_sample_rate();
    int stream_rate = kSampleRate;
    bool seen_head = false;
    bool seen_tags = false;
    bool decoder_ready = false;
    int packets_ok = 0;
    int packets_fail = 0;
    uint8_t hdr[27];

    std::unique_ptr<OpusDecoderWrapper> decoder;
    OpusResampler resampler;

    auto aborted = [&]() {
        return s_stop_play.load() || play_gen != s_play_gen.load();
    };

    auto ensure_decoder = [&]() -> bool {
        if (decoder_ready) {
            return true;
        }
        decoder = std::make_unique<OpusDecoderWrapper>(stream_rate, 1, kFrameDurationMs);
        if (stream_rate != out_rate) {
            resampler.Configure(stream_rate, out_rate);
            if (!resampler.IsConfigured()) {
                ESP_LOGE(TAG, "local resampler %d -> %d failed", stream_rate, out_rate);
                return false;
            }
        } else {
            resampler.Reset();
        }
        decoder_ready = true;
        return true;
    };

    while (!aborted()) {
        if (fread(hdr, 1, 27, file) != 27) {
            break;
        }
        if (std::memcmp(hdr, "OggS", 4) != 0) {
            return false;
        }
        const uint8_t nseg = hdr[26];
        uint8_t segs[255];
        if (nseg == 0 || fread(segs, 1, nseg, file) != nseg) {
            break;
        }

        size_t body_off = 0;
        size_t body_size = 0;
        for (uint8_t i = 0; i < nseg; ++i) {
            body_size += segs[i];
        }
        std::vector<uint8_t> body(body_size);
        if (body_size > 0 && fread(body.data(), 1, body_size, file) != body_size) {
            break;
        }

        size_t seg_idx = 0;
        while (seg_idx < nseg && !aborted()) {
            size_t pkt_len = 0;
            const size_t pkt_start = body_off;
            bool continued = false;
            do {
                const uint8_t l = segs[seg_idx++];
                pkt_len += l;
                body_off += l;
                continued = (l == 255);
            } while (continued && seg_idx < nseg);

            if (pkt_len == 0) {
                continue;
            }
            const uint8_t* pkt = body.data() + pkt_start;

            if (!seen_head) {
                if (pkt_len >= 19 && std::memcmp(pkt, "OpusHead", 8) == 0) {
                    seen_head = true;
                    stream_rate = static_cast<int>(ReadLe32(pkt + 12));
                    if (stream_rate <= 0) {
                        stream_rate = kSampleRate;
                    }
                    ESP_LOGI(TAG, "playback opus head rate=%d", stream_rate);
                }
                continue;
            }
            if (!seen_tags) {
                if (pkt_len >= 8 && std::memcmp(pkt, "OpusTags", 8) == 0) {
                    seen_tags = true;
                }
                continue;
            }

            if (!ensure_decoder()) {
                return false;
            }
            std::vector<uint8_t> opus_pkt(pkt, pkt + pkt_len);
            std::vector<int16_t> pcm;
            if (!decoder->Decode(std::move(opus_pkt), pcm) || pcm.empty()) {
                ++packets_fail;
                continue;
            }
            std::vector<int16_t> out_pcm;
            if (stream_rate != out_rate && resampler.IsConfigured()) {
                const int out_samples =
                    resampler.GetOutputSamples(static_cast<int>(pcm.size()));
                if (out_samples <= 0) {
                    ++packets_fail;
                    continue;
                }
                out_pcm.resize(static_cast<size_t>(out_samples));
                resampler.Process(pcm.data(), static_cast<int>(pcm.size()), out_pcm.data());
            } else {
                out_pcm = std::move(pcm);
            }
            if (!codec->output_enabled()) {
                codec->EnableOutput(true);
            }
            as.NotifyExternalPlayback();
            codec->OutputData(out_pcm);
            ++packets_ok;
        }

        if (hdr[5] & 0x04) {
            break;
        }
    }

    if (!seen_head) {
        ESP_LOGE(TAG, "playback opus: OpusHead not found");
    }
    ESP_LOGI(TAG, "playback opus ok=%d fail=%d", packets_ok, packets_fail);
    return packets_ok > 0 && !aborted();
}

bool PlayWavFile(FILE* file, AudioCodec* codec) {
    uint8_t hdr[44] = {};
    if (fread(hdr, 1, sizeof(hdr), file) != sizeof(hdr) ||
        std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) {
        PostPlayStatusFromWorker(I18n::T("录音文件损坏"));
        return false;
    }
    if (codec == nullptr) {
        PostPlayStatusFromWorker(I18n::T("音频未就绪"));
        return false;
    }
    if (!codec->output_enabled()) {
        codec->EnableOutput(true);
    }
    PostPlayStatusFromWorker(I18n::T("播放中…"));

    std::vector<int16_t> pcm(static_cast<size_t>(kPlayChunkSamples));
    while (!s_stop_play.load()) {
        const size_t want = pcm.size() * sizeof(int16_t);
        const size_t got = fread(pcm.data(), 1, want, file);
        if (got < sizeof(int16_t)) {
            break;
        }
        const size_t samples = got / sizeof(int16_t);
        pcm.resize(samples);
        codec->OutputData(pcm);
        pcm.resize(static_cast<size_t>(kPlayChunkSamples));
    }
    return true;
}

bool SpawnPlayTask(PlayTaskArgs* args) {
    constexpr uint32_t kPlayTaskStack = 28 * 1024;
    s_play_task = nullptr;

    if (s_play_stack == nullptr) {
        s_play_stack = static_cast<StackType_t*>(heap_caps_malloc(
            kPlayTaskStack, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (s_play_tcb == nullptr) {
        s_play_tcb = static_cast<StaticTask_t*>(heap_caps_malloc(
            sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (s_play_stack != nullptr && s_play_tcb != nullptr) {
        s_play_task = xTaskCreateStaticPinnedToCore(
            PlayTask, "rec_play", kPlayTaskStack, args, tskIDLE_PRIORITY + 2,
            s_play_stack, s_play_tcb, 0);
        if (s_play_task != nullptr) {
            return true;
        }
    }

    if (xTaskCreatePinnedToCore(PlayTask, "rec_play", kPlayTaskStack, args,
                                tskIDLE_PRIORITY + 2, &s_play_task, 0) == pdPASS) {
        return true;
    }

    ESP_LOGE(TAG, "play task start failed");
    return false;
}

void PlayTask(void* arg) {
    auto* args = static_cast<PlayTaskArgs*>(arg);
    if (args == nullptr) {
        s_play_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    const uint32_t my_gen = args->gen;
    const bool is_opus = HasExt(args->path, ".opus");
    FILE* file = fopen(args->path, "rb");
    heap_caps_free(args);
    args = nullptr;

    auto& as = Application::GetInstance().GetAudioService();
    AudioCodec* codec = Board::GetInstance().GetAudioCodec();

    if (file == nullptr) {
        FailPlaybackFromWorker(my_gen, I18n::T("无法打开录音文件"));
        vTaskDelete(nullptr);
        return;
    }

    int probed_rate = kSampleRate;
    if (is_opus && !ProbeOpusHeadRate(file, probed_rate)) {
        fclose(file);
        FailPlaybackFromWorker(my_gen, I18n::T("录音文件损坏"));
        vTaskDelete(nullptr);
        return;
    }

    if (my_gen != s_play_gen.load() || s_stop_play.load()) {
        fclose(file);
        FailPlaybackFromWorker(my_gen, nullptr);
        vTaskDelete(nullptr);
        return;
    }

    if (!PrepareAudioForPlayback(as, codec)) {
        fclose(file);
        ReleaseAudioAfterPlayback(as);
        FailPlaybackFromWorker(my_gen, I18n::T("音频未就绪"));
        vTaskDelete(nullptr);
        return;
    }

    if (my_gen == s_play_gen.load()) {
        s_state.store(RecState::Playing);
        PostPlayStatusFromWorker(I18n::T("播放中…"));
    }

    ESP_LOGI(TAG, "playback start opus=%d path=%s gen=%u rate=%d",
             is_opus ? 1 : 0, s_playing_path, static_cast<unsigned>(my_gen),
             probed_rate);
    bool played_ok = false;
    if (is_opus) {
        played_ok = PlayOpusFile(file, codec, as, my_gen);
    } else {
        played_ok = PlayWavFile(file, codec);
    }
    fclose(file);

    ReleaseAudioAfterPlayback(as);

    if (my_gen != s_play_gen.load()) {
        s_playing_path[0] = '\0';
        s_state.store(RecState::Idle);
        s_play_task = nullptr;
        Application::GetInstance().Schedule([]() {
            if (s_screen_alive) {
                UpdateDetailPlayButton();
                UpdateRecordButtonUi();
            }
        });
        vTaskDelete(nullptr);
        return;
    }

    s_playing_path[0] = '\0';
    s_state.store(RecState::Idle);
    const bool stopped = s_stop_play.load();
    s_stop_play.store(false);
    if (played_ok && !stopped) {
        PostPlayStatusFromWorker(I18n::T("播放结束"));
    } else if (stopped) {
        PostPlayStatusFromWorker(I18n::T("已停止播放"));
    } else {
        PostPlayStatusFromWorker(I18n::T("播放失败"));
    }
    Application::GetInstance().Schedule([]() {
        if (s_screen_alive) {
            UpdateRecordButtonUi();
            UpdateDetailPlayButton();
        }
    });
    s_play_task = nullptr;
    vTaskDelete(nullptr);
}

void StopPlayback() {
    ++s_play_gen;
    const bool had_task = s_play_task != nullptr;
    const bool was_active =
        had_task || s_state.load() == RecState::Playing ||
        s_playing_path[0] != '\0';
    if (had_task) {
        s_stop_play.store(true);
        for (int i = 0; i < 50 && s_play_task != nullptr; ++i) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    s_playing_path[0] = '\0';
    s_state.store(RecState::Idle);
    s_stop_play.store(false);
    if (was_active) {
        ReleaseAudioAfterPlayback(Application::GetInstance().GetAudioService());
    }
    Application::GetInstance().Schedule([]() {
        if (s_screen_alive) {
            UpdateDetailPlayButton();
            UpdateRecordButtonUi();
        }
    });
}

void StartPlayback(const char* path) {
    if (!s_sd_ready || path == nullptr || path[0] == '\0') {
        return;
    }
    auto set_status = [](const char* text) {
        if (s_detail_open) {
            SetDetailStatusText(text);
        } else {
            SetListStatusText(text);
        }
    };
    if (s_state.load() == RecState::Recording || s_state.load() == RecState::Saving) {
        set_status(I18n::T("请先结束录音"));
        return;
    }

    StopPlayback();
    if (s_play_task != nullptr) {
        set_status(I18n::T("请稍候"));
        return;
    }

    auto* args = static_cast<PlayTaskArgs*>(
        heap_caps_malloc(sizeof(PlayTaskArgs), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (args == nullptr) {
        set_status(I18n::T("内存不足"));
        return;
    }
    std::strncpy(args->path, path, sizeof(args->path) - 1);
    args->path[sizeof(args->path) - 1] = '\0';
    args->gen = s_play_gen.load();
    std::snprintf(s_playing_path, sizeof(s_playing_path), "%s", path);

    s_stop_play.store(false);
    set_status(I18n::T("准备播放…"));

    if (!SpawnPlayTask(args)) {
        heap_caps_free(args);
        s_play_task = nullptr;
        s_playing_path[0] = '\0';
        s_state.store(RecState::Idle);
        UpdateRecordButtonUi();
        UpdateDetailPlayButton();
        set_status(I18n::T("播放任务启动失败"));
    }
}

int EventIndex(lv_event_t* e) {
    return static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
}

void UpdateDetailPlayButton() {
    if (!s_screen_alive || s_ui.detail_play_lbl == nullptr) {
        return;
    }
    const bool playing_this =
        s_state.load() == RecState::Playing && s_detail_path[0] != '\0' &&
        std::strcmp(s_playing_path, s_detail_path) == 0;
    lv_label_set_text(s_ui.detail_play_lbl,
                      playing_this ? I18n::T("停止播放") : I18n::T("播放"));
}

void OnDetailPlayClicked(lv_event_t* /*e*/) {
    if (s_detail_path[0] == '\0') {
        return;
    }
    const bool playing_this =
        s_state.load() == RecState::Playing &&
        std::strcmp(s_playing_path, s_detail_path) == 0;
    const bool play_busy = s_play_task != nullptr;
    if (playing_this || play_busy) {
        StopPlayback();
        SetDetailStatusText(I18n::T("已停止播放"));
        UpdateDetailPlayButton();
        UpdateRecordButtonUi();
        return;
    }
    StartPlayback(s_detail_path);
}

void AppendJsonStringArray(std::string& out, cJSON* arr, const char* title) {
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) <= 0) {
        return;
    }
    out += title;
    out += "\n";
    const int n = cJSON_GetArraySize(arr);
    for (int i = 0; i < n; ++i) {
        cJSON* item = cJSON_GetArrayItem(arr, i);
        if (cJSON_IsString(item) && item->valuestring != nullptr &&
            item->valuestring[0] != '\0') {
            out += item->valuestring;
            out += "\n";
        }
    }
    out += "\n";
}

std::string FormatCompletedAsrText(cJSON* data) {
    std::string out;
    cJSON* text = cJSON_GetObjectItemCaseSensitive(data, "asrText");
    if (cJSON_IsString(text) && text->valuestring != nullptr &&
        text->valuestring[0] != '\0') {
        out = text->valuestring;
    }

    cJSON* duration = cJSON_GetObjectItemCaseSensitive(data, "durationMs");
    if (cJSON_IsNumber(duration) && duration->valuedouble > 0) {
        char line[64];
        std::snprintf(line, sizeof(line), I18n::T("音频时长：%.1f 秒"),
                      duration->valuedouble / 1000.0);
        if (!out.empty()) {
            out += "\n\n";
        }
        out += line;
    }

    AppendJsonStringArray(out, cJSON_GetObjectItemCaseSensitive(data, "summary"),
                          I18n::T("摘要"));

    if (out.empty()) {
        out = I18n::T("转写成功，但没有可显示的内容");
    }
    return out;
}

void ApplyAsrRecordData(cJSON* data) {
    if (!cJSON_IsObject(data)) {
        return;
    }
    cJSON* status = cJSON_GetObjectItemCaseSensitive(data, "status");
    if (!cJSON_IsString(status) || status->valuestring == nullptr) {
        return;
    }
    const char* st = status->valuestring;
    if (std::strcmp(st, "transcribing") == 0) {
        PostDetailResultFromWorker(I18n::T("转写处理中，请稍后"));
        PostDetailStatusFromWorker(I18n::T("转写处理中，请稍后"));
        return;
    }
    if (std::strcmp(st, "completed") == 0) {
        const std::string formatted = FormatCompletedAsrText(data);
        PostDetailResultFromWorker(formatted.c_str());
        PostDetailStatusFromWorker(I18n::T("转写完成"));
        return;
    }
    if (std::strcmp(st, "failed") == 0) {
        cJSON* err = cJSON_GetObjectItemCaseSensitive(data, "errorMessage");
        if (cJSON_IsString(err) && err->valuestring != nullptr &&
            err->valuestring[0] != '\0') {
            PostDetailResultFromWorker(err->valuestring);
            PostDetailStatusFromWorker(I18n::T("转写失败"));
        } else {
            PostDetailStatusFromWorker(I18n::T("转写失败"));
        }
    }
    // 其它状态：静默忽略
}

void AsrQueryTask(void* /*arg*/) {
    char name_snapshot[96] = {};
    strlcpy(name_snapshot, s_detail_name, sizeof(name_snapshot));
    if (name_snapshot[0] == '\0') {
        s_asr_query_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        s_asr_query_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        s_asr_query_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    const std::string url = api::AsrAudioRecordsUrl(name_snapshot);
    const std::string device_id = SystemInfo::GetMacAddress();
    api::LogHttpRequest(TAG, "GET", url, std::string(), "ASR audio-records");
    http->SetHeader("Accept", "*/*");
    http->SetHeader("Connection", "close");
    http->SetHeader("X-Device-Id", device_id.c_str());

    if (!http->Open("GET", url)) {
        http->Close();
        s_asr_query_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    const int status = http->GetStatusCode();
    const std::string resp = http->ReadAll();
    http->Close();
    api::LogHttpResponse(TAG, status, api::RedactClawUrlsForLog(resp));

    if (s_stop_asr.load() || !s_detail_open ||
        std::strcmp(name_snapshot, s_detail_name) != 0) {
        s_asr_query_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    // 无记录 / 请求失败：不改动 UI（「没有就不管」）
    if (status != 200 || resp.empty()) {
        s_asr_query_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    cJSON* root = cJSON_Parse(resp.c_str());
    if (root == nullptr) {
        s_asr_query_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    cJSON* code = cJSON_GetObjectItemCaseSensitive(root, "code");
    cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (cJSON_IsNumber(code) && code->valueint == 200 && cJSON_IsObject(data)) {
        ApplyAsrRecordData(data);
    }
    cJSON_Delete(root);

    const bool need_again =
        !s_stop_asr.load() && s_detail_open && s_detail_name[0] != '\0' &&
        std::strcmp(name_snapshot, s_detail_name) != 0;
    s_asr_query_task = nullptr;
    if (need_again) {
        ScheduleAsrQuery();
    }
    vTaskDelete(nullptr);
}

void ScheduleAsrQuery() {
    if (!s_screen_alive || !s_detail_open || s_detail_name[0] == '\0') {
        return;
    }
    if (s_asr_query_task != nullptr) {
        return;
    }
    // 查询结果需联网；未连 WiFi 时静默跳过（不弹窗，本地详情仍可用）。
    if (WifiRequired_ShouldBlock()) {
        return;
    }
    s_stop_asr.store(false);
    if (xTaskCreatePinnedToCore(AsrQueryTask, "rec_asr_q", 8 * 1024, nullptr,
                                tskIDLE_PRIORITY + 1, &s_asr_query_task,
                                0) != pdPASS) {
        s_asr_query_task = nullptr;
        ESP_LOGW(TAG, "asr query task start failed");
    }
}

void AsrUploadTask(void* /*arg*/) {
    FILE* file = fopen(s_detail_path, "rb");
    if (file == nullptr) {
        PostDetailStatusFromWorker(I18n::T("无法打开录音文件"));
        s_asr_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        PostDetailStatusFromWorker(I18n::T("读取录音失败"));
        s_asr_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    const long file_size = ftell(file);
    if (file_size <= 0 || file_size > 8 * 1024 * 1024) {
        fclose(file);
        PostDetailStatusFromWorker(I18n::T("录音文件过大或无效"));
        s_asr_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    rewind(file);

    auto* file_buf = static_cast<uint8_t*>(
        heap_caps_malloc(static_cast<size_t>(file_size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (file_buf == nullptr) {
        file_buf = static_cast<uint8_t*>(
            heap_caps_malloc(static_cast<size_t>(file_size), MALLOC_CAP_8BIT));
    }
    if (file_buf == nullptr) {
        fclose(file);
        PostDetailStatusFromWorker(I18n::T("内存不足"));
        s_asr_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    const size_t got = fread(file_buf, 1, static_cast<size_t>(file_size), file);
    fclose(file);
    if (got != static_cast<size_t>(file_size)) {
        heap_caps_free(file_buf);
        PostDetailStatusFromWorker(I18n::T("读取录音失败"));
        s_asr_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    if (s_stop_asr.load()) {
        heap_caps_free(file_buf);
        s_asr_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        heap_caps_free(file_buf);
        PostDetailStatusFromWorker(I18n::T("网络不可用"));
        s_asr_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        heap_caps_free(file_buf);
        PostDetailStatusFromWorker(I18n::T("网络不可用"));
        s_asr_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    const std::string boundary = "----XingzhiAsrBoundary";
    const char* fname = s_detail_name[0] != '\0' ? s_detail_name : "recording.opus";
    std::string body;
    body.reserve(static_cast<size_t>(file_size) + 256);
    body += "--";
    body += boundary;
    body += "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"";
    body += fname;
    body += "\"\r\nContent-Type: application/octet-stream\r\n\r\n";
    body.append(reinterpret_cast<const char*>(file_buf), static_cast<size_t>(file_size));
    heap_caps_free(file_buf);
    file_buf = nullptr;
    body += "\r\n--";
    body += boundary;
    body += "--\r\n";

    const std::string url = api::Url(api::kAsrTranscribe);
    const std::string device_id = SystemInfo::GetMacAddress();
    api::LogHttpBinaryRequest(TAG, "POST", url, body.size(), "ASR transcribe");

    http->SetContent(std::move(body));
    http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    http->SetHeader("Accept", "*/*");
    http->SetHeader("Connection", "close");
    http->SetHeader("X-Device-Id", device_id.c_str());

    PostDetailStatusFromWorker(I18n::T("正在上传…"));
    if (!http->Open("POST", url)) {
        PostDetailStatusFromWorker(I18n::T("转写请求失败"));
        s_asr_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    const int status = http->GetStatusCode();
    const std::string resp = http->ReadAll();
    http->Close();
    api::LogHttpResponse(TAG, status, api::RedactClawUrlsForLog(resp));

    if (s_stop_asr.load() || !s_detail_open) {
        s_asr_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    if (status != 200) {
        char err[64];
        std::snprintf(err, sizeof(err), I18n::T("转写失败（HTTP %d）"), status);
        PostDetailStatusFromWorker(err);
        s_asr_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    cJSON* root = cJSON_Parse(resp.c_str());
    if (root == nullptr) {
        PostDetailStatusFromWorker(I18n::T("转写结果解析失败"));
        s_asr_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    cJSON* code = cJSON_GetObjectItemCaseSensitive(root, "code");
    cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsNumber(code) || code->valueint != 200 || !cJSON_IsObject(data)) {
        cJSON* msg = cJSON_GetObjectItemCaseSensitive(root, "message");
        if (cJSON_IsString(msg) && msg->valuestring != nullptr) {
            PostDetailStatusFromWorker(msg->valuestring);
        } else {
            PostDetailStatusFromWorker(I18n::T("转写失败"));
        }
        cJSON_Delete(root);
        s_asr_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    cJSON_Delete(root);
    PostDetailResultFromWorker(I18n::T("转写处理中，请稍后"));
    PostDetailStatusFromWorker(
        I18n::T("上传成功，请稍后来查看转写结果，勿反复上传"));
    s_asr_task = nullptr;
    vTaskDelete(nullptr);
}

void OnDetailAsrClicked(lv_event_t* /*e*/) {
    if (s_detail_path[0] == '\0') {
        return;
    }
    if (s_asr_task != nullptr) {
        SetDetailStatusText(I18n::T("正在上传…"));
        return;
    }
    if (s_state.load() == RecState::Recording || s_state.load() == RecState::Saving) {
        SetDetailStatusText(I18n::T("请先结束录音"));
        return;
    }
    // 转写需联网：WiFi 模式未连接时弹窗拦截（本地录音/播放不受影响）。
    if (WifiRequired_ShouldBlock()) {
        WifiRequired_ShowDialog("请先连接 WiFi 后再使用转写");
        return;
    }

    s_stop_asr.store(false);
    SetDetailStatusText(I18n::T("正在上传…"));
    if (xTaskCreatePinnedToCore(AsrUploadTask, "rec_asr", 16 * 1024, nullptr,
                                tskIDLE_PRIORITY + 2, &s_asr_task, 0) != pdPASS) {
        s_asr_task = nullptr;
        SetDetailStatusText(I18n::T("转写任务启动失败"));
    }
}

void OnDetailBackClicked(lv_event_t* /*e*/) { HideDetail(); }

bool DeleteRecordingAt(int idx) {
    if (idx < 0 || idx >= static_cast<int>(s_files.size())) {
        return false;
    }
    const FileEntry& entry = s_files[static_cast<size_t>(idx)];
    const char* path = entry.path;

    if (s_detail_open && s_detail_idx == idx) {
        s_stop_asr.store(true);
    }
    if (s_state.load() == RecState::Playing &&
        std::strcmp(s_playing_path, path) == 0) {
        StopPlayback();
    }
    if (unlink(path) != 0) {
        ESP_LOGE(TAG, "delete failed: %s", path);
        if (s_detail_open && s_detail_idx == idx) {
            SetDetailStatusText(I18n::T("删除失败"));
        } else {
            SetListStatusText(I18n::T("删除失败"));
        }
        return false;
    }
    if (entry.format == RecFormat::Opus) {
        UnlinkDurationSidecar(path);
    }
    ESP_LOGI(TAG, "deleted: %s", path);

    if (s_detail_open && s_detail_idx == idx) {
        HideDetail();
        SetListStatusText(I18n::T("已删除"));
    } else {
        SetListStatusText(I18n::T("已删除"));
    }
    RebuildFileList();
    return true;
}

void OnDetailDeleteClicked(lv_event_t* /*e*/) {
    if (!s_detail_open || s_detail_idx < 0) {
        return;
    }
    if (s_state.load() == RecState::Recording || s_state.load() == RecState::Saving) {
        SetDetailStatusText(I18n::T("请先结束录音"));
        return;
    }
    DeleteRecordingAt(s_detail_idx);
}

void HideDetail() {
    if (!s_detail_open) {
        return;
    }
    if (s_state.load() == RecState::Playing &&
        std::strcmp(s_playing_path, s_detail_path) == 0) {
        StopPlayback();
    }
    s_stop_asr.store(true);
    s_detail_open = false;
    s_detail_idx = -1;
    s_detail_path[0] = '\0';
    s_detail_name[0] = '\0';
    if (s_ui.detail_panel != nullptr) {
        lv_obj_add_flag(s_ui.detail_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

void ShowDetail(int idx) {
    if (idx < 0 || idx >= static_cast<int>(s_files.size()) ||
        s_ui.detail_panel == nullptr) {
        return;
    }
    if (s_state.load() == RecState::Playing) {
        StopPlayback();
    }
    FileEntry& entry = s_files[static_cast<size_t>(idx)];
    s_detail_idx = idx;
    s_detail_open = true;
    strlcpy(s_detail_path, entry.path, sizeof(s_detail_path));
    strlcpy(s_detail_name, entry.name, sizeof(s_detail_name));

    if (s_ui.detail_title != nullptr) {
        char title_buf[48];
        if (kRoundLayout) {
            FormatListTitle(entry.name, title_buf, sizeof(title_buf));
        } else {
            strlcpy(title_buf, entry.name, sizeof(title_buf));
        }
        lv_label_set_text(s_ui.detail_title, title_buf);
    }
    if (s_ui.detail_meta != nullptr) {
        char dur[16];
        char size_buf[24];
        FormatTimer(dur, sizeof(dur), entry.duration_sec);
        FormatFileSize(size_buf, sizeof(size_buf), entry.size_bytes);
        char meta[48];
        if (kRoundLayout) {
            std::snprintf(meta, sizeof(meta), I18n::T("%s · %s"), dur, size_buf);
        } else {
            std::snprintf(meta, sizeof(meta), I18n::T("时长 %s · %s"), dur, size_buf);
        }
        lv_label_set_text(s_ui.detail_meta, meta);
    }
    SetDetailStatusText("");
    SetDetailResultText(I18n::T("点击「录音转写」上传，稍后再进详情查看结果"));
    UpdateDetailPlayButton();
    lv_obj_remove_flag(s_ui.detail_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ui.detail_panel);
    if (!entry.duration_ready) {
        ScheduleDurationFill();
    }
    ScheduleAsrQuery();
    ESP_LOGI(TAG, "detail open idx=%d path=%s", idx, entry.path);
}

void OnRowClicked(lv_event_t* e) {
    const int idx = EventIndex(e);
    ESP_LOGI(TAG, "list row click idx=%d files=%u", idx,
             static_cast<unsigned>(s_files.size()));
    if (idx < 0 || idx >= static_cast<int>(s_files.size())) {
        return;
    }
    ShowDetail(idx);
}

void OnDeleteClicked(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    const int idx = EventIndex(e);
    ESP_LOGI(TAG, "list delete click idx=%d", idx);
    if (s_state.load() == RecState::Recording || s_state.load() == RecState::Saving) {
        SetListStatusText(I18n::T("请先结束录音"));
        return;
    }
    DeleteRecordingAt(idx);
}

void ClearListChildren() {
    if (s_ui.list_scroll == nullptr) {
        return;
    }
    lv_obj_clean(s_ui.list_scroll);
}

void RefreshListRowMeta() {
    if (!s_screen_alive || s_ui.list_scroll == nullptr) {
        return;
    }
    const uint32_t row_count = lv_obj_get_child_count(s_ui.list_scroll);
    for (uint32_t i = 0; i < row_count && i < s_files.size(); ++i) {
        lv_obj_t* row = lv_obj_get_child(s_ui.list_scroll, i);
        if (row == nullptr) {
            continue;
        }
        lv_obj_t* open = lv_obj_get_child(row, 0);
        if (open == nullptr) {
            continue;
        }
        lv_obj_t* hint = lv_obj_get_child(open, 1);
        if (hint == nullptr) {
            continue;
        }
        const FileEntry& entry = s_files[i];
        char dur[16];
        char size_buf[24];
        if (entry.duration_ready) {
            FormatTimer(dur, sizeof(dur), entry.duration_sec);
        } else {
            std::snprintf(dur, sizeof(dur), "--:--");
        }
        FormatFileSize(size_buf, sizeof(size_buf), entry.size_bytes);
        char hint_buf[48];
        if (kRoundLayout) {
            std::snprintf(hint_buf, sizeof(hint_buf), I18n::T("%s · %s"), dur, size_buf);
        } else {
            std::snprintf(hint_buf, sizeof(hint_buf), I18n::T("时长 %s · %s · 点击查看"),
                          dur, size_buf);
        }
        lv_label_set_text(hint, hint_buf);
    }
}

void RebuildFileList() { RebuildFileList(true); }

void RebuildFileList(bool schedule_fill) {
    if (!s_screen_alive || s_ui.list_scroll == nullptr) {
        return;
    }
    CollectRecordings();
    ClearListChildren();

    if (s_files.empty()) {
        if (s_ui.list_empty != nullptr) {
            lv_obj_remove_flag(s_ui.list_empty, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    if (s_ui.list_empty != nullptr) {
        lv_obj_add_flag(s_ui.list_empty, LV_OBJ_FLAG_HIDDEN);
    }

    for (size_t i = 0; i < s_files.size(); ++i) {
        void* idx_ud = reinterpret_cast<void*>(static_cast<intptr_t>(i));

        lv_obj_t* row = lv_obj_create(s_ui.list_scroll);
        screen_strip_obj_chrome(row);
        lv_obj_set_size(row, lv_pct(100), kListRowH);
        lv_obj_set_style_bg_color(row, lv_color_hex(kColorCard), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(row, kRoundLayout ? 14 : 16, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(row, kRoundLayout ? 12 : 16, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(row, kRoundLayout ? 10 : 8, LV_PART_MAIN);
        lv_obj_set_style_margin_bottom(row, kRoundLayout ? 8 : 12, LV_PART_MAIN);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, kRoundLayout ? 8 : 12, LV_PART_MAIN);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
        screen_swipe_back_ignore(row, false);

        lv_obj_t* open = lv_button_create(row);
        lv_obj_remove_style_all(open);
        lv_obj_set_flex_grow(open, 1);
        lv_obj_set_height(open, kListRowH - (kRoundLayout ? 20 : 16));
        lv_obj_set_style_min_height(open, kListRowH - (kRoundLayout ? 20 : 16),
                                    LV_PART_MAIN);
        lv_obj_set_flex_flow(open, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(open, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(open, kRoundLayout ? 4 : 6, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(open, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_bg_color(open, lv_color_hex(0xFFFFFF),
                                  Sel(LV_PART_MAIN, LV_STATE_PRESSED));
        lv_obj_set_style_bg_opa(open, LV_OPA_10, Sel(LV_PART_MAIN, LV_STATE_PRESSED));
        lv_obj_set_style_radius(open, kRoundLayout ? 10 : 12, LV_PART_MAIN);
        lv_obj_add_event_cb(open, OnRowClicked, LV_EVENT_CLICKED, idx_ud);
        lv_obj_add_flag(open, LV_OBJ_FLAG_CLICKABLE);
        screen_swipe_back_ignore(open, true);

        char title_buf[48];
        if (kRoundLayout) {
            FormatListTitle(s_files[i].name, title_buf, sizeof(title_buf));
        } else {
            strlcpy(title_buf, s_files[i].name, sizeof(title_buf));
        }
        lv_obj_t* name = lv_label_create(open);
        lv_label_set_text(name, title_buf);
        lv_label_set_long_mode(name, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(name, LV_PCT(100));
        lv_obj_set_style_text_color(name, lv_color_hex(kColorText), LV_PART_MAIN);
        lv_obj_set_style_text_font(name, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_remove_flag(name, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* hint = lv_label_create(open);
        char dur[16];
        char size_buf[24];
        if (s_files[i].duration_ready) {
            FormatTimer(dur, sizeof(dur), s_files[i].duration_sec);
        } else {
            std::snprintf(dur, sizeof(dur), "--:--");
        }
        FormatFileSize(size_buf, sizeof(size_buf), s_files[i].size_bytes);
        char hint_buf[48];
        if (kRoundLayout) {
            std::snprintf(hint_buf, sizeof(hint_buf), I18n::T("%s · %s"), dur, size_buf);
        } else {
            std::snprintf(hint_buf, sizeof(hint_buf), I18n::T("时长 %s · %s · 点击查看"),
                          dur, size_buf);
        }
        lv_label_set_text(hint, hint_buf);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(hint, LV_PCT(100));
        lv_obj_set_style_text_color(hint, lv_color_hex(kColorSubtle), LV_PART_MAIN);
        lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_remove_flag(hint, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* del = lv_button_create(row);
        lv_obj_remove_style_all(del);
        lv_obj_set_size(del, kListDelBtnW, kListDelBtnH);
        lv_obj_set_style_radius(del, kRoundLayout ? 10 : 14, LV_PART_MAIN);
        lv_obj_set_style_bg_color(del, lv_color_hex(kColorDanger), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(del, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(del, lv_color_hex(0xB91C1C),
                                  Sel(LV_PART_MAIN, LV_STATE_PRESSED));
        lv_obj_add_event_cb(del, OnDeleteClicked, LV_EVENT_CLICKED, idx_ud);
        lv_obj_add_flag(del, LV_OBJ_FLAG_CLICKABLE);
        screen_swipe_back_ignore(del, true);
        lv_obj_move_foreground(del);

        lv_obj_t* del_lbl = lv_label_create(del);
        lv_label_set_text(del_lbl, I18n::T("删除"));
        lv_obj_set_style_text_color(del_lbl, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_font(del_lbl, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_center(del_lbl);
    }

    if (schedule_fill) {
        Application::GetInstance().Schedule([]() { ScheduleDurationFill(); });
    }
}

void OnTabChanged(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) {
        return;
    }
    if (s_ui.tabview == nullptr) {
        return;
    }
    const uint32_t idx = lv_tabview_get_tab_active(s_ui.tabview);
    if (idx == 1) {
        RebuildFileList();
    }
}

void ForceStopAll() {
    s_stop_record.store(true);
    s_stop_play.store(true);
    s_stop_asr.store(true);
    s_stop_meta.store(true);
    StopTickTimer();
    for (int i = 0; i < 80 &&
         (s_record_task != nullptr || s_play_task != nullptr || s_asr_task != nullptr ||
          s_asr_query_task != nullptr || s_meta_task != nullptr);
         ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_stop_record.store(false);
    s_stop_play.store(false);
    s_stop_asr.store(false);
    s_stop_meta.store(false);
    s_playing_path[0] = '\0';
    s_detail_open = false;
    s_detail_idx = -1;
    s_detail_path[0] = '\0';
    s_detail_name[0] = '\0';
    s_state.store(RecState::Idle);
    RestoreWakeWordIfNeeded();
    auto& as = Application::GetInstance().GetAudioService();
    as.ResetDecoder();
    ReleaseAudioAfterPlayback(as);
}

void BuildDetailPanel(lv_obj_t* parent) {
    lv_obj_t* panel = lv_obj_create(parent);
    s_ui.detail_panel = panel;
    screen_strip_obj_chrome(panel);
    lv_obj_set_size(panel, kPanelSize, kPanelSize);
    lv_obj_set_pos(panel, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
    screen_swipe_back_ignore(panel, true);

    auto make_back_btn = [&](lv_obj_t* parent_btn, int32_t y_ofs) {
        lv_obj_t* back = lv_button_create(parent_btn);
        lv_obj_remove_style_all(back);
        lv_obj_set_size(back, kBackBtnSize, kBackBtnSize);
        if constexpr (kRoundLayout) {
            lv_obj_align(back, LV_ALIGN_TOP_MID, 0, y_ofs);
        } else {
            lv_obj_align(back, LV_ALIGN_LEFT_MID, 16, 0);
        }
        lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_bg_color(back, lv_color_hex(0xFFFFFF),
                                  Sel(LV_PART_MAIN, LV_STATE_PRESSED));
        lv_obj_set_style_bg_opa(back, LV_OPA_20, Sel(LV_PART_MAIN, LV_STATE_PRESSED));
        lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_add_event_cb(back, OnDetailBackClicked, LV_EVENT_CLICKED, nullptr);
        screen_swipe_back_ignore(back, true);

        lv_obj_t* back_icon = lv_image_create(back);
        lv_image_set_src(back_icon, "A:ic_app_back.spng");
        lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(back_icon);
        return back;
    };

    if constexpr (kRoundLayout) {
        make_back_btn(panel, kDetailTopY);

        lv_obj_t* del = lv_button_create(panel);
        s_ui.detail_del_btn = del;
        lv_obj_remove_style_all(del);
        lv_obj_set_size(del, 64, 36);
        lv_obj_align(del, LV_ALIGN_TOP_RIGHT, -24, kDetailTopY);
        lv_obj_set_style_radius(del, 10, LV_PART_MAIN);
        lv_obj_set_style_bg_color(del, lv_color_hex(kColorDanger), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(del, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(del, lv_color_hex(0xB91C1C),
                                  Sel(LV_PART_MAIN, LV_STATE_PRESSED));
        lv_obj_add_event_cb(del, OnDetailDeleteClicked, LV_EVENT_CLICKED, nullptr);
        screen_swipe_back_ignore(del, true);

        lv_obj_t* del_lbl = lv_label_create(del);
        lv_label_set_text(del_lbl, I18n::T("删除"));
        lv_obj_set_style_text_color(del_lbl, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_font(del_lbl, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_center(del_lbl);

        s_ui.detail_title = lv_label_create(panel);
        lv_label_set_text(s_ui.detail_title, "");
        lv_label_set_long_mode(s_ui.detail_title, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(s_ui.detail_title, kDetailContentW);
        lv_obj_set_style_text_color(s_ui.detail_title, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_ui.detail_title, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_align(s_ui.detail_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(s_ui.detail_title, LV_ALIGN_TOP_MID, 0, kDetailTitleY);
    } else {
        lv_obj_t* header = lv_obj_create(panel);
        screen_strip_obj_chrome(header);
        lv_obj_set_size(header, kPanelSize, kHeaderH);
        lv_obj_set_pos(header, 0, 0);
        lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

        make_back_btn(header, 0);

        s_ui.detail_title = lv_label_create(header);
        lv_label_set_text(s_ui.detail_title, "");
        lv_label_set_long_mode(s_ui.detail_title, LV_LABEL_LONG_DOT);
        lv_obj_set_width(s_ui.detail_title, kPanelSize - 16 - kBackBtnSize - 40);
        lv_obj_set_style_text_color(s_ui.detail_title, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_ui.detail_title, &font_puhui_30_4, LV_PART_MAIN);
        lv_obj_align(s_ui.detail_title, LV_ALIGN_LEFT_MID, 16 + kBackBtnSize + 8, 0);
    }

    s_ui.detail_meta = lv_label_create(panel);
    lv_label_set_text(s_ui.detail_meta, "");
    lv_obj_set_style_text_color(s_ui.detail_meta, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.detail_meta, &font_puhui_20_4, LV_PART_MAIN);
    if constexpr (kRoundLayout) {
        lv_obj_set_width(s_ui.detail_meta, kDetailContentW);
        lv_obj_set_style_text_align(s_ui.detail_meta, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_label_set_long_mode(s_ui.detail_meta, LV_LABEL_LONG_CLIP);
        lv_obj_align(s_ui.detail_meta, LV_ALIGN_TOP_MID, 0, kDetailMetaY);
    } else {
        lv_obj_align(s_ui.detail_meta, LV_ALIGN_TOP_MID, 0, kHeaderH + 8);
    }

    // 操作按钮：大屏并排放（play 左 / asr 右）；圆屏改成竖排堆叠。
    lv_obj_t* play = lv_button_create(panel);
    s_ui.detail_play_btn = play;
    lv_obj_remove_style_all(play);
    lv_obj_set_size(play, kDetailBtnW, kDetailBtnH);
    if constexpr (kRoundLayout) {
        lv_obj_align(play, LV_ALIGN_TOP_MID, 0, kDetailPlayY);
    } else {
        lv_obj_align(play, LV_ALIGN_TOP_LEFT, 40, kHeaderH + 48);
    }
    lv_obj_set_style_radius(play, kRoundLayout ? 14 : 20, LV_PART_MAIN);
    lv_obj_set_style_bg_color(play, lv_color_hex(kColorAccent), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(play, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(play, lv_color_hex(0x2563EB),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_add_flag(play, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(play, OnDetailPlayClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(play, true);
    lv_obj_move_foreground(play);

    s_ui.detail_play_lbl = lv_label_create(play);
    lv_label_set_text(s_ui.detail_play_lbl, I18n::T("播放"));
    lv_obj_set_style_text_color(s_ui.detail_play_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.detail_play_lbl, kRoundLayout ? &font_puhui_20_4
                                                                  : &font_puhui_30_4,
                               LV_PART_MAIN);
    lv_obj_center(s_ui.detail_play_lbl);

    lv_obj_t* asr = lv_button_create(panel);
    s_ui.detail_asr_btn = asr;
    lv_obj_remove_style_all(asr);
    lv_obj_set_size(asr, kDetailBtnW, kDetailBtnH);
    if constexpr (kRoundLayout) {
        lv_obj_align(asr, LV_ALIGN_TOP_MID, 0, kDetailAsrY);
    } else {
        lv_obj_align(asr, LV_ALIGN_TOP_RIGHT, -40, kHeaderH + 48);
    }
    lv_obj_set_style_radius(asr, kRoundLayout ? 14 : 20, LV_PART_MAIN);
    lv_obj_set_style_bg_color(asr, lv_color_hex(0x059669), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(asr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(asr, lv_color_hex(0x047857),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_add_event_cb(asr, OnDetailAsrClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(asr, true);

    s_ui.detail_asr_lbl = lv_label_create(asr);
    lv_label_set_text(s_ui.detail_asr_lbl, I18n::T("录音转写"));
    lv_obj_set_style_text_color(s_ui.detail_asr_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.detail_asr_lbl, kRoundLayout ? &font_puhui_20_4
                                                                  : &font_puhui_30_4,
                               LV_PART_MAIN);
    lv_obj_center(s_ui.detail_asr_lbl);

    if constexpr (!kRoundLayout) {
        lv_obj_t* del = lv_button_create(panel);
        s_ui.detail_del_btn = del;
        lv_obj_remove_style_all(del);
        lv_obj_set_size(del, kDetailBtnW, kDetailBtnH);
        lv_obj_align(del, LV_ALIGN_TOP_MID, 0, kDetailDelY);
        lv_obj_set_style_radius(del, 20, LV_PART_MAIN);
        lv_obj_set_style_bg_color(del, lv_color_hex(kColorDanger), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(del, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(del, lv_color_hex(0xB91C1C),
                                  Sel(LV_PART_MAIN, LV_STATE_PRESSED));
        lv_obj_add_event_cb(del, OnDetailDeleteClicked, LV_EVENT_CLICKED, nullptr);
        screen_swipe_back_ignore(del, true);

        lv_obj_t* del_lbl = lv_label_create(del);
        lv_label_set_text(del_lbl, I18n::T("删除"));
        lv_obj_set_style_text_color(del_lbl, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_font(del_lbl, &font_puhui_30_4, LV_PART_MAIN);
        lv_obj_center(del_lbl);
    }

    // 竖排按钮占用的总高度：圆屏两个按钮 + 间隙，大屏为并排一行再加删除行。
    constexpr int32_t kDetailBtnsBlockH =
        kRoundLayout ? (kDetailAsrY + kDetailBtnH - kDetailPlayY)
                     : (kDetailDelY + kDetailBtnH - kHeaderH);
    constexpr int32_t kDetailStatusY =
        kRoundLayout ? (kDetailAsrY + kDetailBtnH + 10)
                     : (kHeaderH + kDetailBtnsBlockH + 16);

    s_ui.detail_status = lv_label_create(panel);
    lv_label_set_text(s_ui.detail_status, "");
    lv_obj_set_width(s_ui.detail_status, kDetailContentW);
    lv_label_set_long_mode(s_ui.detail_status, kRoundLayout ? LV_LABEL_LONG_WRAP
                                                             : LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_ui.detail_status, lv_color_hex(kColorSubtle),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.detail_status, &font_puhui_20_4, LV_PART_MAIN);
    if constexpr (kRoundLayout) {
        lv_obj_set_style_text_align(s_ui.detail_status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    }
    lv_obj_align(s_ui.detail_status, LV_ALIGN_TOP_MID, 0,
                kRoundLayout ? kDetailStatusY : kHeaderH + kDetailBtnsBlockH + 16);
    screen_make_input_passive(s_ui.detail_status);

    // 圆屏给 status 文字预留出约两行的高度（提示语常有较长的中文句子），
    // 再往下才是结果框，避免长状态文案和结果框标题重叠。
    const int32_t result_box_y =
        kRoundLayout ? (kDetailStatusY + 48) : (kHeaderH + kDetailBtnsBlockH + 48);
    lv_obj_t* result_box = lv_obj_create(panel);
    screen_strip_obj_chrome(result_box);
    lv_obj_set_size(result_box, kDetailContentW,
                    kPanelSize - result_box_y - (kRoundLayout ? 24 : 0));
    lv_obj_align(result_box, LV_ALIGN_TOP_MID, 0, result_box_y);
    lv_obj_set_style_bg_color(result_box, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(result_box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(result_box, kRoundLayout ? 14 : 20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(result_box, kRoundLayout ? 12 : 20, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(result_box, LV_SCROLLBAR_MODE_AUTO);
    screen_swipe_back_ignore(result_box, true);

    s_ui.detail_result = lv_label_create(result_box);
    lv_label_set_text(s_ui.detail_result, "");
    lv_obj_set_width(s_ui.detail_result,
                     kDetailContentW - (kRoundLayout ? 24 : 40));
    lv_label_set_long_mode(s_ui.detail_result, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_ui.detail_result, lv_color_hex(kColorText),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.detail_result, &font_puhui_20_4, LV_PART_MAIN);
}

void BuildHeader(lv_obj_t* parent) {
    lv_obj_t* header = lv_obj_create(parent);
    screen_strip_obj_chrome(header);
    lv_obj_set_size(header, kPanelSize, kHeaderH);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    if constexpr (!kRoundLayout) {
        lv_obj_t* back = lv_button_create(header);
        lv_obj_remove_style_all(back);
        lv_obj_set_size(back, kBackBtnSize, kBackBtnSize);
        lv_obj_align(back, LV_ALIGN_LEFT_MID, 16, 0);
        lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_bg_color(back, lv_color_hex(0xFFFFFF),
                                  Sel(LV_PART_MAIN, LV_STATE_PRESSED));
        lv_obj_set_style_bg_opa(back, LV_OPA_20, Sel(LV_PART_MAIN, LV_STATE_PRESSED));
        lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_add_event_cb(back, OnBackClicked, LV_EVENT_CLICKED, nullptr);
        screen_swipe_back_ignore(back, true);

        lv_obj_t* back_icon = lv_image_create(back);
        lv_image_set_src(back_icon, "A:ic_app_back.spng");
        lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(back_icon);
    }

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, I18n::T("录音"));
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, kRoundLayout ? &font_puhui_20_4 : &font_puhui_30_4,
                               LV_PART_MAIN);
    if constexpr (kRoundLayout) {
        const int title_y = kHeaderTopInset + (kHeaderContentH - 20) / 2;
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, title_y);
    } else {
        lv_obj_align(title, LV_ALIGN_LEFT_MID, 16 + kBackBtnSize + 16, 0);
    }
}

void BuildNoSdHint(lv_obj_t* parent) {
    lv_obj_t* hint = lv_label_create(parent);
    lv_label_set_text(hint, I18n::T("未检测到 SD 卡\n\n请插入 SD 卡后再使用录音功能"));
    lv_obj_set_width(hint, kPanelSize - 80);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 20);
    screen_make_input_passive(hint);
}

void BuildRecordTab(lv_obj_t* tab) {
    lv_obj_set_style_bg_opa(tab, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tab, 0, LV_PART_MAIN);
    lv_obj_remove_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* card = lv_obj_create(tab);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, kRecCardW, kRecCardH);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, kRecCardY);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 28, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    screen_make_input_passive(card);

    s_ui.timer_lbl = lv_label_create(card);
    lv_label_set_text(s_ui.timer_lbl, "00:00");
    lv_obj_set_style_text_color(s_ui.timer_lbl, lv_color_hex(0xF87171), LV_PART_MAIN);
    // 用含冒号字形的字体；纯数字字库会把 "00:07" 渲成 "0007"
    lv_obj_set_style_text_font(s_ui.timer_lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(s_ui.timer_lbl, LV_ALIGN_CENTER, 0, kRoundLayout ? -12 : -16);

    s_ui.status_lbl = lv_label_create(card);
    SetIdleRecordHint();
    lv_obj_set_style_text_color(s_ui.status_lbl, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.status_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_label_set_long_mode(s_ui.status_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_ui.status_lbl, kRecStatusW);
    lv_obj_set_style_text_align(s_ui.status_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(s_ui.status_lbl, LV_ALIGN_BOTTOM_MID, 0, kRoundLayout ? -8 : -24);

    s_ui.record_btn = lv_button_create(tab);
    lv_obj_remove_style_all(s_ui.record_btn);
    lv_obj_set_size(s_ui.record_btn, kRecBtnW, kRecBtnH);
    lv_obj_align(s_ui.record_btn, LV_ALIGN_TOP_MID, 0, kRecBtnY);
    lv_obj_set_style_radius(s_ui.record_btn, kRecBtnH / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.record_btn, lv_color_hex(kColorRecord), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.record_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.record_btn, lv_color_hex(0xB91C1C),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_add_event_cb(s_ui.record_btn, OnRecordBtnClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(s_ui.record_btn, true);

    s_ui.record_btn_lbl = lv_label_create(s_ui.record_btn);
    lv_label_set_text(s_ui.record_btn_lbl, I18n::T("开始录音"));
    lv_obj_set_style_text_color(s_ui.record_btn_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.record_btn_lbl, kRoundLayout ? &font_puhui_20_4
                                                                 : &font_puhui_30_4,
                               LV_PART_MAIN);
    lv_obj_center(s_ui.record_btn_lbl);

    lv_obj_t* tip = lv_label_create(tab);
    {
        char tip_buf[96];
        std::snprintf(tip_buf, sizeof(tip_buf),
                      I18n::T("单次最长 %d 分钟 · Opus：/sdcard/recordings"),
                      kMaxRecordSeconds / 60);
        lv_label_set_text(tip, tip_buf);
    }
    lv_obj_set_style_text_color(tip, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(tip, &font_puhui_20_4, LV_PART_MAIN);
    if constexpr (kRoundLayout) {
        lv_label_set_long_mode(tip, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(tip, kPanelSize - 80);
        lv_obj_set_style_text_align(tip, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    }
    lv_obj_align(tip, LV_ALIGN_TOP_MID, 0, kRecTipY);
    screen_make_input_passive(tip);
}

void BuildListTab(lv_obj_t* tab) {
    lv_obj_set_style_bg_opa(tab, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tab, 0, LV_PART_MAIN);
    lv_obj_remove_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.list_status = lv_label_create(tab);
    lv_label_set_text(s_ui.list_status, "");
    lv_obj_set_width(s_ui.list_status, kPanelSize - 48);
    lv_label_set_long_mode(s_ui.list_status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_ui.list_status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.list_status, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.list_status, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(s_ui.list_status, LV_ALIGN_BOTTOM_MID, 0,
                 kRoundLayout ? -28 : -12);
    screen_make_input_passive(s_ui.list_status);

    s_ui.list_empty = lv_label_create(tab);
    lv_label_set_text(s_ui.list_empty, I18n::T("暂无录音"));
    lv_obj_set_style_text_color(s_ui.list_empty, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.list_empty, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(s_ui.list_empty, LV_ALIGN_CENTER, 0, 0);
    screen_make_input_passive(s_ui.list_empty);

    s_ui.list_scroll = lv_obj_create(tab);
    screen_strip_obj_chrome(s_ui.list_scroll);
    lv_obj_set_size(s_ui.list_scroll, kPanelSize - (kRoundLayout ? 80 : 40),
                    kBodyH - kTabBarH - kListBottomReserve);
    lv_obj_align(s_ui.list_scroll, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_set_style_bg_opa(s_ui.list_scroll, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_ui.list_scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ui.list_scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_ui.list_scroll, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(s_ui.list_scroll, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(s_ui.list_scroll, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    screen_swipe_back_ignore(s_ui.list_scroll, false);
}

void BuildTabView(lv_obj_t* parent) {
    lv_obj_t* tv = lv_tabview_create(parent);
    s_ui.tabview = tv;
    lv_obj_set_size(tv, kPanelSize, kBodyH);
    lv_obj_set_pos(tv, 0, kHeaderH);
    lv_tabview_set_tab_bar_position(tv, LV_DIR_TOP);
    lv_tabview_set_tab_bar_size(tv, kTabBarH);

    lv_obj_set_style_bg_color(tv, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tv, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(tv, 0, LV_PART_MAIN);

    lv_obj_t* bar = lv_tabview_get_tab_bar(tv);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColorTabBar), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(bar, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(bar, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 6, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColorAccent),
                              LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(bar, lv_color_hex(kColorText),
                                LV_PART_ITEMS | LV_STATE_CHECKED);

    lv_obj_t* content = lv_tabview_get_content(tv);
    screen_swipe_back_ignore(content, true);
    lv_obj_add_event_cb(tv, OnTabChanged, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t* tab_rec = lv_tabview_add_tab(tv, I18n::T("录音"));
    BuildRecordTab(tab_rec);

    lv_obj_t* tab_list = lv_tabview_add_tab(tv, I18n::T("列表"));
    BuildListTab(tab_list);
}

void OnSwipeBack() {
    if (s_detail_open) {
        HideDetail();
        return;
    }
    if (s_state.load() == RecState::Recording) {
        StopRecording();
    }
    lv_indev_t* indev = lv_indev_active();
    if (indev != nullptr) {
        lv_indev_wait_release(indev);
    }
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* home = HomeScreen::Create();
    lv_screen_load(home);
    if (old_scr != nullptr && old_scr != home) {
        lv_obj_delete_async(old_scr);
    }
}

void OnBackClicked(lv_event_t* /*e*/) { OnSwipeBack(); }

void OnScreenUnloaded(lv_event_t* /*e*/) {
    ForceStopAll();
    s_screen_alive = false;
    s_detail_open = false;
    s_ui = {};
    s_files.clear();
}

}  // namespace

lv_obj_t* RecordingScreen::Create() {
    s_ui = {};
    s_screen_alive = true;
    s_state.store(RecState::Idle);
    s_sd_ready = SdCardManager::GetInstance().IsMounted();

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_ui.screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelSize, kPanelSize);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    BuildHeader(scr);

    if (!s_sd_ready) {
        BuildNoSdHint(scr);
    } else {
        EnsureRecordingsDir();
        BuildTabView(scr);
        BuildDetailPanel(scr);
        RebuildFileList();
    }

    screen_attach_swipe_back(scr, OnSwipeBack);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);
    return scr;
}

void RecordingScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: recording_screen sd=%d", s_sd_ready ? 1 : 0);
        if (s_sd_ready) {
            CleanupStalePartFiles();
            WarnIfSdFsckArtifacts();
        }
    } else {
        ESP_LOGI(TAG, "unload: recording_screen");
        ForceStopAll();
    }
}
