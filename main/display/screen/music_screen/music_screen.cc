#include "music_screen.h"
#include "i18n.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "application.h"
#include "board.h"
#include "config.h"
#if defined(BOARD_ESP_VOCAT)
#include "music_screen_sd.h"
#endif
#if BOARD_HAS_EXTERNAL_BT
#include "SimpleUart.hpp"
#endif

#include "home_screen/home_screen.h"
#include "native_bluetooth_audio.h"
#include "screen_util.h"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char* TAG = "MusicScreen";

// ---------------------------------------------------------------------------
// 预览布局（去掉上方专辑旋转动画，控制键放大并放到中间偏下）：
//
// 360 圆屏：
//   y=36    歌名
//   y=90    歌词三行
//   y=188   [音量-] [上一曲] [播放] [下一曲] [音量+]  ← 占位圆钮
//   底部    使用提示
//
// 按钮上的内圈 = 将来图标目标区域；中间文字是占位符，方便估尺寸。
// ---------------------------------------------------------------------------
#if defined(BOARD_ESP_VOCAT) || (DISPLAY_WIDTH == 360 && DISPLAY_HEIGHT == 360)
constexpr bool kRoundLayout = true;
constexpr auto kPanelSize = DISPLAY_WIDTH;
constexpr int32_t kTitleY = 36;
constexpr int32_t kHintBottomMargin = 28;
constexpr int32_t kLyricY = 90;
constexpr int32_t kLyricLineGap = 22;
// 控制行：垂直中心约在 y=188+64/2≈220，相对圆心(180)偏下约 40px。
constexpr int32_t kCtrlRowY = 188;
constexpr int32_t kCtrlRowWidth = 320;
constexpr int32_t kCtrlRowHeight = 72;
constexpr int32_t kArtistY = 52;
constexpr int32_t kAlbumInfoY = 66;
constexpr int32_t kCtrlSideBtnSize = 56;
constexpr int32_t kCtrlPlayBtnSize = 72;
// 占位图标区约占按钮直径 62%，方便估最终 png 尺寸。
constexpr int32_t kCtrlSideIconSize = 34;
constexpr int32_t kCtrlPlayIconSize = 44;
constexpr int32_t kBackBtnSize = 40;
constexpr int32_t kBackBtnX = 78;
constexpr int32_t kBackBtnY = 56;
#else
constexpr bool kRoundLayout = false;
constexpr int32_t kPanelSize = 720;
constexpr int32_t kTitleY = 48;
constexpr int32_t kHintBottomMargin = 16;
constexpr int32_t kCtrlRowY = 380;
constexpr int32_t kCtrlRowWidth = 640;
constexpr int32_t kCtrlRowHeight = 140;
constexpr int32_t kArtistY = 88;
constexpr int32_t kAlbumInfoY = 116;
constexpr int32_t kCtrlSideBtnSize = 112;
constexpr int32_t kCtrlPlayBtnSize = 140;
constexpr int32_t kCtrlSideIconSize = 70;
constexpr int32_t kCtrlPlayIconSize = 88;
constexpr int32_t kLyricY = 180;
constexpr int32_t kLyricLineGap = 40;
constexpr int32_t kBackBtnSize = 72;
constexpr int32_t kBackBtnX = 32;
constexpr int32_t kBackBtnY = 36;
#endif

constexpr uint32_t kColorBg = 0x0E1116;
constexpr uint32_t kColorBgGrad = 0x161A22;
constexpr uint32_t kColorTextPrimary = 0xFFFFFF;
constexpr uint32_t kColorAccent = 0xE0FB3C;
constexpr uint32_t kColorCtrlBtnBg = 0x232732;
constexpr uint32_t kColorCtrlBtnBgPressed = 0x303644;
constexpr uint32_t kColorPlayBtnBg = 0x3A4150;
constexpr uint32_t kColorPlayBtnBgPressed = 0x4A5260;
constexpr uint32_t kColorBackBtnBg = 0x1A1E26;
constexpr uint32_t kColorIconPlaceholder = 0x4A5568;
constexpr uint32_t kColorIconPlaceholderPlay = 0x6B7588;

#if !BOARD_HAS_EXTERNAL_BT
constexpr size_t kNativeBtMinInternalFree = 50000;
constexpr size_t kNativeBtMinLargestBlock = 20000;
#endif

// 歌词三行布局：最上面是最新一句，越往下越旧、越透明。
constexpr int32_t kLyricLineCount = 3;
constexpr int32_t kLyricLineWidth = kPanelSize - (kRoundLayout ? 64 : 80);
// 每行的目标 opacity（顶 -> 底）。255 / 153 / 76 大约对应 100% / 60% / 30%。
constexpr lv_opa_t kLyricTargetOpa[kLyricLineCount] = {255, 153, 76};
constexpr uint32_t kLyricFadeDurationMs = 380;

