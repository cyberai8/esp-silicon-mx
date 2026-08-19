#include "digital_people_screen.h"
#include "avatar_compositor.h"
#include "config.h"
#include "i18n.h"

#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "application.h"
#include "audio_service.h"
#include "device_state.h"
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
constexpr uint32_t kColorBg      = 0x000000;
constexpr const char* kDefaultEmotion = "neutral";

char s_current_emotion[32] = "neutral";

struct UiState {
    lv_obj_t* screen        = nullptr;
    lv_obj_t* speech_bubble = nullptr;
    lv_obj_t* speech_label  = nullptr;
};

UiState s_ui;

constexpr size_t kSpeechTextMax = 384;
char s_user_speech[kSpeechTextMax] = "";
char s_system_speech[kSpeechTextMax] = "";
bool s_speech_show_user = true;

#if defined(BOARD_ESP_VOCAT) || (DISPLAY_WIDTH == 360 && DISPLAY_HEIGHT == 360)
constexpr int32_t kSideMargin         = 40;
constexpr int32_t kSpeechBubbleBottom = 36;
#else
constexpr int32_t kSideMargin         = 16;
constexpr int32_t kSpeechBubbleBottom = 24;
#endif
constexpr int32_t kSpeechBubbleMaxW = kPanelSize - kSideMargin * 2;
constexpr uint32_t kColorSpeechText = 0xFFFFFF;

lv_timer_t* s_activation_guard_timer = nullptr;

struct ActivationBlockedDialogUi {
    lv_obj_t* mask = nullptr;
};
ActivationBlockedDialogUi s_activation_dlg;
bool s_activation_blocked = false;

const lv_font_t* bubble_font() {
    return kRoundLayout ? &font_puhui_20_4 : &font_puhui_30_4;
}

void StyleSpeechBubble(lv_obj_t* bubble) {
    screen_strip_obj_chrome(bubble);
    lv_obj_remove_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(bubble, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bubble, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(bubble, 0, LV_PART_MAIN);
}

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
        lv_obj_set_width(s_ui.speech_bubble, bubble_w);
        lv_obj_set_width(s_ui.speech_label, bubble_w);
        lv_obj_set_height(s_ui.speech_label, line_h);
        lv_label_set_long_mode(s_ui.speech_label, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(s_ui.speech_label, LV_TEXT_ALIGN_CENTER,
                                    LV_PART_MAIN);
        lv_obj_align(s_ui.speech_label, LV_ALIGN_CENTER, 0, 0);
    } else {
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

const char* ActiveSpeechText() {
    const char* primary =
        s_speech_show_user ? s_user_speech : s_system_speech;
    if (primary[0] != '\0') {
        return primary;
    }
    return s_speech_show_user ? s_system_speech : s_user_speech;
}

void RefreshSpeechBar() {
    if (s_ui.speech_bubble == nullptr) {
        return;
    }

    if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
        ClearSpeechStorage();
        HideSpeechBubble();
        return;
    }

    const char* text = ActiveSpeechText();
    if (text == nullptr || text[0] == '\0') {
        HideSpeechBubble();
        return;
    }

    ApplySpeechToLabel(text);
    lv_obj_remove_flag(s_ui.speech_bubble, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(s_ui.speech_bubble, LV_ALIGN_BOTTOM_MID, 0,
                 -kSpeechBubbleBottom);
}

void on_refresh_device_state_async(void* /*param*/) {
    const bool speaking =
        Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking;
    AvatarCompositor::SetSpeaking(speaking);
    RefreshSpeechBar();
}

void OnSwipeBack();

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
    ClearSpeechStorage();
    AvatarCompositor::Destroy();
    s_activation_dlg = ActivationBlockedDialogUi{};
    s_activation_blocked = false;
    s_ui.screen        = nullptr;
    s_ui.speech_bubble = nullptr;
    s_ui.speech_label  = nullptr;
}

}  // namespace

lv_obj_t* DigitalPeopleScreen::Create() {
    s_activation_blocked = !is_device_activated();
    if (s_activation_blocked) {
        log_activation_blocked();
    }

    ESP_LOGI(TAG, "create digital people screen (firmware show11 avatar)");

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_ui.screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelSize, kPanelSize);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    if (!s_activation_blocked) {
        AvatarCompositor::Create(scr);
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
        AvatarCompositor::SetEmotion(s_current_emotion);
        on_refresh_device_state_async(nullptr);
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
        ResetLipSync();
    }
}

void DigitalPeopleScreen::SetEmotion(const char* emotion) {
    if (emotion == nullptr || emotion[0] == '\0') {
        emotion = kDefaultEmotion;
    }
    std::strncpy(s_current_emotion, emotion, sizeof(s_current_emotion) - 1);
    s_current_emotion[sizeof(s_current_emotion) - 1] = '\0';
    ESP_LOGI(TAG, "SetEmotion -> %s", s_current_emotion);
    AvatarCompositor::SetEmotion(s_current_emotion);
}

void DigitalPeopleScreen::ArmUtterance(int index) {
    AvatarCompositor::ArmUtterance(index);
}

void DigitalPeopleScreen::LoadVisemeTimeline(
    int index, const DigitalPeopleVisemeEvent* events, size_t count) {
    AvatarCompositor::LoadVisemeTimeline(
        index, reinterpret_cast<const AvatarCompositor::VisemeEvent*>(events),
        count);
}

void DigitalPeopleScreen::ResetLipSync() {
    AvatarCompositor::ResetLipSync();
}
