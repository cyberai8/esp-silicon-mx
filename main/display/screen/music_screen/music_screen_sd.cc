#include "music_screen_sd.h"

#include "i18n.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "esp_audio_simple_player.h"
#include "esp_audio_simple_player_advance.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "config.h"
#include "home_screen/home_screen.h"
#include "lv_eaf.h"
#include "screen_util.h"
#include "settings.h"
#include "SdCardManager.hpp"

#include <font_awesome.h>

#ifdef CONFIG_ESP_AUDIO_SIMPLE_PLAYER_RESAMPLE_EN
#include "esp_gmf_pipeline.h"
#include "esp_gmf_rate_cvt.h"
#endif

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_awesome_20_4);

#if defined(BOARD_ESP_VOCAT)

namespace {

constexpr const char* TAG = "MusicScreenSd";
constexpr int kScanMaxDepth = 5;
constexpr size_t kMaxTracks = 300;
constexpr int kVolStep = 5;
constexpr uint32_t kVolToastMs = 1500;

constexpr uint32_t kColorBg = 0x0E1116;
constexpr uint32_t kColorBgGrad = 0x161A22;
constexpr uint32_t kColorText = 0xFFFFFF;
constexpr uint32_t kColorMuted = 0x8B92A3;
constexpr uint32_t kColorAccent = 0xE0FB3C;
constexpr uint32_t kColorBtn = 0x232732;
constexpr uint32_t kColorBtnPressed = 0x303644;
constexpr uint32_t kColorRow = 0x1A1E26;
constexpr uint32_t kColorRowPressed = 0x252A34;
constexpr uint32_t kColorBarTrack = 0x2A2F3A;

// 360 圆屏：可视区是内切圆，半径 180。下面每个 y 都按 sqrt(180²-dy²) 反推过
// 可用宽度，改动布局时要一起复算，否则内容会被圆角切掉。
constexpr int32_t kPanel = DISPLAY_WIDTH;
constexpr int32_t kBackBtnSize = 36;
constexpr int32_t kBackBtnX = 36;
constexpr int32_t kBackBtnY = 28;
constexpr int32_t kTopLabelY = 24;
constexpr int32_t kAlbumSize = 126;
constexpr int32_t kAlbumY = 56;
constexpr int32_t kAlbumMaskShrink = 3;
constexpr int32_t kAlbumMaskSize = kAlbumSize - kAlbumMaskShrink * 2;
constexpr uint32_t kAlbumFrameDelayMs = 180;
constexpr int32_t kTitleY = 186;
constexpr int32_t kSubY = 212;
constexpr int32_t kTextW = 244;
constexpr int32_t kProgressY = 250;
constexpr int32_t kProgressW = 170;
constexpr int32_t kProgressH = 5;
constexpr int32_t kTimeW = 52;
constexpr int32_t kTimeGap = 6;
constexpr int32_t kCtrlRowY = 268;
constexpr int32_t kCtrlRowW = 272;
constexpr int32_t kCtrlRowH = 42;
constexpr int32_t kSideBtn = 36;   // 音量图标 32×32
constexpr int32_t kStepBtn = 28;   // 上/下一首图标 18×18
constexpr int32_t kPlayBtn = 36;   // 播放/暂停图标 24×24
constexpr int32_t kBottomRowY = 318;
constexpr int32_t kModeBtnW = 68;
constexpr int32_t kModeBtnH = 28;
constexpr int32_t kListBtnSize = 30;

// 列表叠层：行宽 224 是按最下面一行（y≈312）的圆内宽度 244 留边算出来的。
constexpr int32_t kListTop = 66;
constexpr int32_t kListH = 246;
constexpr int32_t kListBoxW = 236;
constexpr int32_t kListRowW = 224;
constexpr int32_t kListRowH = 44;
constexpr int32_t kListRowGap = 8;

enum class RepeatMode : int {
    kList = 0,
    kOne = 1,
    kShuffle = 2,
};

struct SdTrack {
    std::string path;
    std::string name;
    uint32_t dur_sec = 0;   // 0 = 估不出来
    uint32_t size_kb = 0;
};

struct MusicUi {
    lv_obj_t* scr = nullptr;
    lv_obj_t* lbl_top = nullptr;
    lv_obj_t* lbl_title = nullptr;
    lv_obj_t* lbl_sub = nullptr;
    lv_obj_t* bar = nullptr;
    lv_obj_t* lbl_elapsed = nullptr;
    lv_obj_t* lbl_total = nullptr;
    lv_obj_t* img_play_icon = nullptr;
    lv_obj_t* album_eaf = nullptr;
    lv_obj_t* lbl_mode = nullptr;
    lv_obj_t* list_layer = nullptr;
    lv_obj_t* list_box = nullptr;
    lv_obj_t* list_title = nullptr;
    lv_obj_t* state_layer = nullptr;
    lv_obj_t* state_spinner = nullptr;
    lv_obj_t* state_glyph = nullptr;
    lv_obj_t* state_title = nullptr;
    lv_obj_t* state_sub = nullptr;
    lv_obj_t* state_btn = nullptr;
    lv_timer_t* tick = nullptr;
    bool playing = false;
};

MusicUi s_ui;
bool s_screen_active = false;

// ---------------------------------------------------------------------------
// 跨线程状态
//
// 扫描 task 和播放 task 都不碰 LVGL；它们只改这些 atomic / 加锁的曲目表，
// 由 LVGL 里的 250ms tick 定时器统一刷 UI。这样不用再往 lv_async_call 里
// 塞消息，也不会有 LVGL 被别的线程重入的风险。
// ---------------------------------------------------------------------------
std::mutex s_tracks_mutex;
std::vector<SdTrack> s_tracks;

std::atomic<bool> s_scanning{false};
std::atomic<int> s_scan_found{0};
std::atomic<bool> s_scan_done{false};   // tick 消费一次后清掉
std::atomic<bool> s_scan_abort{false};
std::atomic<bool> s_sd_ready{false};

std::atomic<size_t> s_index{0};
std::atomic<bool> s_want_play{false};
std::atomic<bool> s_paused{false};
std::atomic<bool> s_in_run{false};       // 播放 task 正阻塞在 run_to_end 里
std::atomic<bool> s_shutdown{false};
std::atomic<uint32_t> s_play_gen{0};     // 用户切歌时自增，用来废弃旧的 run_to_end
std::atomic<uint32_t> s_track_seq{0};    // 每次开新曲自增，UI 用它归零计时
std::atomic<int> s_pcm_channels{2};      // 解码器上报的声道数，输出前转单声道
std::atomic<uint32_t> s_cur_bytes{0};    // 当前文件大小，配合码率估总时长
std::atomic<uint32_t> s_total_sec{0};
std::atomic<int> s_repeat_mode{static_cast<int>(RepeatMode::kList)};

esp_asp_handle_t s_player = nullptr;
TaskHandle_t s_play_task = nullptr;
AudioCodec* s_codec = nullptr;
std::vector<int16_t> s_pcm_buf;
bool s_wake_disabled_by_us = false;

// UI 侧的播放计时（只在 LVGL 线程读写）
uint32_t s_ui_seq = 0;
uint32_t s_ui_elapsed_ms = 0;
uint32_t s_ui_last_tick = 0;
uint32_t s_vol_toast_tick = 0;

inline lv_style_selector_t Sel(lv_part_t part, lv_state_t state) {
    return static_cast<lv_style_selector_t>(part | state);
}

void RefreshTrackUi();
void RebuildList();
void ShowStateLayer();
void HideStateLayer();
void StartScan();

// ---------------------------------------------------------------------------
// 文件与时长
// ---------------------------------------------------------------------------

bool EndsWithIgnoreCase(const char* name, const char* ext) {
    if (name == nullptr || ext == nullptr) {
        return false;
    }
    const size_t nlen = strlen(name);
    const size_t elen = strlen(ext);
    if (nlen < elen) {
        return false;
    }
    for (size_t i = 0; i < elen; ++i) {
        if (tolower(static_cast<unsigned char>(name[nlen - elen + i])) !=
            tolower(static_cast<unsigned char>(ext[i]))) {
            return false;
        }
    }
    return true;
}

bool IsAudioFile(const char* name) {
    return EndsWithIgnoreCase(name, ".mp3") || EndsWithIgnoreCase(name, ".wav") ||
           EndsWithIgnoreCase(name, ".flac") || EndsWithIgnoreCase(name, ".aac") ||
           EndsWithIgnoreCase(name, ".m4a") || EndsWithIgnoreCase(name, ".ogg") ||
           EndsWithIgnoreCase(name, ".opus");
}

std::string StripExt(const std::string& name) {
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) {
        return name;
    }
    return name.substr(0, dot);
}