struct MusicUi {
    lv_obj_t* lbl_song = nullptr;
    lv_obj_t* lbl_artist = nullptr;
    lv_obj_t* lbl_album = nullptr;
    lv_obj_t* lbl_lyric[kLyricLineCount] = {nullptr, nullptr, nullptr};
    // 占位阶段用 label；后续换真图标时可改回 image。
    lv_obj_t* lbl_play_icon = nullptr;
#if !BOARD_HAS_EXTERNAL_BT
    lv_timer_t* native_bt_ui_timer = nullptr;
#endif
    // 进入界面时蓝牙端通常还没在播放，默认按钮显示"▶ 播放"，
    // 用户点击后才切到"❚❚ 暂停"图标。
    bool playing = false;
};

MusicUi s_ui;
bool s_screen_active = false;
std::string s_rx_buffer;
#if !BOARD_HAS_EXTERNAL_BT
bool s_restore_wake_word_after_native_bt = false;
bool s_restart_audio_service_after_native_bt = false;
struct NativeBtUiCache {
    std::string title;
    std::string artist;
    std::string album;
    std::string status;
    bool track_dirty = false;
    bool status_dirty = false;
    bool play_dirty = false;
    bool connected = false;
    bool playing = false;
};
std::mutex s_native_bt_ui_mutex;
NativeBtUiCache s_native_bt_ui_cache;
#endif

void set_play_placeholder_text(bool playing) {
    if (s_ui.lbl_play_icon == nullptr) {
        return;
    }
    // 占位文字：播放态显示暂停符，暂停态显示播放符。
    lv_label_set_text(s_ui.lbl_play_icon, playing ? "II" : ">");
}

// 把 (part | state) 显式转成 lv_style_selector_t，规避
// -Wdeprecated-enum-enum-conversion 告警。
inline lv_style_selector_t Sel(lv_part_t part, lv_state_t state) {
    return static_cast<lv_style_selector_t>(part | state);
}

// ---------------------------------------------------------------------------
// 异步 UI 更新（UART task -> LVGL 主线程）
// ---------------------------------------------------------------------------
struct AsyncTextMsg {
    char text[192];
};

struct AsyncTrackInfoMsg {
    char title[128];
    char artist[128];
    char album[128];
};

void apply_track_info_to_ui(const char* title, const char* artist, const char* album) {
    if (!s_screen_active) {
        return;
    }
    const char* safe_title = title != nullptr ? title : "";
    const char* safe_artist = artist != nullptr ? artist : "";
    const char* safe_album = album != nullptr ? album : "";

    if (s_ui.lbl_song != nullptr && safe_title[0] != '\0') {
        lv_label_set_text(s_ui.lbl_song, safe_title);
    }
    if (s_ui.lbl_artist != nullptr) {
        lv_label_set_text(s_ui.lbl_artist, safe_artist);
        if (safe_artist[0] == '\0') {
            lv_obj_add_flag(s_ui.lbl_artist, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_ui.lbl_artist, LV_OBJ_FLAG_HIDDEN);
            screen_make_input_passive(s_ui.lbl_artist);
        }
    }
    if (s_ui.lbl_album != nullptr) {
        lv_label_set_text(s_ui.lbl_album, safe_album);
        if (safe_album[0] == '\0') {
            lv_obj_add_flag(s_ui.lbl_album, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_ui.lbl_album, LV_OBJ_FLAG_HIDDEN);
            screen_make_input_passive(s_ui.lbl_album);
        }
    }
}

void async_set_track_info(void* user_data) {
    auto* msg = static_cast<AsyncTrackInfoMsg*>(user_data);
    apply_track_info_to_ui(msg->title, msg->artist, msg->album);
    delete msg;
}

// 单行 opacity 动画：把 label 当前 opa 平滑过渡到 to。
void anim_label_opa_cb(void* var, int32_t v) {
    lv_obj_set_style_opa(static_cast<lv_obj_t*>(var),
                         static_cast<lv_opa_t>(v), LV_PART_MAIN);
}

void start_opa_anim(lv_obj_t* obj, int32_t from, int32_t to) {
    if (obj == nullptr) {
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, kLyricFadeDurationMs);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, anim_label_opa_cb);
    lv_anim_start(&a);
}

// 推动一行新歌词进队列：
//   line[2] <- line[1]   (旧 -> 更旧)
//   line[1] <- line[0]   (上 -> 中)
//   line[0] <- new       (新词在顶部)
// 同时把每行的 opacity 从「上一格的目标值」缓慢渐入到「自己格子的目标值」，
// 视觉上像是文字向下沉、越来越淡。
void push_lyric_line(const char* new_text) {
    if (s_ui.lbl_lyric[0] == nullptr) {
        return;
    }
    const char* cur_top = lv_label_get_text(s_ui.lbl_lyric[0]);
    const char* cur_mid = lv_label_get_text(s_ui.lbl_lyric[1]);
    std::string s_top = cur_top != nullptr ? cur_top : "";
    std::string s_mid = cur_mid != nullptr ? cur_mid : "";

    lv_label_set_text(s_ui.lbl_lyric[2], s_mid.c_str());
    lv_label_set_text(s_ui.lbl_lyric[1], s_top.c_str());
    lv_label_set_text(s_ui.lbl_lyric[0], new_text);

    // 顶行：从 0 渐入到 100% （新词淡入）
    // 中行：从 100% 渐落到 60%
    // 底行：从 60%  渐落到 30%
    start_opa_anim(s_ui.lbl_lyric[0], 0, kLyricTargetOpa[0]);
    start_opa_anim(s_ui.lbl_lyric[1], kLyricTargetOpa[0], kLyricTargetOpa[1]);
    start_opa_anim(s_ui.lbl_lyric[2], kLyricTargetOpa[1], kLyricTargetOpa[2]);
}

