#include "music_screen_sd.h"

#include "i18n.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "esp_audio_simple_player.h"
#include "esp_audio_simple_player_advance.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "config.h"
#include "home_screen/home_screen.h"
#include "lv_eaf.h"
#include "screen_util.h"
#include "SdCardManager.hpp"

#ifdef CONFIG_ESP_AUDIO_SIMPLE_PLAYER_RESAMPLE_EN
#include "esp_gmf_pipeline.h"
#include "esp_gmf_rate_cvt.h"
#endif

LV_FONT_DECLARE(font_puhui_20_4);

#if defined(BOARD_ESP_VOCAT)

namespace {

constexpr const char* TAG = "MusicScreenSd";
constexpr int kScanMaxDepth = 5;
constexpr size_t kMaxTracks = 300;
constexpr int kVolStep = 5;

constexpr auto kPanelSize = DISPLAY_WIDTH;
constexpr int32_t kTitleY = 36;
constexpr int32_t kAlbumSize = 140;
constexpr int32_t kHintBottomMargin = 32;
constexpr int32_t kAlbumMaskShrink = 3;
constexpr int32_t kAlbumY = 78;
constexpr int32_t kLyricY = 221;
constexpr int32_t kLyricLineGap = 14;
constexpr int32_t kCtrlRowY = 274;
constexpr int32_t kCtrlRowWidth = 280;
constexpr int32_t kCtrlRowHeight = 32;
constexpr int32_t kCtrlSideBtnSize = 26;
constexpr int32_t kCtrlPlayBtnSize = 32;
constexpr int32_t kBackBtnSize = 36;
constexpr int32_t kBackBtnX = 36;
constexpr int32_t kBackBtnY = 28;
constexpr int32_t kAlbumMaskSize = kAlbumSize - kAlbumMaskShrink * 2;
constexpr uint32_t kAlbumFrameDelayMs = 180;
constexpr int32_t kLyricLineWidth = kPanelSize - 80;

constexpr uint32_t kColorBg = 0x0E1116;
constexpr uint32_t kColorBgGrad = 0x161A22;
constexpr uint32_t kColorTextPrimary = 0xFFFFFF;
constexpr uint32_t kColorAccent = 0xE0FB3C;
constexpr uint32_t kColorCtrlBtnBg = 0x232732;
constexpr uint32_t kColorCtrlBtnBgPressed = 0x303644;
constexpr uint32_t kColorPlayBtnBg = 0x3A4150;
constexpr uint32_t kColorPlayBtnBgPressed = 0x4A5260;

inline lv_style_selector_t Sel(lv_part_t part, lv_state_t state) {
    return static_cast<lv_style_selector_t>(part | state);
}

struct SdTrack {
    std::string path;
    std::string name;
};

struct MusicUi {
    lv_obj_t* lbl_song = nullptr;
    lv_obj_t* lbl_status = nullptr;
    lv_obj_t* img_play_icon = nullptr;
    lv_obj_t* album_eaf = nullptr;
    bool playing = false;
};

MusicUi s_ui;
bool s_screen_active = false;
std::vector<SdTrack> s_tracks;
size_t s_index = 0;

esp_asp_handle_t s_player = nullptr;
TaskHandle_t s_play_task = nullptr;
AudioCodec* s_codec = nullptr;
std::vector<int16_t> s_pcm_buf;

std::atomic<bool> s_want_play{false};
std::atomic<bool> s_shutdown{false};
std::atomic<uint32_t> s_play_gen{0};
bool s_wake_disabled_by_us = false;

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
    lv_image_set_src(s_ui.img_play_icon,
                     playing ? "A:ic_s_player_pause.spng"
                             : "A:ic_s_player_play.spng");
    SyncAlbumEaf(playing);
}

void AsyncApplyPlayState(void* user_data) {
    auto* msg = static_cast<bool*>(user_data);
    ApplyPlayStateToUi(*msg);
    delete msg;
}

void PostPlayState(bool playing) {
    if (!s_screen_active) {
        return;
    }
    auto* msg = new bool(playing);
    if (lv_async_call(AsyncApplyPlayState, msg) != LV_RESULT_OK) {
        delete msg;
    }
}

struct AsyncTextMsg {
    char text[192];
};