uint32_t ReadLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// WAV：头里的 byte_rate 是准的，直接除。
uint32_t WavDurationSec(const uint8_t* buf, size_t len, uint32_t file_size) {
    if (len < 44 || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
        return 0;
    }
    const uint32_t byte_rate = ReadLe32(buf + 28);
    if (byte_rate == 0 || file_size <= 44) {
        return 0;
    }
    return (file_size - 44) / byte_rate;
}

// MP3：跳过 ID3v2，找第一个帧同步头拿码率，按 CBR 估算（VBR 会偏）。
uint32_t Mp3DurationSec(const uint8_t* buf, size_t len, uint32_t file_size) {
    static const int kBitrateV1L3[16] = {0,  32, 40, 48,  56,  64,  80,  96,
                                         112, 128, 160, 192, 224, 256, 320, 0};
    static const int kBitrateV2L3[16] = {0,  8,  16, 24, 32, 40, 48,  56,
                                         64, 80, 96, 112, 128, 144, 160, 0};

    size_t pos = 0;
    if (len >= 10 && memcmp(buf, "ID3", 3) == 0) {
        const uint32_t tag = (static_cast<uint32_t>(buf[6] & 0x7F) << 21) |
                             (static_cast<uint32_t>(buf[7] & 0x7F) << 14) |
                             (static_cast<uint32_t>(buf[8] & 0x7F) << 7) |
                             static_cast<uint32_t>(buf[9] & 0x7F);
        pos = tag + 10;
        if (pos >= len) {
            // ID3 比这次读的还长，用整段文件按 128kbps 猜没意义，放弃。
            return 0;
        }
    }
    for (; pos + 1 < len; ++pos) {
        if (buf[pos] != 0xFF || (buf[pos + 1] & 0xE0) != 0xE0) {
            continue;
        }
        const uint8_t b1 = buf[pos + 1];
        if (pos + 2 >= len) {
            break;
        }
        const uint8_t b2 = buf[pos + 2];
        const int layer = (b1 >> 1) & 0x03;
        if (layer != 0x01) {  // 只认 Layer III
            continue;
        }
        const bool v1 = (b1 & 0x08) != 0;
        const int idx = (b2 >> 4) & 0x0F;
        const int kbps = v1 ? kBitrateV1L3[idx] : kBitrateV2L3[idx];
        if (kbps <= 0 || file_size <= pos) {
            continue;
        }
        return static_cast<uint32_t>((static_cast<uint64_t>(file_size - pos) * 8) /
                                     (static_cast<uint64_t>(kbps) * 1000));
    }
    return 0;
}

uint32_t EstimateDurationSec(const std::string& path, uint32_t file_size) {
    if (file_size == 0) {
        return 0;
    }
    const bool wav = EndsWithIgnoreCase(path.c_str(), ".wav");
    const bool mp3 = EndsWithIgnoreCase(path.c_str(), ".mp3");
    if (!wav && !mp3) {
        return 0;  // flac/aac/m4a 等交给播放时的码率事件
    }

    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return 0;
    }
    uint8_t buf[1024];
    const size_t got = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (got < 44) {
        return 0;
    }
    return wav ? WavDurationSec(buf, got, file_size)
               : Mp3DurationSec(buf, got, file_size);
}

void FormatClock(char* out, size_t out_size, uint32_t sec) {
    if (sec >= 3600) {
        snprintf(out, out_size, "%u:%02u:%02u", static_cast<unsigned>(sec / 3600),
                 static_cast<unsigned>((sec / 60) % 60),
                 static_cast<unsigned>(sec % 60));
        return;
    }
    snprintf(out, out_size, "%02u:%02u", static_cast<unsigned>(sec / 60),
             static_cast<unsigned>(sec % 60));
}

const char* ExtLabel(const std::string& name) {
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) {
        return "";
    }
    static char ext[8];
    size_t n = 0;
    for (size_t i = dot + 1; i < name.size() && n + 1 < sizeof(ext); ++i, ++n) {
        ext[n] = static_cast<char>(toupper(static_cast<unsigned char>(name[i])));
    }
    ext[n] = '\0';
    return ext;
}

// ---------------------------------------------------------------------------
// 扫描
// ---------------------------------------------------------------------------