void async_set_lyric(void* user_data) {
    auto* msg = static_cast<AsyncTextMsg*>(user_data);
    if (s_screen_active && s_ui.lbl_lyric[0] != nullptr) {
        push_lyric_line(msg->text);
    }
    delete msg;
}

// ---- 播放 / 暂停图标同步（UART task -> LVGL 主线程） -----------------------
// 按钮图标的语义：图标本身就是「点了之后会发生的动作」。
//   playing == true  -> 当前正在播 -> 图标显示 ❚❚（点了就暂停）
//   playing == false -> 当前已暂停 -> 图标显示 ▶ （点了就播放）
struct AsyncPlayStateMsg {
    bool playing;
};

void apply_play_state_to_ui(bool playing) {
    if (!s_screen_active || s_ui.lbl_play_icon == nullptr) {
        return;
    }
    s_ui.playing = playing;
    set_play_placeholder_text(playing);
}

void async_set_play_icon(void* user_data) {
    auto* msg = static_cast<AsyncPlayStateMsg*>(user_data);
    apply_play_state_to_ui(msg->playing);
    delete msg;
}

[[maybe_unused]] void post_play_state(bool playing) {
    if (!s_screen_active) {
        return;
    }
    auto* msg = new AsyncPlayStateMsg{playing};
    screen_async_call(async_set_play_icon, msg);
}

void post_track_info(const std::string& title, const std::string& artist,
                     const std::string& album) {
    if (!s_screen_active) {
        return;
    }
    auto* msg = new AsyncTrackInfoMsg{};
    snprintf(msg->title, sizeof(msg->title), "%s", title.c_str());
    snprintf(msg->artist, sizeof(msg->artist), "%s", artist.c_str());
    snprintf(msg->album, sizeof(msg->album), "%s", album.c_str());
    screen_async_call(async_set_track_info, msg);
}

void post_lyric(const std::string& text) {
    if (!s_screen_active) {
        return;
    }
    auto* msg = new AsyncTextMsg{};
    snprintf(msg->text, sizeof(msg->text), "%s", text.c_str());
    screen_async_call(async_set_lyric, msg);
}

#if BOARD_HAS_EXTERNAL_BT
std::string trim_copy(const std::string& in) {
    size_t begin = 0;
    while (begin < in.size() && (in[begin] == ' ' || in[begin] == '\t')) {
        ++begin;
    }
    size_t end = in.size();
    while (end > begin && (in[end - 1] == ' ' || in[end - 1] == '\t')) {
        --end;
    }
    return in.substr(begin, end - begin);
}

void post_song_with_optional_artist(const std::string& data) {
    std::string title = data;
    std::string artist;
    const size_t sep = data.find(" - ") != std::string::npos
                           ? data.find(" - ")
                           : data.find('-');
    if (sep != std::string::npos) {
        title = trim_copy(data.substr(0, sep));
        const size_t sep_len = data.compare(sep, 3, " - ") == 0 ? 3 : 1;
        artist = trim_copy(data.substr(sep + sep_len));
    }
    post_track_info(title, artist, "");
}

// ---------------------------------------------------------------------------
// JSON 解析（极简）—— 只支持下面这两种行：
//   {"type":"song",  "data":"..."}
//   {"type":"lyrics","data":"..."}
// 实际数据来自手机回传，data 字段是 UTF-8 字符串，不会包含转义符号或
// 嵌套结构。这里直接做字符串查找，避开引入完整 JSON 解析器的开销。
// ---------------------------------------------------------------------------
bool extract_quoted_value(const std::string& line, const std::string& key,
                          std::string& out) {
    const std::string pattern = "\"" + key + "\"";
    size_t p = line.find(pattern);
    if (p == std::string::npos) {
        return false;
    }
    size_t colon = line.find(':', p + pattern.size());
    if (colon == std::string::npos) {
        return false;
    }
    size_t quote_open = line.find('"', colon + 1);
    if (quote_open == std::string::npos) {
        return false;
    }
    size_t quote_close = line.find('"', quote_open + 1);
    if (quote_close == std::string::npos) {
        return false;
    }
    out = line.substr(quote_open + 1, quote_close - quote_open - 1);
    return true;
}