void AsyncSetSong(void* user_data) {
    auto* msg = static_cast<AsyncTextMsg*>(user_data);
    if (s_screen_active && s_ui.lbl_song != nullptr) {
        lv_label_set_text(s_ui.lbl_song, msg->text);
    }
    delete msg;
}

void AsyncSetStatus(void* user_data) {
    auto* msg = static_cast<AsyncTextMsg*>(user_data);
    if (s_screen_active && s_ui.lbl_status != nullptr) {
        lv_label_set_text(s_ui.lbl_status, msg->text);
    }
    delete msg;
}

void PostSong(const char* text) {
    if (!s_screen_active || text == nullptr) {
        return;
    }
    auto* msg = new AsyncTextMsg{};
    snprintf(msg->text, sizeof(msg->text), "%s", text);
    if (lv_async_call(AsyncSetSong, msg) != LV_RESULT_OK) {
        delete msg;
    }
}

void PostStatus(const char* text) {
    if (!s_screen_active || text == nullptr) {
        return;
    }
    auto* msg = new AsyncTextMsg{};
    snprintf(msg->text, sizeof(msg->text), "%s", text);
    if (lv_async_call(AsyncSetStatus, msg) != LV_RESULT_OK) {
        delete msg;
    }
}

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

void ScanDir(const std::string& dir, int depth) {
    if (depth > kScanMaxDepth || s_tracks.size() >= kMaxTracks) {
        return;
    }

    DIR* d = opendir(dir.c_str());
    if (d == nullptr) {
        return;
    }

    while (s_tracks.size() < kMaxTracks) {
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
            ScanDir(path, depth + 1);
        } else if (S_ISREG(st.st_mode) && IsAudioFile(ent->d_name)) {
            s_tracks.push_back(SdTrack{path, ent->d_name});
        }
    }
    closedir(d);
}

void ScanMusic() {
    s_tracks.clear();
    s_index = 0;
    if (!SdCardManager::GetInstance().Mount()) {
        ESP_LOGW(TAG, "SD mount failed, empty playlist");
        return;
    }
    ScanDir(SdCardManager::kMountPoint, 0);
    ESP_LOGI(TAG, "scanned %u tracks (max depth %d)",
             static_cast<unsigned>(s_tracks.size()), kScanMaxDepth);
}

std::string MakeFileUri(const std::string& path) {
    return std::string("file://") + path;
}

extern "C" int SdMusicOutCallback(uint8_t* data, int data_size, void* ctx) {
    auto* codec = static_cast<AudioCodec*>(ctx);
    if (codec == nullptr || data == nullptr || data_size <= 0) {
        return 0;
    }
    const int samples = data_size / static_cast<int>(sizeof(int16_t));
    if (samples <= 0) {
        return 0;
    }
    const auto* pcm = reinterpret_cast<const int16_t*>(data);
    s_pcm_buf.resize(static_cast<size_t>(samples));
    std::memcpy(s_pcm_buf.data(), pcm, static_cast<size_t>(data_size));
    codec->OutputData(s_pcm_buf);
    return 0;
}

extern "C" int SdMusicEventCallback(esp_asp_event_pkt_t* /*event*/,
                                    void* /*ctx*/) {
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
    if (s_player != nullptr) {
        esp_audio_simple_player_stop(s_player);
    }
}

