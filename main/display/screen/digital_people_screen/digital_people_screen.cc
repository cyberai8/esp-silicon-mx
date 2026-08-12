#include "digital_people_screen.h"
#include "config.h"
#include "i18n.h"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lv_eaf.h"

#include "application.h"
#include "audio_service.h"
#include "device_state.h"
#include "SdCardManager.hpp"
#include "home_screen/home_screen.h"
#include "screen_util.h"

LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_puhui_20_4);

namespace {

constexpr const char* TAG = "DigitalPeopleScreen";

#if defined(BOARD_ESP_VOCAT) || (DISPLAY_WIDTH == 360 && DISPLAY_HEIGHT == 360)
constexpr bool    kRoundLayout = true;
constexpr int32_t kPanelSize   = DISPLAY_WIDTH;
#else
constexpr bool    kRoundLayout = false;
constexpr int32_t kPanelSize   = 720;
#endif
constexpr uint32_t kColorBg      = 0x000000;          // 纯黑背景

// 表情资源目录与扩展名（改 DIGITAL_PEOPLE_EMOTION_EXT 切换格式）：
//   完整路径 = kEmotionDir + 大类名 + kEmotionExt
//   例: "S:/sdcard/system/emotion/loving.eaf"
// 大类名来自 LVAdapterDisplay::SetEmotion 里的 GetEmoteCategory()，
// 取值范围被收敛到 6 个：crying / happy / loving / neutral / surprised /
// thinking。所有资源都放在 SD 卡的 system/emotion/ 目录下。
// .eaf 可用官方工具从 GIF 转换：https://esp32-gif.espressif.com/
constexpr const char* kEmotionDir = "S:/sdcard/system/emotion/";
constexpr const char* kEmotionPosixDir = "/sdcard/system/emotion/";
constexpr const char* kEmotionExt = DIGITAL_PEOPLE_EMOTION_EXT;
constexpr const char* kDefaultEmotion = "neutral";

// 端侧必须存在的 6 个大类资源；缺任意一个都视为资源包未就绪。
constexpr const char* kRequiredEmotions[] = {
    "crying", "happy", "loving", "neutral", "surprised", "thinking",
};
constexpr size_t kRequiredEmotionCount =
    sizeof(kRequiredEmotions) / sizeof(kRequiredEmotions[0]);

// 记录"当前应该播放哪一张"——LVAdapterDisplay 在屏幕没进前台时也可以
// 调用 SetEmotion 预置；下次 Create() 拿这个值拼路径。
// s_emotion_path_buf 是 lv_eaf_set_src / lv_image_set_src 传入的路径缓冲，
// 必须保证在调用之间一直有效，所以放在 namespace 静态。
constexpr size_t kEmotionPathBufSize = 64;
// 切换表情会整文件读入 PSRAM（常见 200~300KB）+ 解码首帧，瞬时电流高。
// 与功放开声叠在一起容易拉垮电源触发 Brownout，因此：
// 1) 同名表情不重复加载；2) 真正读卡延后一小段，避开 codec 开声尖峰。
constexpr uint32_t kEmotionLoadDelayMs = 180;
// 全屏 360×360 EAF 解码较重；略降帧率减轻 core 1 压力，给 AFE fetch 留余量。
constexpr uint32_t kEmotionFrameDelayMs = 66;
char s_current_emotion[24] = "neutral";
char s_applied_emotion[24] = "";
char s_pending_emotion[24] = "";
char s_emotion_path_buf[kEmotionPathBufSize];
lv_timer_t* s_emotion_load_timer = nullptr;

struct UiState {
    lv_obj_t* screen        = nullptr;
    lv_obj_t* eaf           = nullptr;
    lv_obj_t* hint_label    = nullptr;
    lv_obj_t* speech_bubble = nullptr;
    lv_obj_t* speech_label  = nullptr;
};

UiState s_ui;

constexpr size_t kSpeechTextMax = 384;
constexpr uint32_t kSpeechCarouselPeriodMs = 3500;
char s_user_speech[kSpeechTextMax] = "";
char s_system_speech[kSpeechTextMax] = "";
bool s_speech_show_user = true;
lv_timer_t* s_speech_carousel_timer = nullptr;

// 切换表情读入 PSRAM + 首帧解码时短暂关唤醒词，避免瞬时 feed 堆积。
struct WakeWordGuard {
    AudioService& as;
    bool disabled = false;
    explicit WakeWordGuard(AudioService& audio) : as(audio) {
        if (as.IsWakeWordRunning()) {
            as.EnableWakeWordDetection(false);
            disabled = true;
            vTaskDelay(pdMS_TO_TICKS(150));
        }
    }
    ~WakeWordGuard() {
        if (disabled) {
            as.EnableWakeWordDetection(true);
        }
    }
    WakeWordGuard(const WakeWordGuard&) = delete;
    WakeWordGuard& operator=(const WakeWordGuard&) = delete;
};

const char* EmotionCategoryName(const char* category) {
    return (category != nullptr && category[0] != '\0') ? category
                                                        : kDefaultEmotion;
}

const char* BuildEmotionPath(const char* category) {
    std::snprintf(s_emotion_path_buf, sizeof(s_emotion_path_buf), "%s%s%s",
                  kEmotionDir, EmotionCategoryName(category), kEmotionExt);
    return s_emotion_path_buf;
}

bool EmotionUsesEaf() { return std::strcmp(kEmotionExt, ".eaf") == 0; }

void CancelPendingEmotionLoad() {
    if (s_emotion_load_timer != nullptr) {
        lv_timer_delete(s_emotion_load_timer);
        s_emotion_load_timer = nullptr;
    }
    s_pending_emotion[0] = '\0';
}

void SetEmotionSrc(lv_obj_t* widget, const char* category) {
    const char* name = EmotionCategoryName(category);
    const char* path = BuildEmotionPath(name);
    ESP_LOGI(TAG, "set emotion src: %s", path);

    // 先用 POSIX 确认文件可读；LVGL 走 S: 盘符（需 CONFIG_LV_USE_FS_POSIX）。
    char posix_path[96];
    std::snprintf(posix_path, sizeof(posix_path), "%s%s%s", kEmotionPosixDir,
                  name, kEmotionExt);
    FILE* fp = std::fopen(posix_path, "rb");
    if (fp == nullptr) {
        ESP_LOGE(TAG, "emotion fopen failed: %s", posix_path);
        return;
    }
    if (std::fseek(fp, 0, SEEK_END) != 0) {
        ESP_LOGE(TAG, "emotion fseek failed: %s", posix_path);
        std::fclose(fp);
        return;
    }
    const long file_size = std::ftell(fp);
    std::fclose(fp);
    if (file_size <= 0) {
        ESP_LOGE(TAG, "emotion empty: %s", posix_path);
        return;
    }
    ESP_LOGI(TAG, "emotion file ok: %s (%ld bytes)", posix_path, file_size);

    WakeWordGuard guard(Application::GetInstance().GetAudioService());
    if (EmotionUsesEaf()) {
        lv_eaf_set_src(widget, path);
        lv_eaf_set_loop_count(widget, -1);  // 无限循环
        lv_eaf_set_frame_delay(widget, kEmotionFrameDelayMs);
    } else {
        lv_image_set_src(widget, path);
    }
    std::strncpy(s_applied_emotion, name, sizeof(s_applied_emotion) - 1);
    s_applied_emotion[sizeof(s_applied_emotion) - 1] = '\0';
}

void OnEmotionLoadTimer(lv_timer_t* /*t*/) {
    s_emotion_load_timer = nullptr;
    if (s_ui.eaf == nullptr || s_pending_emotion[0] == '\0') {
        return;
    }
    char name[sizeof(s_pending_emotion)];
    std::strncpy(name, s_pending_emotion, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    s_pending_emotion[0] = '\0';
    if (s_applied_emotion[0] != '\0' &&
        std::strcmp(s_applied_emotion, name) == 0) {
        return;
    }
    SetEmotionSrc(s_ui.eaf, name);
}

void ScheduleEmotionLoad(const char* category) {
    const char* name = EmotionCategoryName(category);
    if (s_ui.eaf == nullptr) {
        return;
    }
    if (s_applied_emotion[0] != '\0' &&
        std::strcmp(s_applied_emotion, name) == 0 &&
        s_pending_emotion[0] == '\0') {
        ESP_LOGD(TAG, "emotion already applied: %s", name);
        return;
    }
    std::strncpy(s_pending_emotion, name, sizeof(s_pending_emotion) - 1);
    s_pending_emotion[sizeof(s_pending_emotion) - 1] = '\0';
    if (s_emotion_load_timer != nullptr) {
        lv_timer_reset(s_emotion_load_timer);
        return;
    }
    s_emotion_load_timer =
        lv_timer_create(OnEmotionLoadTimer, kEmotionLoadDelayMs, nullptr);
    lv_timer_set_repeat_count(s_emotion_load_timer, 1);
}

lv_obj_t* CreateEmotionWidget(lv_obj_t* parent) {
    if (EmotionUsesEaf()) {
        lv_obj_t* eaf = lv_eaf_create(parent);
        lv_eaf_set_frame_delay(eaf, kEmotionFrameDelayMs);
        return eaf;
    }
    return lv_image_create(parent);
}

// ---------------------------------------------------------------------------
// 底部单行字幕气泡
//
//   ┌─────────────────────────────────────────┐
//   │            (EAF 表情动画)                 │
//   │        ╭─ speech bubble ─╮              │ ← bottom-center，单行
//   │        ╰──────────────────╯              │   过长横向滚动；用户/设备轮流
//   └─────────────────────────────────────────┘
// ---------------------------------------------------------------------------
#if defined(BOARD_ESP_VOCAT) || (DISPLAY_WIDTH == 360 && DISPLAY_HEIGHT == 360)
constexpr int32_t kSideMargin         = 40;
constexpr int32_t kSpeechBubbleBottom = 36;
#else
constexpr int32_t kSideMargin         = 16;
constexpr int32_t kSpeechBubbleBottom = 24;
#endif
constexpr int32_t kSpeechBubbleMaxW = kPanelSize - kSideMargin * 2;

constexpr uint32_t kColorSpeechText = 0xFFFFFF;
constexpr uint32_t kColorHintText   = 0xC8C9CC;

lv_timer_t* s_activation_guard_timer = nullptr;

// 未激活拦截：全屏模态弹窗，不可关闭，仅能通过返回键离开。
struct ActivationBlockedDialogUi {
    lv_obj_t* mask = nullptr;
};
ActivationBlockedDialogUi s_activation_dlg;
bool s_activation_blocked = false;

const lv_font_t* bubble_font() {
    return kRoundLayout ? &font_puhui_20_4 : &font_puhui_30_4;
}

bool EmotionFileExists(const char* name) {
    char path[96];
    std::snprintf(path, sizeof(path), "%s%s%s", kEmotionPosixDir, name,
                  kEmotionExt);
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

void LogMissingEmotionFiles() {
    for (size_t i = 0; i < kRequiredEmotionCount; ++i) {
        if (!EmotionFileExists(kRequiredEmotions[i])) {
            ESP_LOGW(TAG, "missing emotion file: %s%s%s", kEmotionPosixDir,
                     kRequiredEmotions[i], kEmotionExt);
        }
    }
}

bool CheckEmotionResourcesReady() {
    if (!SdCardManager::GetInstance().IsMounted()) {
        return false;
    }
    for (size_t i = 0; i < kRequiredEmotionCount; ++i) {
        if (!EmotionFileExists(kRequiredEmotions[i])) {
            return false;
        }
    }
    return true;
}

const char* MissingResourceHintText() {
    if (!SdCardManager::GetInstance().IsMounted()) {
        return I18n::T(
            "未检测到 SD 卡\n\n请将数字人资源包放入 SD 卡\nsystem/emotion/ 目录");
    }
    return I18n::T(
        "数字人资源缺失\n\n请将 .eaf 动画复制到 SD 卡\nsystem/emotion/ 目录\n\n"
        "需包含 6 个：crying/happy/loving/\nneutral/surprised/thinking.eaf");
}

lv_obj_t* BuildMissingResourceHint(lv_obj_t* parent) {
    lv_obj_t* hint = lv_label_create(parent);
    lv_label_set_text(hint, MissingResourceHintText());
    lv_obj_set_width(hint, kPanelSize - 80);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(kColorHintText),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, bubble_font(), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 0);
    screen_make_input_passive(hint);
    return hint;
}

void StyleSpeechBubble(lv_obj_t* bubble) {
    screen_strip_obj_chrome(bubble);
    lv_obj_remove_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(bubble, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bubble, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(bubble, 0, LV_PART_MAIN);
}

// 创建底部单行字幕气泡，初始隐藏。
struct SpeechBubbleHandles {
    lv_obj_t* bubble;
    lv_obj_t* label;
};

SpeechBubbleHandles BuildSpeechBubble(lv_obj_t* parent) {
    const lv_font_t* font = bubble_font();
    const int32_t line_h  = lv_font_get_line_height(font);

    lv_obj_t* bubble = lv_obj_create(parent);
    StyleSpeechBubble(bubble);
    lv_obj_set_width(bubble, kSpeechBubbleMaxW);
    lv_obj_set_height(bubble, line_h);
    lv_obj_align(bubble, LV_ALIGN_BOTTOM_MID, 0, -kSpeechBubbleBottom);
    lv_obj_add_flag(bubble, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* label = lv_label_create(bubble);
    lv_obj_set_width(label, kSpeechBubbleMaxW);
    lv_obj_set_height(label, line_h);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(label, "");
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(kColorSpeechText),
                                LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

    screen_make_input_passive(bubble);
    return {bubble, label};
}

void StopSpeechCarouselTimer() {
    if (s_speech_carousel_timer != nullptr) {
        lv_timer_delete(s_speech_carousel_timer);
        s_speech_carousel_timer = nullptr;
    }
}

void ClearSpeechStorage() {
    s_user_speech[0]   = '\0';
    s_system_speech[0] = '\0';
    s_speech_show_user = true;
}

void HideSpeechBubble() {
    if (s_ui.speech_bubble == nullptr) {
        return;
    }
    if (s_ui.speech_label != nullptr) {
        lv_label_set_text(s_ui.speech_label, "");
    }
    lv_obj_add_flag(s_ui.speech_bubble, LV_OBJ_FLAG_HIDDEN);
}

void ApplySpeechToLabel(const char* text) {
    if (s_ui.speech_label == nullptr || s_ui.speech_bubble == nullptr ||
        text == nullptr) {
        return;
    }

    const lv_font_t* font = bubble_font();
    const int32_t inner_max_w = kSpeechBubbleMaxW;
    const int32_t line_h = lv_font_get_line_height(font);
    int32_t text_w =
        lv_txt_get_width(text, std::strlen(text), font, 0);
    if (text_w < 1) {
        text_w = 1;
    }

    lv_label_set_text(s_ui.speech_label, text);

    if (text_w <= inner_max_w) {
        int32_t bubble_w = text_w;
        if (bubble_w < 48) {
            bubble_w = 48;
        }
        const int32_t inner_w = bubble_w;
        lv_obj_set_width(s_ui.speech_bubble, bubble_w);
        lv_obj_set_width(s_ui.speech_label, inner_w);
        lv_obj_set_height(s_ui.speech_label, line_h);
        lv_label_set_long_mode(s_ui.speech_label, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(s_ui.speech_label, LV_TEXT_ALIGN_CENTER,
                                    LV_PART_MAIN);
        lv_obj_align(s_ui.speech_label, LV_ALIGN_CENTER, 0, 0);
    } else {
        // 长文案：气泡拉满宽度，横向循环滚动。
        lv_obj_set_width(s_ui.speech_bubble, kSpeechBubbleMaxW);
        lv_obj_set_width(s_ui.speech_label, inner_max_w);
        lv_obj_set_height(s_ui.speech_label, line_h);
        lv_obj_set_style_text_align(s_ui.speech_label, LV_TEXT_ALIGN_LEFT,
                                    LV_PART_MAIN);
        lv_obj_align(s_ui.speech_label, LV_ALIGN_LEFT_MID, 0, 0);
        lv_label_set_long_mode(s_ui.speech_label, LV_LABEL_LONG_CLIP);
        lv_label_set_long_mode(s_ui.speech_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    }
}

void OnSpeechCarouselTimer(lv_timer_t* /*t*/) {
    if (s_user_speech[0] == '\0' || s_system_speech[0] == '\0') {
        StopSpeechCarouselTimer();
        return;
    }
    s_speech_show_user = !s_speech_show_user;
    ApplySpeechToLabel(s_speech_show_user ? s_user_speech : s_system_speech);
}

void StartSpeechCarouselTimer() {
    if (s_speech_carousel_timer != nullptr) {
        return;
    }
    s_speech_carousel_timer = lv_timer_create(OnSpeechCarouselTimer,
                                              kSpeechCarouselPeriodMs, nullptr);
}

void RefreshSpeechBar() {
    if (s_ui.speech_bubble == nullptr) {
        return;
    }

    if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
        ClearSpeechStorage();
        HideSpeechBubble();
        StopSpeechCarouselTimer();
        return;
    }

    const bool has_user = s_user_speech[0] != '\0';
    const bool has_sys  = s_system_speech[0] != '\0';
    if (!has_user && !has_sys) {
        HideSpeechBubble();
        StopSpeechCarouselTimer();
        return;
    }

    const char* text = nullptr;
    if (has_user && has_sys) {
        text = s_speech_show_user ? s_user_speech : s_system_speech;
        StartSpeechCarouselTimer();
    } else {
        text = has_user ? s_user_speech : s_system_speech;
        StopSpeechCarouselTimer();
    }

    ApplySpeechToLabel(text);
    lv_obj_remove_flag(s_ui.speech_bubble, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(s_ui.speech_bubble, LV_ALIGN_BOTTOM_MID, 0,
                 -kSpeechBubbleBottom);
}

void on_refresh_device_state_async(void* /*param*/) {
    RefreshSpeechBar();
}

void OnSwipeBack();

// ---------------------------------------------------------------------------
// 设备激活检查
// ---------------------------------------------------------------------------
bool is_device_activated() {
    auto& app = Application::GetInstance();
    if (app.HasPendingActivation()) {
        return false;
    }
    if (app.GetDeviceState() == kDeviceStateActivating) {
        return false;
    }
    return true;
}

void log_activation_blocked() {
    auto& app = Application::GetInstance();
    ESP_LOGW(TAG, "DigitalPeople blocked: device not activated");
    if (app.HasPendingActivation()) {
        ESP_LOGW(TAG, "pending activation code: %s",
                 app.GetPendingActivationCode().c_str());
    }
    if (app.GetDeviceState() == kDeviceStateActivating) {
        ESP_LOGW(TAG, "device state: activating");
    }
}

void open_activation_blocked_dialog() {
    if (s_ui.screen == nullptr || s_activation_dlg.mask != nullptr) {
        return;
    }

    auto& app = Application::GetInstance();
    const bool has_code = app.HasPendingActivation();

    const int32_t kCardW = kRoundLayout ? 280 : 520;
    const int32_t kCardH = kRoundLayout ? (has_code ? 240 : 200)
                                        : (has_code ? 420 : 340);
    const int32_t kBackBtnW = kRoundLayout ? 120 : 200;
    const int32_t kBackBtnH = kRoundLayout ? 44 : 72;
    const int32_t kCardPad = kRoundLayout ? 16 : 28;

    lv_obj_t* mask = lv_obj_create(s_ui.screen);
    screen_strip_obj_chrome(mask);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(mask, kPanelSize, kPanelSize);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_70, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    screen_swipe_back_ignore(mask, true);
    s_activation_dlg.mask = mask;

    lv_obj_t* card = lv_obj_create(mask);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, kCardW, kCardH);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1B2030), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, kCardPad, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* title = lv_label_create(card);
    lv_label_set_text(title, I18n::T("设备未激活"));
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(title,
                               kRoundLayout ? &font_puhui_20_4 : &font_puhui_30_4,
                               LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* desc = lv_label_create(card);
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(desc, kCardW - kCardPad * 2);
    lv_label_set_text(desc, I18n::T("请先完成设备激活后再使用数字人。"));
    lv_obj_set_style_text_color(desc, lv_color_hex(0x9AA3B2), LV_PART_MAIN);
    lv_obj_set_style_text_font(desc, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(desc, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(desc, LV_ALIGN_CENTER, 0, has_code ? -30 : -10);
    lv_obj_remove_flag(desc, LV_OBJ_FLAG_CLICKABLE);

    if (has_code) {
        char code_buf[64];
        std::snprintf(code_buf, sizeof(code_buf), I18n::T("验证码: %s"),
                      app.GetPendingActivationCode().c_str());
        lv_obj_t* code_lbl = lv_label_create(card);
        lv_label_set_text(code_lbl, code_buf);
        lv_obj_set_style_text_color(code_lbl, lv_color_hex(0xFBBF24),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_font(
            code_lbl, kRoundLayout ? &font_puhui_20_4 : &font_puhui_30_4,
            LV_PART_MAIN);
        lv_obj_align(code_lbl, LV_ALIGN_BOTTOM_MID, 0, -(kBackBtnH + 24));
        lv_obj_remove_flag(code_lbl, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t* back = lv_button_create(card);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, kBackBtnW, kBackBtnH);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x2A2F3A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(back, 16, LV_PART_MAIN);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(back,
                        [](lv_event_t* /*e*/) { OnSwipeBack(); },
                        LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);

    lv_obj_t* back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, I18n::T("返回"));
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(0xE5E7EB), LV_PART_MAIN);
    lv_obj_set_style_text_font(back_lbl,
                               kRoundLayout ? &font_puhui_20_4 : &font_puhui_30_4,
                               LV_PART_MAIN);
    lv_obj_center(back_lbl);
    lv_obj_remove_flag(back_lbl, LV_OBJ_FLAG_CLICKABLE);
}

void ensure_activation_blocked_dialog() {
    if (!s_activation_blocked) {
        return;
    }
    if (s_activation_dlg.mask == nullptr) {
        open_activation_blocked_dialog();
    }
}

void on_activation_guard_timer(lv_timer_t* /*timer*/) {
    ensure_activation_blocked_dialog();
}

void OnSwipeBack() {
    lv_indev_t* indev = lv_indev_active();
    if (indev != nullptr) {
        lv_indev_wait_release(indev);
    }
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* home    = HomeScreen::Create();
    lv_screen_load(home);
    if (old_scr != nullptr && old_scr != home) {
        lv_obj_delete_async(old_scr);
    }
}

void OnScreenUnloaded(lv_event_t* /*e*/) {
    if (s_activation_guard_timer != nullptr) {
        lv_timer_delete(s_activation_guard_timer);
        s_activation_guard_timer = nullptr;
    }
    StopSpeechCarouselTimer();
    ClearSpeechStorage();
    CancelPendingEmotionLoad();
    s_applied_emotion[0] = '\0';
    s_activation_dlg = ActivationBlockedDialogUi{};
    s_activation_blocked = false;
    s_ui.screen        = nullptr;
    s_ui.eaf           = nullptr;
    s_ui.hint_label    = nullptr;
    s_ui.speech_bubble = nullptr;
    s_ui.speech_label  = nullptr;
}

}  // namespace

lv_obj_t* DigitalPeopleScreen::Create() {
    s_activation_blocked = !is_device_activated();
    if (s_activation_blocked) {
        log_activation_blocked();
    }

    ESP_LOGI(TAG, "create digital people screen");

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_ui.screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelSize, kPanelSize);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);


    const bool resources_ready = CheckEmotionResourcesReady();
    if (!resources_ready) {
        ESP_LOGW(TAG, "emotion resources not ready (mounted=%d)",
                 SdCardManager::GetInstance().IsMounted() ? 1 : 0);
        LogMissingEmotionFiles();
        s_ui.hint_label = BuildMissingResourceHint(scr);
    } else {
        s_ui.eaf = CreateEmotionWidget(scr);
        // 真正 set_src 延后到 SCREEN_LOADED，与聊天页一致，避免 Create 阻塞过久。
        lv_image_set_inner_align(s_ui.eaf, LV_IMAGE_ALIGN_CONTAIN);
        lv_obj_center(s_ui.eaf);
        screen_make_input_passive(s_ui.eaf);
    }

    {
        SpeechBubbleHandles speech = BuildSpeechBubble(scr);
        s_ui.speech_bubble = speech.bubble;
        s_ui.speech_label  = speech.label;
    }

    screen_attach_swipe_back(scr, OnSwipeBack);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);

    if (s_activation_blocked) {
        open_activation_blocked_dialog();
        s_activation_guard_timer =
            lv_timer_create(on_activation_guard_timer, 1000, nullptr);
    }

    lv_obj_add_event_cb(scr, [](lv_event_t* e) {
        if (lv_event_get_code(e) != LV_EVENT_SCREEN_LOADED) {
            return;
        }
        if (s_activation_blocked) {
            ESP_LOGW(TAG, "screen loaded while not activated, keep dialog");
            ensure_activation_blocked_dialog();
            return;
        }
        if (s_ui.eaf != nullptr) {
            ScheduleEmotionLoad(s_current_emotion);
        }
    }, LV_EVENT_SCREEN_LOADED, nullptr);

    return scr;
}

bool DigitalPeopleScreen::IsActive() {
    return s_ui.screen != nullptr;
}

void DigitalPeopleScreen::ShowUserMessage(const char* text) {
    if (!IsActive() || text == nullptr || text[0] == '\0') return;
    if (s_activation_blocked) return;
    std::strncpy(s_user_speech, text, sizeof(s_user_speech) - 1);
    s_user_speech[sizeof(s_user_speech) - 1] = '\0';
    s_speech_show_user = true;
    RefreshSpeechBar();
}

void DigitalPeopleScreen::ShowSystemMessage(const char* text) {
    if (!IsActive() || text == nullptr || text[0] == '\0') return;
    if (s_activation_blocked) return;
    std::strncpy(s_system_speech, text, sizeof(s_system_speech) - 1);
    s_system_speech[sizeof(s_system_speech) - 1] = '\0';
    s_speech_show_user = false;
    RefreshSpeechBar();
}

void DigitalPeopleScreen::ClearMessages() {
    ClearSpeechStorage();
    HideSpeechBubble();
    StopSpeechCarouselTimer();
}

void DigitalPeopleScreen::RefreshDeviceState() {
    if (!IsActive()) {
        return;
    }
    lv_async_call(on_refresh_device_state_async, nullptr);
}

void DigitalPeopleScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    auto& audio_service = Application::GetInstance().GetAudioService();
    if (event == SCREEN_LIFECYCLE_LOAD) {
        if (!is_device_activated()) {
            ESP_LOGW(TAG,
                     "load: digital_people_screen blocked (device not activated)");
            log_activation_blocked();
        } else {
            ESP_LOGI(TAG, "load: digital_people_screen");
        }
        audio_service.EnableWakeWordDetection(true);
        RefreshDeviceState();
    } else {
        ESP_LOGI(TAG, "unload: digital_people_screen");
        Application::GetInstance().ForceReturnToIdle();
        audio_service.EnableWakeWordDetection(false);
    }
}

void DigitalPeopleScreen::SetEmotion(const char* category) {
    if (category == nullptr || category[0] == '\0') {
        category = kDefaultEmotion;
    }
    // 同步更新静态缓存：屏幕不在前台时也能记住请求，下次 Create()
    // 走 BuildEmotionPath(s_current_emotion) 时就会用上。
    std::strncpy(s_current_emotion, category, sizeof(s_current_emotion) - 1);
    s_current_emotion[sizeof(s_current_emotion) - 1] = '\0';

    ESP_LOGI(TAG, "SetEmotion -> %s", s_current_emotion);

    // 在前台才真的替换表情源；调用方必须已经持有 LVGL 主锁
    // （和 ShowUserMessage / ShowSystemMessage 一致的约定）。
    ScheduleEmotionLoad(s_current_emotion);
}