void handle_json_line(const std::string& line) {
    if (line.empty() || line.front() != '{') {
        return;
    }
    std::string type;
    std::string data;
    if (!extract_quoted_value(line, "type", type)) {
        return;
    }
    if (!extract_quoted_value(line, "data", data)) {
        return;
    }
    if (type == "song") {
        ESP_LOGI(TAG, "song: %s", data.c_str());
        post_song_with_optional_artist(data);
    } else if (type == "lyrics") {
        ESP_LOGI(TAG, "lyrics: %s", data.c_str());
        post_lyric(data);
    }
}

// BT 模块在切换播放状态时会主动回包，行里通常包含 "MPLAY" 或 "MPAUSE"
// 字样（不强制是独立行，可能是 +EVT:MPLAY / OK MPAUSE 之类的格式）。
// 用 substring 匹配兼容所有形式。MPAUSE 必须先匹配，因为 "MPLAY" 不是
// "MPAUSE" 的子串、而判断顺序对结果有意义。
void handle_play_state_line(const std::string& line) {
    if (line.find("MPAUSE") != std::string::npos) {
        ESP_LOGI(TAG, "BT report: paused");
        post_play_state(false);
        return;
    }
    if (line.find("MPLAY") != std::string::npos) {
        ESP_LOGI(TAG, "BT report: playing");
        post_play_state(true);
    }
}

void handle_line(const std::string& line) {
    if (line.empty()) {
        return;
    }
    if (line.front() == '{') {
        handle_json_line(line);
    } else {
        handle_play_state_line(line);
    }
}

void on_uart_data(const std::vector<uint8_t>& data) {
    s_rx_buffer.append(data.begin(), data.end());

    size_t pos = 0;
    while (true) {
        size_t nl = s_rx_buffer.find('\n', pos);
        if (nl == std::string::npos) {
            break;
        }
        std::string line = s_rx_buffer.substr(pos, nl - pos);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        handle_line(line);
        pos = nl + 1;
    }
    if (pos > 0) {
        s_rx_buffer.erase(0, pos);
    }
    if (s_rx_buffer.size() > 2048) {
        ESP_LOGW(TAG, "RX buffer overflow, clearing");
        s_rx_buffer.clear();
    }
}

// ---------------------------------------------------------------------------
// 蓝牙模式三切换 task：BT 模块需要 AT+RX=1 + 700ms 间隔 + AT+MODE=3。
// ---------------------------------------------------------------------------
void switch_to_mode3_task(void* /*arg*/) {
    SimpleUart& uart = SimpleUart::getInstance();
    if (!uart.isInitialized()) {
        ESP_LOGE(TAG, "UART not initialized, cannot switch BT to mode 3");
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "TX: AT+RX=1");
    uart.sendString("AT+RX=1\r\n");
    vTaskDelay(pdMS_TO_TICKS(700));
    ESP_LOGI(TAG, "TX: AT+MODE=3");
    uart.sendString("AT+MODE=3\r\n");
    vTaskDelete(nullptr);
}

// 退出音乐界面：BT 模块需要先 AT+RX=2，延时 700ms，再 AT+MODE=1，
// 把模块从音乐接收模式切回普通模式。
void switch_to_mode1_task(void* /*arg*/) {
    SimpleUart& uart = SimpleUart::getInstance();
    if (!uart.isInitialized()) {
        ESP_LOGE(TAG, "UART not initialized, cannot switch BT to mode 1");
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "TX: AT+RX=2");
    uart.sendString("AT+RX=2\r\n");
    vTaskDelay(pdMS_TO_TICKS(700));
    ESP_LOGI(TAG, "TX: AT+MODE=1");
    uart.sendString("AT+MODE=1\r\n");
    vTaskDelete(nullptr);
}

void send_at(const char* cmd) {
    SimpleUart& uart = SimpleUart::getInstance();
    if (!uart.isInitialized()) {
        ESP_LOGW(TAG, "UART not initialized, drop cmd: %s", cmd);
        return;
    }
    ESP_LOGI(TAG, "TX: %s", cmd);
    uart.sendString(cmd);
}
#else
bool native_bt_heap_ready() {
    const size_t internal_free =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return internal_free >= kNativeBtMinInternalFree &&
           largest >= kNativeBtMinLargestBlock;
}

void queue_native_bt_track_info(const std::string& title,
                                const std::string& artist,
                                const std::string& album) {
    std::lock_guard<std::mutex> lock(s_native_bt_ui_mutex);
    s_native_bt_ui_cache.title = title.empty() ? I18n::T("蓝牙音乐") : title;
    s_native_bt_ui_cache.artist = artist;
    s_native_bt_ui_cache.album = album;
    s_native_bt_ui_cache.track_dirty = true;
}