void RefreshTrackUi() {
    if (s_tracks.empty()) {
        PostSong(I18n::T("未找到音乐"));
        PostStatus(I18n::T("请将 mp3/wav 等放入 SD 卡"));
        return;
    }
    if (s_index >= s_tracks.size()) {
        s_index = 0;
    }
    const auto& t = s_tracks[s_index];
    PostSong(t.name.c_str());
    char status[96];
    snprintf(status, sizeof(status), "%u / %u",
             static_cast<unsigned>(s_index + 1),
             static_cast<unsigned>(s_tracks.size()));
    PostStatus(status);
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
        .prev = SdMusicPrevCallback,
        .prev_ctx = s_codec,
    };

    if (esp_audio_simple_player_new(&cfg, &s_player) != ESP_GMF_ERR_OK ||
        s_player == nullptr) {
        ESP_LOGE(TAG, "create SD music player failed");
        PostStatus(I18n::T("播放器创建失败"));
        s_play_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    esp_audio_simple_player_set_event(s_player, SdMusicEventCallback, nullptr);

    if (s_codec != nullptr) {
        s_codec->EnableOutput(true);
    }

    while (!s_shutdown.load(std::memory_order_relaxed)) {
        if (!s_want_play.load(std::memory_order_relaxed) || s_tracks.empty()) {
            PostPlayState(false);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (s_index >= s_tracks.size()) {
            s_index = 0;
        }
        const uint32_t gen = s_play_gen.load(std::memory_order_relaxed);
        const size_t idx = s_index;
        const std::string uri = MakeFileUri(s_tracks[idx].path);
        RefreshTrackUi();
        PostPlayState(true);
        ESP_LOGI(TAG, "play: %s", uri.c_str());

        const esp_gmf_err_t err =
            esp_audio_simple_player_run_to_end(s_player, uri.c_str(), nullptr);

        if (s_shutdown.load(std::memory_order_relaxed)) {
            break;
        }
        // 用户切歌 / 暂停会 stop + bump gen，这里不再自动下一首。
        if (gen != s_play_gen.load(std::memory_order_relaxed)) {
            continue;
        }
        if (!s_want_play.load(std::memory_order_relaxed)) {
            PostPlayState(false);
            continue;
        }
        if (err != ESP_GMF_ERR_OK) {
            ESP_LOGW(TAG, "play ended/failed: 0x%x", err);
        }
        // 自然播完：自动下一首
        s_index = (idx + 1) % s_tracks.size();
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

void OnPrevClicked(lv_event_t* /*e*/) {
    if (s_tracks.empty()) {
        return;
    }
    s_index = (s_index + s_tracks.size() - 1) % s_tracks.size();
    s_want_play.store(true, std::memory_order_relaxed);
    StopCurrentPlayback();
    RefreshTrackUi();
}

void OnNextClicked(lv_event_t* /*e*/) {
    if (s_tracks.empty()) {
        return;
    }
    s_index = (s_index + 1) % s_tracks.size();
    s_want_play.store(true, std::memory_order_relaxed);
    StopCurrentPlayback();
    RefreshTrackUi();
}

void OnPlayClicked(lv_event_t* /*e*/) {
    if (s_tracks.empty()) {
        PostStatus(I18n::T("请将 mp3/wav 等放入 SD 卡"));
        return;
    }
    const bool want = !s_ui.playing;
    if (want) {
        s_want_play.store(true, std::memory_order_relaxed);
        ApplyPlayStateToUi(true);
    } else {
        s_want_play.store(false, std::memory_order_relaxed);
        StopCurrentPlayback();
        ApplyPlayStateToUi(false);
    }
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
    char status[48];
    snprintf(status, sizeof(status), "%s %d%%", I18n::T("音量"), vol);
    if (s_ui.lbl_status != nullptr) {
        lv_label_set_text(s_ui.lbl_status, status);
    }
}

void OnVolDownClicked(lv_event_t* /*e*/) { AdjustVolume(-kVolStep); }
void OnVolUpClicked(lv_event_t* /*e*/) { AdjustVolume(kVolStep); }

void OnSwipeBack() {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* home = HomeScreen::Create();
    lv_screen_load(home);
    if (old_scr != nullptr && old_scr != home) {
        lv_obj_delete_async(old_scr);
    }
}

void OnScreenUnloaded(lv_event_t* /*e*/) {
    s_screen_active = false;
    s_ui = MusicUi{};
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
    lv_obj_set_ext_click_area(btn, 12);

    lv_obj_t* img = lv_image_create(btn);
    lv_image_set_src(img, icon_path);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);
    lv_obj_center(img);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    return img;
}

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

    lv_obj_add_event_cb(
        back_btn, [](lv_event_t* /*e*/) { OnSwipeBack(); }, LV_EVENT_CLICKED,
        nullptr);
}

void BuildUi(lv_obj_t* scr) {
    s_ui.lbl_song = lv_label_create(scr);
    lv_label_set_text(s_ui.lbl_song, I18n::T("SD 卡音乐"));
    lv_obj_set_style_text_font(s_ui.lbl_song, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.lbl_song, lv_color_hex(kColorTextPrimary),
                                LV_PART_MAIN);
    lv_obj_set_style_text_align(s_ui.lbl_song, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_label_set_long_mode(s_ui.lbl_song, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_ui.lbl_song, kLyricLineWidth);
    lv_obj_align(s_ui.lbl_song, LV_ALIGN_TOP_MID, 0, kTitleY);
    screen_make_input_passive(s_ui.lbl_song);

    lv_obj_t* hint = lv_label_create(scr);
    lv_label_set_text(hint, I18n::T("本地音乐 · 扫描 SD 卡最多 5 级目录"));
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x8B92A3), LV_PART_MAIN);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);
    lv_obj_set_width(hint, kLyricLineWidth);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -kHintBottomMargin);
    screen_make_input_passive(hint);

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

    s_ui.lbl_status = lv_label_create(scr);
    lv_label_set_text(s_ui.lbl_status, "");
    lv_obj_set_style_text_font(s_ui.lbl_status, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.lbl_status, lv_color_hex(kColorAccent),
                                LV_PART_MAIN);
    lv_obj_set_style_text_align(s_ui.lbl_status, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_label_set_long_mode(s_ui.lbl_status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_ui.lbl_status, kLyricLineWidth);
    lv_obj_align(s_ui.lbl_status, LV_ALIGN_TOP_MID, 0, kLyricY + kLyricLineGap);
    screen_make_input_passive(s_ui.lbl_status);

    lv_obj_t* row = lv_obj_create(scr);
    lv_obj_set_size(row, kCtrlRowWidth, kCtrlRowHeight);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, kCtrlRowY);
    screen_strip_obj_chrome(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    CreateRoundButton(row, kCtrlSideBtnSize, kColorCtrlBtnBg,
                      kColorCtrlBtnBgPressed, "A:ic_s_music_volume_down.spng",
                      OnVolDownClicked);
    CreateRoundButton(row, kCtrlSideBtnSize, kColorCtrlBtnBg,
                      kColorCtrlBtnBgPressed, "A:ic_s_player_previous.spng",
                      OnPrevClicked);
    s_ui.img_play_icon =
        CreateRoundButton(row, kCtrlPlayBtnSize, kColorPlayBtnBg,
                          kColorPlayBtnBgPressed, "A:ic_s_player_play.spng",
                          OnPlayClicked);
    CreateRoundButton(row, kCtrlSideBtnSize, kColorCtrlBtnBg,
                      kColorCtrlBtnBgPressed, "A:ic_s_player_next.spng",
                      OnNextClicked);
    CreateRoundButton(row, kCtrlSideBtnSize, kColorCtrlBtnBg,
                      kColorCtrlBtnBgPressed, "A:ic_s_music_volume_up.spng",
                      OnVolUpClicked);

    BuildBackButton(scr);
}

}  // namespace