void ScanDir(const std::string& dir, int depth, std::vector<SdTrack>* out) {
    if (depth > kScanMaxDepth || out->size() >= kMaxTracks ||
        s_scan_abort.load(std::memory_order_relaxed)) {
        return;
    }
    DIR* d = opendir(dir.c_str());
    if (d == nullptr) {
        return;
    }
    while (out->size() < kMaxTracks &&
           !s_scan_abort.load(std::memory_order_relaxed)) {
        const dirent* ent = readdir(d);
        if (ent == nullptr) {
            break;
        }
        if (ent->d_name[0] == '.') {
            continue;
        }
        std::string path = dir;
        path += '/';
        path += ent->d_name;

        struct stat st = {};
        if (stat(path.c_str(), &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            // 跳过系统资源目录，避免无意义遍历。
            if (strcmp(ent->d_name, "system") == 0) {
                continue;
            }
            ScanDir(path, depth + 1, out);
        } else if (S_ISREG(st.st_mode) && IsAudioFile(ent->d_name)) {
            SdTrack t;
            t.path = path;
            t.name = ent->d_name;
            t.size_kb = static_cast<uint32_t>(st.st_size / 1024);
            t.dur_sec =
                EstimateDurationSec(path, static_cast<uint32_t>(st.st_size));
            out->push_back(std::move(t));
            s_scan_found.store(static_cast<int>(out->size()),
                               std::memory_order_relaxed);
        }
    }
    closedir(d);
}

size_t FindTrackByPath(const std::vector<SdTrack>& tracks,
                       const std::string& path) {
    if (path.empty()) {
        return 0;
    }
    for (size_t i = 0; i < tracks.size(); ++i) {
        if (tracks[i].path == path) {
            return i;
        }
    }
    return 0;
}

void ScanTask(void* /*arg*/) {
    std::vector<SdTrack> found;
    const bool mounted = SdCardManager::GetInstance().Mount();
    s_sd_ready.store(mounted, std::memory_order_relaxed);
    if (mounted) {
        ScanDir(SdCardManager::kMountPoint, 0, &found);
    } else {
        ESP_LOGW(TAG, "SD mount failed, empty playlist");
    }
    ESP_LOGI(TAG, "scanned %u tracks (max depth %d)",
             static_cast<unsigned>(found.size()), kScanMaxDepth);

    std::string last;
    {
        Settings settings("music", false);
        last = settings.GetString("last_path");
    }
    const size_t restore = FindTrackByPath(found, last);

    {
        // 中止判断和发布放在同一把锁里：页面已经退出了就别再往回写，
        // 否则这批曲目会一直挂在全局列表上，下次进来也无从判断新旧。
        std::lock_guard<std::mutex> lock(s_tracks_mutex);
        if (!s_scan_abort.load(std::memory_order_relaxed)) {
            s_tracks = std::move(found);
            s_index.store(restore, std::memory_order_relaxed);
        }
    }
    s_scan_done.store(true, std::memory_order_relaxed);
    s_scanning.store(false, std::memory_order_relaxed);
    vTaskDelete(nullptr);
}

void StartScan() {
    // 只用 s_scanning 做互斥：任务句柄由任务自己清空，而 xTaskCreate 写回句柄的
    // 时机不一定在那之前，靠句柄判断会有把自己永久挡住的风险。
    if (s_scanning.load(std::memory_order_relaxed)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(s_tracks_mutex);
        s_tracks.clear();
    }
    s_scan_found.store(0, std::memory_order_relaxed);
    s_scan_done.store(false, std::memory_order_relaxed);
    s_scan_abort.store(false, std::memory_order_relaxed);
    s_scanning.store(true, std::memory_order_relaxed);
    // 扫描要读文件头估时长，栈放外部 RAM，别占内部堆。
    if (xTaskCreate(ScanTask, "sd_music_scan", 6144, nullptr, 4, nullptr) != pdPASS) {
        // 建不出任务就别把界面永久卡在「正在扫描」，让它落到空态去。
        ESP_LOGE(TAG, "scan task create failed");
        s_scanning.store(false, std::memory_order_relaxed);
        s_scan_done.store(true, std::memory_order_relaxed);
    }
    ShowStateLayer();
}

// ---------------------------------------------------------------------------
// 播放
// ---------------------------------------------------------------------------

std::string MakeFileUri(const std::string& path) {
    return std::string("file://") + path;
}

extern "C" int SdMusicOutCallback(uint8_t* data, int data_size, void* ctx) {
    auto* codec = static_cast<AudioCodec*>(ctx);
    if (codec == nullptr || data == nullptr || data_size <= 0) {
        return 0;
    }
    const int frames = data_size / static_cast<int>(sizeof(int16_t));
    if (frames <= 0) {
        return 0;
    }
    const auto* pcm = reinterpret_cast<const int16_t*>(data);
    const int ch = s_pcm_channels.load(std::memory_order_relaxed);

    if (ch >= 2) {
        const int mono = frames / ch;
        if (mono <= 0) {
            return 0;
        }
        s_pcm_buf.resize(static_cast<size_t>(mono));
        for (int i = 0; i < mono; ++i) {
            int sum = 0;
            for (int c = 0; c < ch; ++c) {
                sum += pcm[i * ch + c];
            }
            s_pcm_buf[static_cast<size_t>(i)] =
                static_cast<int16_t>(sum / ch);
        }
    } else {
        s_pcm_buf.assign(pcm, pcm + frames);
    }
    Application::GetInstance().GetAudioService().NotifyExternalPlayback();
    codec->OutputData(s_pcm_buf);
    return 0;
}

extern "C" int SdMusicEventCallback(esp_asp_event_pkt_t* event, void* /*ctx*/) {
    if (event == nullptr || event->payload == nullptr) {
        return 0;
    }
    if (event->type == ESP_ASP_EVENT_TYPE_MUSIC_INFO &&
        event->payload_size >= static_cast<int>(sizeof(esp_asp_music_info_t))) {
        const auto* info = static_cast<const esp_asp_music_info_t*>(event->payload);
        if (info->channels > 0) {
            s_pcm_channels.store(info->channels, std::memory_order_relaxed);
        }
        const uint32_t bytes = s_cur_bytes.load(std::memory_order_relaxed);
        // 解码器报的码率最靠谱，拿它换算总时长；扫描时的估算只是兜底。
        if (info->bitrate > 0 && bytes > 0) {
            s_total_sec.store(static_cast<uint32_t>(
                                  (static_cast<uint64_t>(bytes) * 8) /
                                  static_cast<uint64_t>(info->bitrate)),
                              std::memory_order_relaxed);
        }
    }
    return 0;
}

extern "C" int SdMusicPrevCallback(esp_asp_handle_t* handle, void* ctx) {
#ifdef CONFIG_ESP_AUDIO_SIMPLE_PLAYER_RESAMPLE_EN
    const esp_asp_handle_t player = reinterpret_cast<esp_asp_handle_t>(handle);
    auto* codec = static_cast<AudioCodec*>(ctx);
    if (player == nullptr || codec == nullptr) {
        return 0;
    }
    esp_gmf_pipeline_handle_t pipe = nullptr;
    esp_gmf_element_handle_t rate_el = nullptr;
    if (esp_audio_simple_player_get_pipeline(player, &pipe) != ESP_GMF_ERR_OK ||
        pipe == nullptr) {
        return 0;
    }
    if (esp_gmf_pipeline_get_el_by_name(pipe, "aud_rate_cvt", &rate_el) !=
            ESP_GMF_ERR_OK ||
        rate_el == nullptr) {
        return 0;
    }
    esp_gmf_rate_cvt_set_dest_rate(rate_el, codec->output_sample_rate());
#else
    (void)handle;
    (void)ctx;
#endif
    return 0;
}

void StopCurrentPlayback() {
    s_play_gen.fetch_add(1, std::memory_order_relaxed);
    s_paused.store(false, std::memory_order_relaxed);
    if (s_player != nullptr) {
        esp_audio_simple_player_stop(s_player);
    }
}

size_t TrackCount() {
    std::lock_guard<std::mutex> lock(s_tracks_mutex);
    return s_tracks.size();
}

bool GetTrack(size_t idx, SdTrack* out) {
    std::lock_guard<std::mutex> lock(s_tracks_mutex);
    if (idx >= s_tracks.size()) {
        return false;
    }
    *out = s_tracks[idx];
    return true;
}

size_t NextIndexAfterEnd(size_t current, size_t count) {
    if (count == 0) {
        return 0;
    }
    switch (static_cast<RepeatMode>(s_repeat_mode.load(std::memory_order_relaxed))) {
        case RepeatMode::kOne:
            return current;
        case RepeatMode::kShuffle:
            if (count == 1) {
                return current;
            }
            for (int i = 0; i < 8; ++i) {
                const size_t pick = esp_random() % count;
                if (pick != current) {
                    return pick;
                }
            }
            return (current + 1) % count;
        case RepeatMode::kList:
        default:
            return (current + 1) % count;
    }
}

void RememberTrack(const std::string& path) {
    Settings settings("music", true);
    settings.SetString("last_path", path);
}

void SdPlayTask(void* /*arg*/) {
    esp_asp_cfg_t cfg = {
        .in = {},
        .out =
            {
                .cb = SdMusicOutCallback,
                .user_ctx = s_codec,
            },
        .task_prio = 5,
        .task_stack = 8 * 1024,
        .task_stack_in_ext = true,
        .prev = SdMusicPrevCallback,
        .prev_ctx = s_codec,
    };

    if (esp_audio_simple_player_new(&cfg, &s_player) != ESP_GMF_ERR_OK ||
        s_player == nullptr) {
        ESP_LOGE(TAG, "create SD music player failed");
        s_play_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    esp_audio_simple_player_set_event(s_player, SdMusicEventCallback, nullptr);

    if (s_codec != nullptr) {
        s_codec->EnableOutput(true);
    }

    while (!s_shutdown.load(std::memory_order_relaxed)) {
        const size_t count = TrackCount();
        if (!s_want_play.load(std::memory_order_relaxed) || count == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        size_t idx = s_index.load(std::memory_order_relaxed);
        if (idx >= count) {
            idx = 0;
            s_index.store(0, std::memory_order_relaxed);
        }
        SdTrack track;
        if (!GetTrack(idx, &track)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (s_codec != nullptr) {
            Application::GetInstance().GetAudioService().NotifyExternalPlayback();
        }

        const uint32_t gen = s_play_gen.load(std::memory_order_relaxed);
        s_cur_bytes.store(track.size_kb * 1024, std::memory_order_relaxed);
        s_total_sec.store(track.dur_sec, std::memory_order_relaxed);
        s_paused.store(false, std::memory_order_relaxed);
        s_pcm_channels.store(2, std::memory_order_relaxed);
        s_track_seq.fetch_add(1, std::memory_order_relaxed);
        RememberTrack(track.path);
        ESP_LOGI(TAG, "play: %s", track.path.c_str());

        s_in_run.store(true, std::memory_order_relaxed);
        const esp_gmf_err_t err = esp_audio_simple_player_run_to_end(
            s_player, MakeFileUri(track.path).c_str(), nullptr);
        s_in_run.store(false, std::memory_order_relaxed);

        if (s_shutdown.load(std::memory_order_relaxed)) {
            break;
        }
        // 用户切歌 / 停止会 bump gen，这一轮的结束事件就不该再推进曲目。
        if (gen != s_play_gen.load(std::memory_order_relaxed)) {
            continue;
        }
        if (!s_want_play.load(std::memory_order_relaxed)) {
            continue;
        }
        if (err != ESP_GMF_ERR_OK) {
            ESP_LOGW(TAG, "play ended/failed: 0x%x", err);
        }
        s_index.store(NextIndexAfterEnd(idx, TrackCount()),
                      std::memory_order_relaxed);
    }

    if (s_player != nullptr) {
        esp_audio_simple_player_stop(s_player);
        esp_audio_simple_player_destroy(s_player);
        s_player = nullptr;
    }
    s_play_task = nullptr;
    vTaskDelete(nullptr);
}

void EnsurePlayTask() {
    if (s_play_task != nullptr) {
        return;
    }
    s_shutdown.store(false, std::memory_order_relaxed);
    xTaskCreate(SdPlayTask, "sd_music", 4096, nullptr, 5, &s_play_task);
}

void ShutdownPlayTask() {
    s_want_play.store(false, std::memory_order_relaxed);
    s_shutdown.store(true, std::memory_order_relaxed);
    StopCurrentPlayback();
    for (int i = 0; i < 100 && s_play_task != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (s_play_task != nullptr) {
        ESP_LOGW(TAG, "play task still running on unload");
    }
}

bool IsPlayingNow() {
    return s_want_play.load(std::memory_order_relaxed) &&
           !s_paused.load(std::memory_order_relaxed);
}

void PlayIndex(size_t idx) {
    if (idx >= TrackCount()) {
        return;
    }
    s_index.store(idx, std::memory_order_relaxed);
    s_want_play.store(true, std::memory_order_relaxed);
    Application::GetInstance().GetAudioService().NotifyExternalPlayback();
    StopCurrentPlayback();
    RefreshTrackUi();
}

// ---------------------------------------------------------------------------
// UI 构件
// ---------------------------------------------------------------------------

lv_obj_t* MakeLabel(lv_obj_t* parent, const char* text, uint32_t color,
                    int32_t width) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    if (width > 0) {
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, width);
    }
    screen_make_input_passive(lbl);
    return lbl;
}

lv_obj_t* CreateRoundButton(lv_obj_t* parent, int32_t size, uint32_t bg_color,
                            uint32_t bg_pressed, const char* icon_path,
                            lv_event_cb_t cb) {
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_pressed),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_ext_click_area(btn, 10);

    lv_obj_t* img = lv_image_create(btn);
    lv_image_set_src(img, icon_path);
    lv_obj_set_size(img, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_center(img);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    return img;
}

void GoHome() {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* home = HomeScreen::Create();
    lv_screen_load(home);
    if (old_scr != nullptr && old_scr != home) {
        lv_obj_delete_async(old_scr);
    }
}

bool ListVisible() {
    return s_ui.list_layer != nullptr &&
           !lv_obj_has_flag(s_ui.list_layer, LV_OBJ_FLAG_HIDDEN);
}

void HideList() {
    if (s_ui.list_layer != nullptr) {
        lv_obj_add_flag(s_ui.list_layer, LV_OBJ_FLAG_HIDDEN);
    }
}

void ShowList() {
    if (s_ui.list_layer == nullptr) {
        return;
    }
    RebuildList();
    lv_obj_remove_flag(s_ui.list_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ui.list_layer);
    if (s_ui.state_layer != nullptr) {
        lv_obj_move_foreground(s_ui.state_layer);
    }
}

void OnSwipeBack() {
    if (ListVisible()) {
        HideList();
        return;
    }
    GoHome();
}

// ---------------------------------------------------------------------------
// UI 刷新
// ---------------------------------------------------------------------------

void SyncAlbumEaf(bool playing) {
    if (s_ui.album_eaf == nullptr) {
        return;
    }
    if (playing) {
        lv_eaf_resume(s_ui.album_eaf);
    } else {
        lv_eaf_pause(s_ui.album_eaf);
    }
}

void ApplyPlayStateToUi(bool playing) {
    if (!s_screen_active || s_ui.img_play_icon == nullptr) {
        return;
    }
    s_ui.playing = playing;
    lv_image_set_src(s_ui.img_play_icon, playing ? "A:ic_s_player_pause.spng"
                                                 : "A:ic_s_player_play.spng");
    SyncAlbumEaf(playing);
}

const char* RepeatModeText() {
    switch (static_cast<RepeatMode>(s_repeat_mode.load(std::memory_order_relaxed))) {
        case RepeatMode::kOne:
            return "单曲";
        case RepeatMode::kShuffle:
            return "随机";
        case RepeatMode::kList:
        default:
            return "顺序";
    }
}

void RefreshModeUi() {
    if (s_ui.lbl_mode == nullptr) {
        return;
    }
    lv_label_set_text(s_ui.lbl_mode, I18n::T(RepeatModeText()));
}

void RefreshTrackUi() {
    if (!s_screen_active) {
        return;
    }
    const size_t count = TrackCount();
    if (count == 0) {
        if (s_ui.lbl_title != nullptr) {
            lv_label_set_text(s_ui.lbl_title, I18n::T("未发现音乐文件"));
        }
        if (s_ui.lbl_sub != nullptr) {
            lv_label_set_text(s_ui.lbl_sub, I18n::T("把 mp3 / wav 放到 SD 卡"));
        }
        return;
    }

    SdTrack track;
    const size_t idx = s_index.load(std::memory_order_relaxed);
    if (!GetTrack(idx, &track)) {
        return;
    }
    if (s_ui.lbl_title != nullptr) {
        lv_label_set_text(s_ui.lbl_title, StripExt(track.name).c_str());
    }
    if (s_ui.lbl_sub != nullptr) {
        char sub[96];
        snprintf(sub, sizeof(sub), "%u / %u · %s", static_cast<unsigned>(idx + 1),
                 static_cast<unsigned>(count), ExtLabel(track.name));
        lv_label_set_text(s_ui.lbl_sub, sub);
    }
    if (ListVisible()) {
        RebuildList();
    }
}

void RefreshProgressUi() {
    const uint32_t total = s_total_sec.load(std::memory_order_relaxed);
    const uint32_t elapsed_sec = s_ui_elapsed_ms / 1000;

    if (s_ui.lbl_elapsed != nullptr) {
        char buf[16];
        FormatClock(buf, sizeof(buf), elapsed_sec);
        lv_label_set_text(s_ui.lbl_elapsed, buf);
    }
    if (s_ui.lbl_total != nullptr) {
        char buf[16];
        // 估不出总时长（无码率信息的格式）就明确显示未知，不要编一个数字。
        if (total > 0) {
            FormatClock(buf, sizeof(buf), total);
        } else {
            snprintf(buf, sizeof(buf), "--:--");
        }
        lv_label_set_text(s_ui.lbl_total, buf);
    }
    if (s_ui.bar != nullptr) {
        int32_t value = 0;
        if (total > 0) {
            value = static_cast<int32_t>((elapsed_sec * 100) / total);
            if (value > 100) {
                value = 100;
            }
        }
        lv_bar_set_value(s_ui.bar, value, LV_ANIM_OFF);
    }
}

void ShowVolumeToast(int vol) {
    if (s_ui.lbl_sub == nullptr) {
        return;
    }
    char text[48];
    snprintf(text, sizeof(text), "%s %d%%", I18n::T("音量"), vol);
    lv_label_set_text(s_ui.lbl_sub, text);
    s_vol_toast_tick = lv_tick_get();
}

// ---------------------------------------------------------------------------
// 曲目列表叠层
// ---------------------------------------------------------------------------

void OnListRowClicked(lv_event_t* e) {
    const auto idx = static_cast<size_t>(
        reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    // 必须先收起列表：PlayIndex 会走 RefreshTrackUi，列表还开着就会
    // lv_obj_clean 掉正在派发事件的这一行，等于在回调里删自己。
    HideList();
    PlayIndex(idx);
    ApplyPlayStateToUi(true);
}

void RebuildList() {
    if (s_ui.list_box == nullptr) {
        return;
    }
    lv_obj_clean(s_ui.list_box);

    std::vector<SdTrack> tracks;
    {
        std::lock_guard<std::mutex> lock(s_tracks_mutex);
        tracks = s_tracks;
    }
    const size_t current = s_index.load(std::memory_order_relaxed);

    if (s_ui.list_title != nullptr) {
        char title[64];
        snprintf(title, sizeof(title), "%s · %u %s", I18n::T("SD 卡音乐"),
                 static_cast<unsigned>(tracks.size()), I18n::T("首"));
        lv_label_set_text(s_ui.list_title, title);
    }

    for (size_t i = 0; i < tracks.size(); ++i) {
        const bool active = (i == current);

        lv_obj_t* row = lv_button_create(s_ui.list_box);
        lv_obj_set_size(row, kListRowW, kListRowH);
        lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, lv_color_hex(kColorRow), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, lv_color_hex(kColorRowPressed),
                                  Sel(LV_PART_MAIN, LV_STATE_PRESSED));
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(row, 12, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(row, 0, LV_PART_MAIN);
        lv_obj_add_event_cb(row, OnListRowClicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(i)));

        if (active) {
            // 左侧竖条标出正在播放的那一行。
            lv_obj_t* mark = lv_obj_create(row);
            lv_obj_remove_style_all(mark);
            lv_obj_set_size(mark, 4, 24);
            lv_obj_align(mark, LV_ALIGN_LEFT_MID, -6, 0);
            lv_obj_set_style_radius(mark, 2, LV_PART_MAIN);
            lv_obj_set_style_bg_color(mark, lv_color_hex(kColorAccent),
                                      LV_PART_MAIN);
            lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_remove_flag(mark, LV_OBJ_FLAG_CLICKABLE);
        }

        lv_obj_t* name = lv_label_create(row);
        lv_label_set_text(name, StripExt(tracks[i].name).c_str());
        lv_obj_set_style_text_font(name, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(
            name, lv_color_hex(active ? kColorAccent : kColorText), LV_PART_MAIN);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name, active ? 116 : 138);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, active ? 4 : 0, 0);
        lv_obj_remove_flag(name, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* meta = lv_label_create(row);
        if (tracks[i].dur_sec > 0) {
            char buf[16];
            FormatClock(buf, sizeof(buf), tracks[i].dur_sec);
            lv_label_set_text(meta, buf);
        } else {
            // 时长估不出来（flac/aac 等）就退回文件大小，别显示假时间。
            char buf[20];
            const uint32_t kb = tracks[i].size_kb;
            if (kb >= 1024) {
                snprintf(buf, sizeof(buf), "%u.%u MB",
                         static_cast<unsigned>(kb / 1024),
                         static_cast<unsigned>((kb % 1024) * 10 / 1024));
            } else {
                snprintf(buf, sizeof(buf), "%u KB", static_cast<unsigned>(kb));
            }
            lv_label_set_text(meta, buf);
        }
        lv_obj_set_style_text_font(meta, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(
            meta, lv_color_hex(active ? kColorAccent : kColorMuted), LV_PART_MAIN);
        lv_obj_align(meta, LV_ALIGN_RIGHT_MID, active ? -22 : 0, 0);
        lv_obj_remove_flag(meta, LV_OBJ_FLAG_CLICKABLE);

        if (active) {
            lv_obj_t* spk = lv_label_create(row);
            lv_label_set_text(spk, FONT_AWESOME_VOLUME_HIGH);
            lv_obj_set_style_text_font(spk, &font_awesome_20_4, LV_PART_MAIN);
            lv_obj_set_style_text_color(spk, lv_color_hex(kColorAccent),
                                        LV_PART_MAIN);
            lv_obj_align(spk, LV_ALIGN_RIGHT_MID, 4, 0);
            lv_obj_remove_flag(spk, LV_OBJ_FLAG_CLICKABLE);
        }
    }

    if (tracks.empty()) {
        lv_obj_t* empty = MakeLabel(s_ui.list_box, I18n::T("列表为空"), kColorMuted,
                                    kListRowW);
        lv_obj_set_style_pad_top(empty, 40, LV_PART_MAIN);
    }
}

void BuildListLayer(lv_obj_t* scr) {
    lv_obj_t* layer = lv_obj_create(scr);
    screen_strip_obj_chrome(layer);
    lv_obj_set_size(layer, kPanel, kPanel);
    lv_obj_set_pos(layer, 0, 0);
    lv_obj_set_style_bg_color(layer, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(layer, lv_color_hex(kColorBgGrad), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(layer, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(layer, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(layer, LV_OBJ_FLAG_HIDDEN);
    s_ui.list_layer = layer;

    lv_obj_t* back = lv_button_create(layer);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, kBackBtnSize, kBackBtnSize);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back, lv_color_hex(0xFFFFFF),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_bg_opa(back, LV_OPA_20, Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, kBackBtnX, kBackBtnY);
    screen_swipe_back_ignore(back, true);
    lv_obj_t* back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_center(back_icon);
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, [](lv_event_t*) { HideList(); }, LV_EVENT_CLICKED,
                        nullptr);

    s_ui.list_title = MakeLabel(layer, I18n::T("SD 卡音乐"), kColorText, 200);
    lv_obj_align(s_ui.list_title, LV_ALIGN_TOP_MID, 0, kTopLabelY + 4);

    lv_obj_t* box = lv_obj_create(layer);
    screen_strip_obj_chrome(box);
    lv_obj_set_size(box, kListBoxW, kListH);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, kListTop);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, kListRowGap, LV_PART_MAIN);
    // 列表自己吃掉横向拖动，避免滚动被当成右滑返回。
    screen_swipe_back_ignore(box, true);
    s_ui.list_box = box;
}

// ---------------------------------------------------------------------------
// 扫描中 / 空态叠层
// ---------------------------------------------------------------------------

void OnRescanClicked(lv_event_t* /*e*/) {
    s_want_play.store(false, std::memory_order_relaxed);
    StopCurrentPlayback();
    StartScan();
}

void BuildStateLayer(lv_obj_t* scr) {
    lv_obj_t* layer = lv_obj_create(scr);
    screen_strip_obj_chrome(layer);
    lv_obj_set_size(layer, kPanel, kPanel);
    lv_obj_set_pos(layer, 0, 0);
    lv_obj_set_style_bg_color(layer, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(layer, lv_color_hex(kColorBgGrad), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(layer, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(layer, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(layer, LV_OBJ_FLAG_HIDDEN);
    s_ui.state_layer = layer;

    lv_obj_t* spinner = lv_spinner_create(layer);
    lv_obj_set_size(spinner, 190, 190);
    lv_obj_center(spinner);
    lv_spinner_set_anim_params(spinner, 1400, 200);
    lv_obj_set_style_arc_width(spinner, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(0x232732), LV_PART_MAIN);
    lv_obj_set_style_arc_width(spinner, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(kColorAccent),
                               LV_PART_INDICATOR);
    screen_make_input_passive(spinner);
    s_ui.state_spinner = spinner;

    lv_obj_t* glyph = lv_image_create(layer);
    lv_image_set_src(glyph, "A:ic_s_player_album.spng");
    lv_obj_set_size(glyph, 96, 96);
    lv_image_set_inner_align(glyph, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_align(glyph, LV_ALIGN_TOP_MID, 0, 78);
    lv_obj_set_style_image_opa(glyph, LV_OPA_40, LV_PART_MAIN);
    lv_obj_remove_flag(glyph, LV_OBJ_FLAG_CLICKABLE);
    s_ui.state_glyph = glyph;

    s_ui.state_title = MakeLabel(layer, "", kColorText, kTextW);
    lv_obj_align(s_ui.state_title, LV_ALIGN_CENTER, 0, -8);

    s_ui.state_sub = MakeLabel(layer, "", kColorMuted, kTextW);
    lv_obj_align(s_ui.state_sub, LV_ALIGN_CENTER, 0, 20);

    lv_obj_t* btn = lv_button_create(layer);
    lv_obj_set_size(btn, 120, 36);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 66);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(kColorAccent),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_bg_opa(btn, LV_OPA_20, Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_hex(kColorAccent), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, OnRescanClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, I18n::T("重新扫描"));
    lv_obj_set_style_text_font(btn_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(btn_lbl, lv_color_hex(kColorAccent), LV_PART_MAIN);
    lv_obj_center(btn_lbl);
    s_ui.state_btn = btn;
}

// 扫描中和空态共用一层，靠显示/隐藏里面的元素切换。
void ShowStateLayer() {
    if (s_ui.state_layer == nullptr) {
        return;
    }
    const bool scanning = s_scanning.load(std::memory_order_relaxed);
    lv_obj_remove_flag(s_ui.state_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ui.state_layer);

    if (scanning) {
        lv_obj_remove_flag(s_ui.state_spinner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ui.state_glyph, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ui.state_btn, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_ui.state_title, I18n::T("扫描中…"));
        char sub[64];
        snprintf(sub, sizeof(sub), "%s %d %s", I18n::T("已发现"),
                 s_scan_found.load(std::memory_order_relaxed), I18n::T("首"));
        lv_label_set_text(s_ui.state_sub, sub);
        return;
    }

    lv_obj_add_flag(s_ui.state_spinner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_ui.state_glyph, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_ui.state_btn, LV_OBJ_FLAG_HIDDEN);
    if (!s_sd_ready.load(std::memory_order_relaxed)) {
        lv_label_set_text(s_ui.state_title, I18n::T("未检测到 SD 卡"));
        lv_label_set_text(s_ui.state_sub, I18n::T("插入 SD 卡后重新扫描"));
    } else {
        lv_label_set_text(s_ui.state_title, I18n::T("未发现音乐文件"));
        lv_label_set_text(s_ui.state_sub, I18n::T("把 mp3 / wav 放到 SD 卡"));
    }
}

// 扫描进度只改一行文字。整个 ShowStateLayer 里有 move_foreground，
// 每 250ms 调一次会不停重排 z-order 并整屏失效，不能放进 tick。
void UpdateScanProgressUi() {
    if (s_ui.state_sub == nullptr) {
        return;
    }
    char sub[64];
    snprintf(sub, sizeof(sub), "%s %d %s", I18n::T("已发现"),
             s_scan_found.load(std::memory_order_relaxed), I18n::T("首"));
    lv_label_set_text(s_ui.state_sub, sub);
}

void HideStateLayer() {
    if (s_ui.state_layer != nullptr) {
        lv_obj_add_flag(s_ui.state_layer, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---------------------------------------------------------------------------
// 事件
// ---------------------------------------------------------------------------

void OnPrevClicked(lv_event_t* /*e*/) {
    const size_t count = TrackCount();
    if (count == 0) {
        return;
    }
    const size_t idx = s_index.load(std::memory_order_relaxed);
    PlayIndex((idx + count - 1) % count);
    ApplyPlayStateToUi(true);
}

void OnNextClicked(lv_event_t* /*e*/) {
    const size_t count = TrackCount();
    if (count == 0) {
        return;
    }
    const size_t idx = s_index.load(std::memory_order_relaxed);
    PlayIndex((idx + 1) % count);
    ApplyPlayStateToUi(true);
}

void OnPlayClicked(lv_event_t* /*e*/) {
    if (TrackCount() == 0) {
        ShowStateLayer();
        return;
    }
    if (IsPlayingNow()) {
        // 真暂停：pause 而不是 stop，恢复时接着放而不是从头开始。
        if (s_player != nullptr && s_in_run.load(std::memory_order_relaxed)) {
            esp_audio_simple_player_pause(s_player);
            s_paused.store(true, std::memory_order_relaxed);
        } else {
            s_want_play.store(false, std::memory_order_relaxed);
            StopCurrentPlayback();
        }
        ApplyPlayStateToUi(false);
        return;
    }
    Application::GetInstance().GetAudioService().NotifyExternalPlayback();
    if (s_paused.load(std::memory_order_relaxed) && s_player != nullptr) {
        esp_audio_simple_player_resume(s_player);
        s_paused.store(false, std::memory_order_relaxed);
    } else {
        s_want_play.store(true, std::memory_order_relaxed);
    }
    ApplyPlayStateToUi(true);
}

void AdjustVolume(int delta) {
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        return;
    }
    int vol = codec->output_volume() + delta;
    if (vol < 0) {
        vol = 0;
    }
    if (vol > 100) {
        vol = 100;
    }
    codec->SetOutputVolume(vol);
    ShowVolumeToast(vol);
}

void OnVolDownClicked(lv_event_t* /*e*/) { AdjustVolume(-kVolStep); }
void OnVolUpClicked(lv_event_t* /*e*/) { AdjustVolume(kVolStep); }

void OnListClicked(lv_event_t* /*e*/) { ShowList(); }

void OnModeClicked(lv_event_t* /*e*/) {
    const int next = (s_repeat_mode.load(std::memory_order_relaxed) + 1) % 3;
    s_repeat_mode.store(next, std::memory_order_relaxed);
    Settings settings("music", true);
    settings.SetInt("repeat", next);
    RefreshModeUi();
}

// 250ms 统一刷 UI：扫描进度、播放状态、已播时间都在这里对齐后台状态。
void OnTick(lv_timer_t* /*t*/) {
    if (!s_screen_active) {
        return;
    }
    const uint32_t now = lv_tick_get();

    if (s_scanning.load(std::memory_order_relaxed)) {
        UpdateScanProgressUi();
        s_ui_last_tick = now;
        return;
    }
    if (s_scan_done.exchange(false, std::memory_order_relaxed)) {
        if (TrackCount() == 0) {
            ShowStateLayer();
        } else {
            HideStateLayer();
            RefreshTrackUi();
        }
    }

    const uint32_t seq = s_track_seq.load(std::memory_order_relaxed);
    if (seq != s_ui_seq) {
        s_ui_seq = seq;
        s_ui_elapsed_ms = 0;
        RefreshTrackUi();
    }

    const bool playing = IsPlayingNow();
    if (playing) {
        s_ui_elapsed_ms += now - s_ui_last_tick;
    }
    s_ui_last_tick = now;

    if (playing != s_ui.playing) {
        ApplyPlayStateToUi(playing);
    }
    RefreshProgressUi();

    if (s_vol_toast_tick != 0 && lv_tick_elaps(s_vol_toast_tick) > kVolToastMs) {
        s_vol_toast_tick = 0;
        RefreshTrackUi();
    }
}

// ---------------------------------------------------------------------------
// 播放页
// ---------------------------------------------------------------------------

void BuildBackButton(lv_obj_t* scr) {
    lv_obj_t* back_btn = lv_button_create(scr);
    lv_obj_remove_style_all(back_btn);
    lv_obj_set_size(back_btn, kBackBtnSize, kBackBtnSize);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xFFFFFF),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_20,
                            Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_radius(back_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back_btn, 0, LV_PART_MAIN);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, kBackBtnX, kBackBtnY);
    screen_swipe_back_ignore(back_btn, true);

    lv_obj_t* back_icon = lv_image_create(back_btn);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    lv_obj_add_event_cb(back_btn, [](lv_event_t*) { GoHome(); }, LV_EVENT_CLICKED,
                        nullptr);
}

void BuildAlbum(lv_obj_t* scr) {
    lv_obj_t* mask = lv_obj_create(scr);
    lv_obj_set_size(mask, kAlbumMaskSize, kAlbumMaskSize);
    lv_obj_align(mask, LV_ALIGN_TOP_MID, 0, kAlbumY + kAlbumMaskShrink);
    screen_strip_obj_chrome(mask);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(mask, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(mask, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(mask, 0, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(mask, true, LV_PART_MAIN);

    s_ui.album_eaf = lv_eaf_create(mask);
    lv_eaf_set_src(s_ui.album_eaf, "A:ic_s_music_album.eaf");
    lv_eaf_set_frame_delay(s_ui.album_eaf, kAlbumFrameDelayMs);
    lv_obj_set_size(s_ui.album_eaf, kAlbumSize, kAlbumSize);
    lv_image_set_inner_align(s_ui.album_eaf, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_center(s_ui.album_eaf);
    SyncAlbumEaf(false);
    screen_make_input_passive(mask);
}

void BuildProgress(lv_obj_t* scr) {
    lv_obj_t* bar = lv_bar_create(scr);
    lv_obj_set_size(bar, kProgressW, kProgressH);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, kProgressY);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColorBarTrack), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColorAccent), LV_PART_INDICATOR);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    screen_make_input_passive(bar);
    s_ui.bar = bar;

    const int32_t offset = kProgressW / 2 + kTimeGap + kTimeW / 2;
    s_ui.lbl_elapsed = MakeLabel(scr, "00:00", kColorMuted, kTimeW);
    lv_obj_align(s_ui.lbl_elapsed, LV_ALIGN_TOP_MID, -offset, kProgressY - 10);

    s_ui.lbl_total = MakeLabel(scr, "--:--", kColorMuted, kTimeW);
    lv_obj_align(s_ui.lbl_total, LV_ALIGN_TOP_MID, offset, kProgressY - 10);
}

void BuildControls(lv_obj_t* scr) {
    lv_obj_t* row = lv_obj_create(scr);
    lv_obj_set_size(row, kCtrlRowW, kCtrlRowH);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, kCtrlRowY);
    screen_strip_obj_chrome(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    CreateRoundButton(row, kSideBtn, kColorBtn, kColorBtnPressed,
                      "A:ic_s_music_volume_down.spng", OnVolDownClicked);
    CreateRoundButton(row, kStepBtn, kColorBtn, kColorBtnPressed,
                      "A:ic_s_player_previous.spng", OnPrevClicked);
    s_ui.img_play_icon =
        CreateRoundButton(row, kPlayBtn, kColorAccent, 0xC7E035,
                          "A:ic_s_player_play.spng", OnPlayClicked);
    CreateRoundButton(row, kStepBtn, kColorBtn, kColorBtnPressed,
                      "A:ic_s_player_next.spng", OnNextClicked);
    CreateRoundButton(row, kSideBtn, kColorBtn, kColorBtnPressed,
                      "A:ic_s_music_volume_up.spng", OnVolUpClicked);
}

void BuildBottomRow(lv_obj_t* scr) {
    // 列表图标没有现成资源，用三根小横条自己画，省一张图。
    lv_obj_t* list_btn = lv_button_create(scr);
    lv_obj_set_size(list_btn, kListBtnSize, kListBtnSize);
    lv_obj_set_style_radius(list_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(list_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(list_btn, lv_color_hex(0xFFFFFF),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_bg_opa(list_btn, LV_OPA_20,
                            Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_border_width(list_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(list_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list_btn, 0, LV_PART_MAIN);
    lv_obj_align(list_btn, LV_ALIGN_TOP_MID, -46, kBottomRowY);
    lv_obj_set_ext_click_area(list_btn, 10);
    lv_obj_add_event_cb(list_btn, OnListClicked, LV_EVENT_CLICKED, nullptr);
    for (int i = 0; i < 3; ++i) {
        lv_obj_t* line = lv_obj_create(list_btn);
        lv_obj_remove_style_all(line);
        lv_obj_set_size(line, 16, 2);
        lv_obj_align(line, LV_ALIGN_CENTER, 0, (i - 1) * 6);
        lv_obj_set_style_radius(line, 1, LV_PART_MAIN);
        lv_obj_set_style_bg_color(line, lv_color_hex(kColorMuted), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t* mode_btn = lv_button_create(scr);
    lv_obj_set_size(mode_btn, kModeBtnW, kModeBtnH);
    lv_obj_align(mode_btn, LV_ALIGN_TOP_MID, 30, kBottomRowY + 1);
    lv_obj_set_style_radius(mode_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(mode_btn, lv_color_hex(kColorBtn), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mode_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(mode_btn, lv_color_hex(kColorBtnPressed),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_border_width(mode_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(mode_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(mode_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(mode_btn, OnModeClicked, LV_EVENT_CLICKED, nullptr);

    s_ui.lbl_mode = lv_label_create(mode_btn);
    lv_obj_set_style_text_font(s_ui.lbl_mode, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.lbl_mode, lv_color_hex(kColorMuted),
                                LV_PART_MAIN);
    lv_obj_center(s_ui.lbl_mode);
    RefreshModeUi();
}

void BuildUi(lv_obj_t* scr) {
    s_ui.lbl_top = MakeLabel(scr, I18n::T("本地音乐 · SD 卡"), kColorMuted, kTextW);
    lv_obj_align(s_ui.lbl_top, LV_ALIGN_TOP_MID, 0, kTopLabelY);

    BuildAlbum(scr);

    s_ui.lbl_title = MakeLabel(scr, I18n::T("SD 卡音乐"), kColorText, kTextW);
    lv_obj_align(s_ui.lbl_title, LV_ALIGN_TOP_MID, 0, kTitleY);

    s_ui.lbl_sub = MakeLabel(scr, "", kColorMuted, kTextW);
    lv_obj_align(s_ui.lbl_sub, LV_ALIGN_TOP_MID, 0, kSubY);

    BuildProgress(scr);
    BuildControls(scr);
    BuildBottomRow(scr);
    BuildListLayer(scr);
    BuildStateLayer(scr);

    // BackButton 最后建，保证它在 z-order 顶层、可被点中。
    BuildBackButton(scr);
    lv_obj_move_foreground(s_ui.list_layer);
    lv_obj_move_foreground(s_ui.state_layer);
}

void OnScreenUnloaded(lv_event_t* /*e*/) {
    if (s_ui.tick != nullptr) {
        lv_timer_delete(s_ui.tick);
    }
    s_screen_active = false;
    s_ui = MusicUi{};
}

}  // namespace

lv_obj_t* MusicScreenSd::Create() {
    s_ui = MusicUi{};
    s_ui.playing = false;

    lv_obj_t* scr = lv_obj_create(nullptr);
    screen_strip_obj_chrome(scr);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(kColorBgGrad), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    {
        Settings settings("music", false);
        int mode = settings.GetInt("repeat", static_cast<int>(RepeatMode::kList));
        if (mode < 0 || mode > 2) {
            mode = static_cast<int>(RepeatMode::kList);
        }
        s_repeat_mode.store(mode, std::memory_order_relaxed);
    }

    s_ui.scr = scr;
    BuildUi(scr);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);
    screen_attach_swipe_back(scr, OnSwipeBack);

    s_screen_active = true;
    s_ui_seq = s_track_seq.load(std::memory_order_relaxed);
    s_ui_elapsed_ms = 0;
    s_ui_last_tick = lv_tick_get();
    s_vol_toast_tick = 0;
    s_ui.tick = lv_timer_create(OnTick, 250, nullptr);
    return scr;
}

void MusicScreenSd::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: SD card music");
        s_codec = Board::GetInstance().GetAudioCodec();
        auto& as = Application::GetInstance().GetAudioService();
        s_wake_disabled_by_us = false;
        if (as.IsWakeWordRunning()) {
            as.ReleaseWakeWordDetection();
            s_wake_disabled_by_us = true;
            vTaskDelay(pdMS_TO_TICKS(150));
        }
        if (s_codec != nullptr) {
            s_codec->EnableInput(false);
            as.SetExternalPlaybackHold(true);
            as.NotifyExternalPlayback();
        }

        StartScan();
        EnsurePlayTask();
    } else {
        ESP_LOGI(TAG, "unload: SD card music");
        // 扫描可能还在读 SD，先让它自己收尾，别让它在退出后继续搅 SD。
        s_scan_abort.store(true, std::memory_order_relaxed);
        for (int i = 0; i < 40 && s_scanning.load(std::memory_order_relaxed); ++i) {
            vTaskDelay(pdMS_TO_TICKS(25));
        }
        ShutdownPlayTask();
        Application::GetInstance().GetAudioService().SetExternalPlaybackHold(false);
        if (s_wake_disabled_by_us) {
            Application::GetInstance().GetAudioService().EnableWakeWordDetection(
                true);
            s_wake_disabled_by_us = false;
        }
        s_screen_active = false;
        {
            std::lock_guard<std::mutex> lock(s_tracks_mutex);
            s_tracks.clear();
        }
        s_index.store(0, std::memory_order_relaxed);
        s_codec = nullptr;
    }
}

#else  // !BOARD_ESP_VOCAT

lv_obj_t* MusicScreenSd::Create() { return nullptr; }

void MusicScreenSd::LifecycleCallback(screen_lifecycle_event_t /*event*/) {}

#endif  // BOARD_ESP_VOCAT