void queue_native_bt_state(bool connected, bool playing) {
    std::lock_guard<std::mutex> lock(s_native_bt_ui_mutex);
    s_native_bt_ui_cache.connected = connected;
    s_native_bt_ui_cache.playing = playing;
    s_native_bt_ui_cache.play_dirty = true;
    if (!connected) {
        s_native_bt_ui_cache.status = I18n::T("等待手机连接本机蓝牙");
    } else if (playing) {
        s_native_bt_ui_cache.status = I18n::T("蓝牙音乐正在播放");
    } else {
        s_native_bt_ui_cache.status = I18n::T("蓝牙已连接，等待手机播放");
    }
    s_native_bt_ui_cache.status_dirty = true;
}

void OnNativeBtUiTimer(lv_timer_t* /*timer*/) {
    NativeBtUiCache cache;
    {
        std::lock_guard<std::mutex> lock(s_native_bt_ui_mutex);
        cache = s_native_bt_ui_cache;
        s_native_bt_ui_cache.track_dirty = false;
        s_native_bt_ui_cache.status_dirty = false;
        s_native_bt_ui_cache.play_dirty = false;
    }

    if (!s_screen_active) {
        return;
    }
    if (cache.track_dirty) {
        apply_track_info_to_ui(cache.title.c_str(), cache.artist.c_str(),
                               cache.album.c_str());
    }
    if (cache.play_dirty) {
        apply_play_state_to_ui(cache.playing);
    }
    if (cache.status_dirty && !cache.status.empty()) {
        push_lyric_line(cache.status.c_str());
    }
}

void restore_audio_service_after_native_bt() {
    auto& audio_service = Application::GetInstance().GetAudioService();
    if (s_restart_audio_service_after_native_bt || !audio_service.IsStarted()) {
        ESP_LOGI(TAG, "restore audio service after native BT");
        audio_service.Start();
    }
    if (s_restore_wake_word_after_native_bt || !audio_service.IsWakeWordRunning()) {
        ESP_LOGI(TAG, "restore wake word after native BT");
        if (Application::GetInstance().IsVoiceChatAllowed()) {
            audio_service.EnableWakeWordDetection(true);
        }
    }
    s_restart_audio_service_after_native_bt = false;
    s_restore_wake_word_after_native_bt = false;
}

void on_native_bt_metadata_changed(const NativeBluetoothAudio::Metadata& metadata) {
    const std::string title =
        metadata.title != nullptr && metadata.title[0] != '\0'
            ? metadata.title
            : I18n::T("蓝牙音乐");
    const std::string artist =
        metadata.artist != nullptr ? metadata.artist : "";
    const std::string album =
        metadata.album != nullptr ? metadata.album : "";
    queue_native_bt_track_info(title, artist, album);
}

void on_native_bt_state_changed(bool connected, bool playing) {
    queue_native_bt_state(connected, playing);
}
#endif

// ---------------------------------------------------------------------------
// 控件回调
// ---------------------------------------------------------------------------
void OnPrevClicked(lv_event_t* /*e*/) {
#if BOARD_HAS_EXTERNAL_BT
    send_at("AT+PREV\r\n");
#else
    NativeBluetoothAudio::GetInstance().SendCommand(NativeBluetoothAudio::Command::kPrevious);
#endif
}

void OnNextClicked(lv_event_t* /*e*/) {
#if BOARD_HAS_EXTERNAL_BT
    send_at("AT+NEXT\r\n");
#else
    NativeBluetoothAudio::GetInstance().SendCommand(NativeBluetoothAudio::Command::kNext);
#endif
}

void OnPlayClicked(lv_event_t* /*e*/) {
    // 按钮图标语义 = 「点了之后的动作」。
    //   - 正在播放（playing=true，图标=❚❚） -> 点 = 暂停 -> 发 MPAUSE
    //   - 已暂停  （playing=false，图标=▶ ） -> 点 = 播放 -> 发 MPLAY
    // 先乐观切图标，BT 模块随后会回包 MPLAY / MPAUSE 让 handle_play_state_line
    // 做最终对齐，万一命令丢了也能恢复。
    const bool want_playing = !s_ui.playing;
#if BOARD_HAS_EXTERNAL_BT
    send_at(want_playing ? "AT+MPLAY=1\r\n" : "AT+MPAUSE=1\r\n");
#else
    NativeBluetoothAudio::GetInstance().SendCommand(
        want_playing ? NativeBluetoothAudio::Command::kPlay
                     : NativeBluetoothAudio::Command::kPause);
#endif
    s_ui.playing = want_playing;
    set_play_placeholder_text(want_playing);
}

void OnVolDownClicked(lv_event_t* /*e*/) {
#if BOARD_HAS_EXTERNAL_BT
    send_at("AT+VOLDOWN\r\n");
#else
    NativeBluetoothAudio::GetInstance().SendCommand(NativeBluetoothAudio::Command::kVolumeDown);
#endif
}