lv_obj_t* MusicScreenSd::Create() {
    s_ui = MusicUi{};
    s_ui.playing = false;

    lv_obj_t* scr = lv_obj_create(nullptr);
    screen_strip_obj_chrome(scr);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(kColorBgGrad),
                                   LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    BuildUi(scr);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);
    screen_attach_swipe_back(scr, OnSwipeBack);

    s_screen_active = true;
    return scr;
}

void MusicScreenSd::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: SD card music");
        s_codec = Board::GetInstance().GetAudioCodec();
        auto& as = Application::GetInstance().GetAudioService();
        s_wake_disabled_by_us = false;
        if (as.IsWakeWordRunning()) {
            as.EnableWakeWordDetection(false);
            s_wake_disabled_by_us = true;
            vTaskDelay(pdMS_TO_TICKS(150));
        }
        if (s_codec != nullptr) {
            s_codec->EnableOutput(true);
        }

        ScanMusic();
        EnsurePlayTask();
        RefreshTrackUi();
    } else {
        ESP_LOGI(TAG, "unload: SD card music");
        ShutdownPlayTask();
        if (s_wake_disabled_by_us) {
            Application::GetInstance()
                .GetAudioService()
                .EnableWakeWordDetection(true);
            s_wake_disabled_by_us = false;
        }
        s_screen_active = false;
        s_tracks.clear();
        s_index = 0;
        s_codec = nullptr;
    }
}

#else  // !BOARD_ESP_VOCAT

lv_obj_t* MusicScreenSd::Create() { return nullptr; }

void MusicScreenSd::LifecycleCallback(screen_lifecycle_event_t /*event*/) {}

#endif  // BOARD_ESP_VOCAT