void OnVolUpClicked(lv_event_t* /*e*/) {
#if BOARD_HAS_EXTERNAL_BT
    send_at("AT+VOLUP\r\n");
#else
    NativeBluetoothAudio::GetInstance().SendCommand(NativeBluetoothAudio::Command::kVolumeUp);
#endif
}

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
#if !BOARD_HAS_EXTERNAL_BT
    if (s_ui.native_bt_ui_timer != nullptr) {
        lv_timer_delete(s_ui.native_bt_ui_timer);
        s_ui.native_bt_ui_timer = nullptr;
    }
#endif
    s_ui = MusicUi{};
}

// ---------------------------------------------------------------------------
// UI 构造
// ---------------------------------------------------------------------------
// 占位圆钮：外圈 = 可点区域，内圈 = 将来图标目标尺寸，中间文字 = 功能提示。
// 返回中间 label，播放键可用来切换 > / II。
lv_obj_t* CreatePlaceholderButton(lv_obj_t* parent, int32_t size,
                                  int32_t icon_size, uint32_t bg_color,
                                  uint32_t bg_pressed, uint32_t icon_bg,
                                  const char* mark, lv_event_cb_t cb) {
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

    lv_obj_t* icon_box = lv_obj_create(btn);
    lv_obj_set_size(icon_box, icon_size, icon_size);
    screen_strip_obj_chrome(icon_box);
    lv_obj_remove_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(icon_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(icon_box, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(icon_box, lv_color_hex(icon_bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(icon_box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(icon_box, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(icon_box, 0, LV_PART_MAIN);
    lv_obj_center(icon_box);

    lv_obj_t* lbl = lv_label_create(icon_box);
    lv_label_set_text(lbl, mark);
    lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(kColorTextPrimary),
                                LV_PART_MAIN);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_center(lbl);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    return lbl;
}

void BuildBackButton(lv_obj_t* scr) {
    lv_obj_t* back_btn = lv_button_create(scr);
    lv_obj_remove_style_all(back_btn);
    lv_obj_set_size(back_btn, kBackBtnSize, kBackBtnSize);
    if constexpr (kRoundLayout) {
        lv_obj_set_style_bg_color(back_btn, lv_color_hex(kColorBackBtnBg),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_opa(back_btn, LV_OPA_70, LV_PART_MAIN);
        lv_obj_set_style_border_width(back_btn, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(back_btn, lv_color_hex(0xFFFFFF),
                                      LV_PART_MAIN);
        lv_obj_set_style_border_opa(back_btn, LV_OPA_30, LV_PART_MAIN);
    } else {
        lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(back_btn, 0, LV_PART_MAIN);
    }
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xFFFFFF),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_30,
                            Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_radius(back_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back_btn, 0, LV_PART_MAIN);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, kBackBtnX, kBackBtnY);
    lv_obj_set_ext_click_area(back_btn, 12);
    // 返回按钮自身的点击不应被全屏右滑手势拦截。
    screen_swipe_back_ignore(back_btn, true);

    lv_obj_t* back_icon = lv_image_create(back_btn);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    lv_obj_add_event_cb(
        back_btn, [](lv_event_t* /*e*/) { OnSwipeBack(); },
        LV_EVENT_CLICKED, nullptr);
}

void BuildUsageHint(lv_obj_t* scr) {
    // 底部弱提示：蓝牙音箱怎么用。控制键已上移到中间偏下，这里留到底边即可。
    lv_obj_t* hint = lv_label_create(scr);
    lv_label_set_text(
        hint,
        I18n::T("蓝牙音箱模式 · 手机蓝牙连接本设备后，用手机音乐 App 播放歌曲"));
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x8B92A3), LV_PART_MAIN);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);
    lv_obj_set_width(hint, kPanelSize - 60);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -kHintBottomMargin);
    screen_make_input_passive(hint);
}

void BuildSongTitle(lv_obj_t* scr) {
    s_ui.lbl_song = lv_label_create(scr);
    // 默认占位文本，等手机回传 song 字段时被覆盖。
    lv_label_set_text(s_ui.lbl_song, I18n::T("蓝牙音乐"));
    lv_obj_set_style_text_font(s_ui.lbl_song,
                               kRoundLayout ? &font_puhui_20_4 : &font_puhui_30_4,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.lbl_song, lv_color_hex(kColorTextPrimary),
                                LV_PART_MAIN);
    lv_obj_set_style_text_align(s_ui.lbl_song, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(s_ui.lbl_song, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_ui.lbl_song, kPanelSize - (kRoundLayout ? 64 : 80));
    lv_obj_align(s_ui.lbl_song, LV_ALIGN_TOP_MID, 0, kTitleY);
    screen_make_input_passive(s_ui.lbl_song);

    s_ui.lbl_artist = lv_label_create(scr);
    lv_label_set_text(s_ui.lbl_artist, "");
    lv_obj_set_style_text_font(s_ui.lbl_artist, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.lbl_artist, lv_color_hex(0xD4DAE8),
                                LV_PART_MAIN);
    lv_obj_set_style_text_align(s_ui.lbl_artist, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(s_ui.lbl_artist, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_ui.lbl_artist, kPanelSize - 120);
    lv_obj_align(s_ui.lbl_artist, LV_ALIGN_TOP_MID, 0, kArtistY);
    lv_obj_add_flag(s_ui.lbl_artist, LV_OBJ_FLAG_HIDDEN);
    screen_make_input_passive(s_ui.lbl_artist);

    s_ui.lbl_album = lv_label_create(scr);
    lv_label_set_text(s_ui.lbl_album, "");
    lv_obj_set_style_text_font(s_ui.lbl_album, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.lbl_album, lv_color_hex(0x8B92A3),
                                LV_PART_MAIN);
    lv_obj_set_style_text_align(s_ui.lbl_album, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(s_ui.lbl_album, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_ui.lbl_album, kPanelSize - 140);
    lv_obj_align(s_ui.lbl_album, LV_ALIGN_TOP_MID, 0, kAlbumInfoY);
    lv_obj_add_flag(s_ui.lbl_album, LV_OBJ_FLAG_HIDDEN);
    screen_make_input_passive(s_ui.lbl_album);
}

void BuildLyric(lv_obj_t* scr) {
    for (int i = 0; i < kLyricLineCount; ++i) {
        lv_obj_t* lbl = lv_label_create(scr);
        lv_label_set_text(lbl, "");
        lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl, lv_color_hex(kColorAccent),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, kLyricLineWidth);
        lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0,
                     kLyricY + i * kLyricLineGap);
        // 三行各自的初始 opacity：顶 100%、中 60%、底 30%。
        lv_obj_set_style_opa(lbl, kLyricTargetOpa[i], LV_PART_MAIN);
        screen_make_input_passive(lbl);
        s_ui.lbl_lyric[i] = lbl;
    }
}

void BuildControls(lv_obj_t* scr) {
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

    // 占位预览：内圈尺寸 = 建议图标边长（圆屏 34 / 播放 44）。
    // 顺序：音量减 / 上一曲 / 播放暂停 / 下一曲 / 音量加
    CreatePlaceholderButton(row, kCtrlSideBtnSize, kCtrlSideIconSize,
                            kColorCtrlBtnBg, kColorCtrlBtnBgPressed,
                            kColorIconPlaceholder, "-", OnVolDownClicked);
    CreatePlaceholderButton(row, kCtrlSideBtnSize, kCtrlSideIconSize,
                            kColorCtrlBtnBg, kColorCtrlBtnBgPressed,
                            kColorIconPlaceholder, "<<", OnPrevClicked);
    s_ui.lbl_play_icon = CreatePlaceholderButton(
        row, kCtrlPlayBtnSize, kCtrlPlayIconSize, kColorPlayBtnBg,
        kColorPlayBtnBgPressed, kColorIconPlaceholderPlay, ">", OnPlayClicked);
    CreatePlaceholderButton(row, kCtrlSideBtnSize, kCtrlSideIconSize,
                            kColorCtrlBtnBg, kColorCtrlBtnBgPressed,
                            kColorIconPlaceholder, ">>", OnNextClicked);
    CreatePlaceholderButton(row, kCtrlSideBtnSize, kCtrlSideIconSize,
                            kColorCtrlBtnBg, kColorCtrlBtnBgPressed,
                            kColorIconPlaceholder, "+", OnVolUpClicked);

    // 底部一行尺寸标注，方便对照找图标。
    lv_obj_t* size_hint = lv_label_create(scr);
    char buf[64];
    snprintf(buf, sizeof(buf), "icon %dx%d / play %dx%d",
             static_cast<int>(kCtrlSideIconSize),
             static_cast<int>(kCtrlSideIconSize),
             static_cast<int>(kCtrlPlayIconSize),
             static_cast<int>(kCtrlPlayIconSize));
    lv_label_set_text(size_hint, buf);
    lv_obj_set_style_text_font(size_hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(size_hint, lv_color_hex(0x8B92A3), LV_PART_MAIN);
    lv_obj_set_style_text_align(size_hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(size_hint, LV_ALIGN_TOP_MID, 0,
                 kCtrlRowY + kCtrlRowHeight + 8);
    screen_make_input_passive(size_hint);
}

}  // namespace

lv_obj_t* MusicScreen::Create() {
#if defined(BOARD_ESP_VOCAT)
    return MusicScreenSd::Create();
#else
    s_ui = MusicUi{};
    // 进入界面默认按钮是"▶ 播放"，等用户点一次才进入播放状态。
    s_ui.playing = false;
    s_rx_buffer.clear();

    lv_obj_t* scr = lv_obj_create(nullptr);
    screen_strip_obj_chrome(scr);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(kColorBgGrad), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    BuildSongTitle(scr);
    BuildUsageHint(scr);
    BuildLyric(scr);
    BuildControls(scr);
#if BOARD_HAS_NATIVE_BT
    {
        std::lock_guard<std::mutex> lock(s_native_bt_ui_mutex);
        s_native_bt_ui_cache = NativeBtUiCache{};
    }
    s_ui.native_bt_ui_timer = lv_timer_create(OnNativeBtUiTimer, 200, nullptr);
#endif
    // BackButton 最后建，保证它在 z-order 顶层、可被点中。
    BuildBackButton(scr);

    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);
    screen_attach_swipe_back(scr, OnSwipeBack);

    s_screen_active = true;
    return scr;
#endif
}

void MusicScreen::LifecycleCallback(screen_lifecycle_event_t event) {
#if defined(BOARD_ESP_VOCAT)
    MusicScreenSd::LifecycleCallback(event);
#else
    if (event == SCREEN_LIFECYCLE_LOAD) {
#if BOARD_HAS_EXTERNAL_BT
        ESP_LOGI(TAG, "load: music_screen -> switching BT to mode 3");
        // 让 BT 模块切到「音乐接收」模式三；命令需要 700ms 间隔，放后台 task。
        xTaskCreate(switch_to_mode3_task, "mus_mode3", 4096, nullptr, 5, nullptr);
        // 注册 UART RX 回调，开始监听手机回传的 JSON。
        s_rx_buffer.clear();
        SimpleUart::getInstance().registerCallback(on_uart_data);
#elif BOARD_HAS_NATIVE_BT
        ESP_LOGI(TAG, "load: music_screen -> native BT speaker mode");
        s_rx_buffer.clear();
        auto& audio_service = Application::GetInstance().GetAudioService();
        s_restart_audio_service_after_native_bt = true;
        s_restore_wake_word_after_native_bt = audio_service.ReleaseWakeWordDetection();
        if (audio_service.IsStarted()) {
            if (!audio_service.StopAndWait(1200)) {
                ESP_LOGW(TAG, "audio service did not fully stop before native BT init");
            }
        }
        if (auto* codec = Board::GetInstance().GetAudioCodec()) {
            codec->EnableInput(false);
        }
        vTaskDelay(pdMS_TO_TICKS(180));
        ESP_LOGI(TAG, "before native BT init: internal free=%u largest=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        auto& bt = NativeBluetoothAudio::GetInstance();
        if (!bt.IsSupported()) {
            ESP_LOGW(TAG, "native BT not enabled in sdkconfig");
            post_lyric(I18n::T("内置蓝牙未启用，请检查固件配置"));
            restore_audio_service_after_native_bt();
            return;
        }
        if (!bt.IsInitialized() && !native_bt_heap_ready()) {
            ESP_LOGE(TAG, "not enough internal heap for native BT");
            post_lyric(I18n::T("内存不足，蓝牙音乐暂不可用"));
            restore_audio_service_after_native_bt();
            return;
        }
        bt.SetStateCallback(on_native_bt_state_changed);
        bt.SetMetadataCallback(on_native_bt_metadata_changed);
        if (bt.SetMode(NativeBluetoothAudio::Mode::kSpeakerSink)) {
            char title[96];
            snprintf(title, sizeof(title), "%s", bt.DeviceName());
            post_track_info(title, "", "");
            on_native_bt_state_changed(bt.IsConnected(), bt.IsPlaying());
        } else {
            post_lyric(I18n::T("内置蓝牙未启用，请检查固件配置"));
            restore_audio_service_after_native_bt();
        }
#else
        // VoCat 等无蓝牙板：禁止走 native BT 初始化（会在 UI 线程
        // ReleaseWakeWord/StopAndWait，容易整屏卡死）。
        ESP_LOGI(TAG, "load: music_screen -> BT music unsupported on this board");
        s_rx_buffer.clear();
        post_track_info(I18n::T("音乐"), "", "");
        post_lyric(I18n::T("本机不支持蓝牙音乐"));
#endif
    } else {
#if BOARD_HAS_EXTERNAL_BT
        ESP_LOGI(TAG, "unload: music_screen -> switching BT back to mode 1");
        // 摘掉回调，避免 UART task 仍向已经销毁的 UI 投递更新。
        SimpleUart::getInstance().registerCallback(
            std::function<void(const std::vector<uint8_t>&)>());
        s_screen_active = false;
        s_rx_buffer.clear();
        // 切回模式 1 同样需要 700ms 间隔，放后台 task 异步执行。
        xTaskCreate(switch_to_mode1_task, "mus_mode1", 4096, nullptr, 5, nullptr);
#elif BOARD_HAS_NATIVE_BT
        ESP_LOGI(TAG, "unload: music_screen -> native BT suspend");
        auto& bt = NativeBluetoothAudio::GetInstance();
        bt.SetStateCallback(nullptr);
        bt.SetMetadataCallback(nullptr);
        bt.Suspend();
        restore_audio_service_after_native_bt();
        s_screen_active = false;
        s_rx_buffer.clear();
#else
        ESP_LOGI(TAG, "unload: music_screen");
        s_screen_active = false;
        s_rx_buffer.clear();
#endif
    }
#endif
}
