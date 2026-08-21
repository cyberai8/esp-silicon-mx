#include "settings_screen.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#include <esp_log.h>

#include "audio_codec.h"
#include "backlight.h"
#include "bluetooth_screen/bluetooth_screen.h"
#include "board.h"
#include "config.h"
#include "cx25601n.h"
#include "home_screen/home_screen.h"
#include "i18n.h"
#include "native_bluetooth_audio.h"
#include "screen_util.h"
#include "settings.h"
#include "standby_screen/standby_screen.h"

#ifndef ESP_YUN_SIM
#include "application.h"
#include "ota.h"
#include "wifi_required_dialog.h"
#include <esp_app_desc.h>
#endif

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_puhui_number_50_4);

namespace {

constexpr const char* TAG = "SettingsScreen";

#if defined(BOARD_ESP_VOCAT) || (DISPLAY_WIDTH == 360 && DISPLAY_HEIGHT == 360)
constexpr bool kRoundLayout = true;
constexpr int kPanelW = DISPLAY_WIDTH;
constexpr int kPanelH = DISPLAY_HEIGHT;
constexpr int kHeaderTopInset = 28;
constexpr int kHeaderContentH = 36;
constexpr int kHeaderH = kHeaderTopInset + kHeaderContentH;
constexpr int kBackBtnSize = 36;
constexpr int kHeaderSideInset = 32;
constexpr int kTabBarSize = 40;  // 圆屏用顶部 Tab
constexpr int kTabItemH = 36;
constexpr int kTabItemGap = 4;
constexpr int kBodyH = kPanelH - kHeaderH;
constexpr int kSliderCardH = 96;
constexpr int kTabPadHor = 40;   // 内容左右安全区（≈280 宽）
constexpr int kTabPadTop = 6;
constexpr int kTabPadBottom = 36;
constexpr int kTabBtnMinW = 60;
#else
constexpr bool kRoundLayout = false;
constexpr int kPanelW = DISPLAY_WIDTH;
constexpr int kPanelH = DISPLAY_HEIGHT;
constexpr int kHeaderH = (kPanelH >= 700) ? 90 : 64;
constexpr int kBackBtnSize = (kPanelH >= 700) ? 72 : 48;
constexpr int kHeaderSideInset = 16;
constexpr int kTabBarSize = (kPanelW >= 700) ? 120 : 100;
constexpr int kTabItemH = (kPanelH >= 700) ? 64 : 48;
constexpr int kTabItemGap = 10;
constexpr int kBodyH = kPanelH - kHeaderH;
constexpr int kSliderCardH = 180;
constexpr int kTabPadHor = 24;
constexpr int kTabPadTop = 24;
constexpr int kTabPadBottom = 24;
#endif

constexpr uint32_t kColorBg = 0x0E1116;
constexpr uint32_t kColorText = 0xFFFFFF;
constexpr uint32_t kColorSubtle = 0x9AA3B2;
constexpr uint32_t kColorCard = 0x1B2030;
constexpr uint32_t kColorTabBar = 0x12151C;
constexpr uint32_t kColorAccent = 0x3B82F6;
constexpr uint32_t kColorValue = 0x60A5FA;
constexpr uint32_t kColorSliderTrack = 0x2A2F3A;

struct UiState {
    lv_obj_t* screen = nullptr;
    lv_obj_t* tabview = nullptr;
    lv_obj_t* brightness_pct_label = nullptr;
    lv_obj_t* brightness_slider = nullptr;
    lv_obj_t* volume_pct_label = nullptr;
    lv_obj_t* volume_slider = nullptr;
    lv_obj_t* enter_standby_min_label = nullptr;
    lv_obj_t* enter_standby_slider = nullptr;
    lv_obj_t* standby_face_weather = nullptr;
    lv_obj_t* standby_face_clock = nullptr;
    lv_obj_t* standby_face_gallery = nullptr;
    lv_obj_t* charge_tab = nullptr;
    lv_obj_t* ota_tab = nullptr;
    lv_obj_t* ota_current_label = nullptr;
    lv_obj_t* ota_remote_label = nullptr;
    lv_obj_t* ota_status_label = nullptr;
    lv_obj_t* ota_auto_switch = nullptr;
    lv_obj_t* ota_check_btn = nullptr;
    lv_obj_t* ota_upgrade_btn = nullptr;
    bool ota_checking = false;
    int brightness_drag_start_x = 0;
    int brightness_drag_start_val = 0;
};
UiState s_ui;

constexpr int kChargeNormalMa = 500;
constexpr int kChargeFastMa = 1000;
constexpr int kChargeDefaultMa = kChargeFastMa;
constexpr const char* kChargeNs = "charge";
constexpr const char* kChargeIchgKey = "ichg_ma";
constexpr const char* kAutoOtaKey = "auto_ota";
constexpr const char* kOtaRemoteVerKey = "ota_remote_ver";
constexpr const char* kOtaHasUpdateKey = "ota_has_update";
constexpr const char* kOtaFirmwareUrlKey = "ota_firmware_url";

const char* ReadCurrentFirmwareVersion() {
#ifndef ESP_YUN_SIM
    const esp_app_desc_t* app_desc = esp_app_get_description();
    if (app_desc != nullptr && app_desc->version[0] != '\0') {
        return app_desc->version;
    }
#endif
    return "1.0.0";
}

bool ReadAutoOtaEnabled() {
    Settings settings("display", false);
    return settings.GetInt(kAutoOtaKey, 1) != 0;
}

void SaveAutoOtaEnabled(bool enabled) {
    Settings settings("display", true);
    settings.SetInt(kAutoOtaKey, enabled ? 1 : 0);
}

std::string ReadCachedRemoteVersion() {
    Settings settings("display", false);
    return settings.GetString(kOtaRemoteVerKey, "");
}

bool ReadCachedHasUpdate() {
    Settings settings("display", false);
    return settings.GetInt(kOtaHasUpdateKey, 0) != 0;
}

std::string ReadCachedFirmwareUrl() {
    Settings settings("display", false);
    return settings.GetString(kOtaFirmwareUrlKey, "");
}

void SaveOtaCheckCache(const std::string& remote_ver, bool has_update,
                       const std::string& firmware_url) {
    Settings settings("display", true);
    settings.SetString(kOtaRemoteVerKey, remote_ver);
    settings.SetInt(kOtaHasUpdateKey, has_update ? 1 : 0);
    if (has_update) {
        settings.SetString(kOtaFirmwareUrlKey, firmware_url);
    }
}

void SetBrightnessFromUi(int value);

int NormalizeChargeMa(int ma) {
    if (ma == kChargeNormalMa || ma == kChargeFastMa) {
        return ma;
    }
    return kChargeDefaultMa;
}

int ReadSavedChargeMa() {
    Settings settings(kChargeNs);
    return NormalizeChargeMa(settings.GetInt(kChargeIchgKey, kChargeDefaultMa));
}

void SaveChargeMa(int ma) {
    ma = NormalizeChargeMa(ma);
    Settings settings(kChargeNs, true);
    settings.SetInt(kChargeIchgKey, ma);
}

bool ApplyChargeMa(int ma) {
    ma = NormalizeChargeMa(ma);
    if (!cx25601n_is_ready()) {
        ESP_LOGW(TAG, "CX25601N not ready, skip apply ichg=%d", ma);
        return false;
    }
    esp_err_t err = cx25601n_set_ichg_ma(static_cast<uint32_t>(ma));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set ichg=%d failed: %s", ma, esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "charge current -> %d mA", ma);
    return true;
}

void OnSwipeBack();
void OnBackClicked(lv_event_t* e);

int ReadInitialBrightness() {
    int value = kBacklightDefaultPercent;
    if (Backlight* backlight = Board::GetInstance().GetBacklight()) {
        value = backlight->brightness();
    } else {
        Settings settings("display");
        value = settings.GetInt("brightness", kBacklightDefaultPercent);
    }
    if (value < static_cast<int>(kBacklightMinPercent)) {
        value = kBacklightMinPercent;
    }
    if (value > 100) {
        value = 100;
    }
    return value;
}

int ReadInitialVolume() {
    int volume = 70;
    if (AudioCodec* codec = Board::GetInstance().GetAudioCodec()) {
        volume = codec->output_volume();
    }
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }
    return volume;
}

void UpdatePctLabel(lv_obj_t* label, int pct) {
    if (label == nullptr) {
        return;
    }
    if (pct < 0) {
        pct = 0;
    } else if (pct > 100) {
        pct = 100;
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d%%", pct);
    lv_label_set_text(label, buf);
}

void ApplyBrightness(int value) {
    if (value < static_cast<int>(kBacklightMinPercent)) {
        value = kBacklightMinPercent;
    }
    Backlight* backlight = Board::GetInstance().GetBacklight();
    if (backlight != nullptr) {
        backlight->SetBrightness(static_cast<uint8_t>(value), true);
    }
}

void SetBrightnessFromUi(int value) {
    if (value < static_cast<int>(kBacklightMinPercent)) {
        value = kBacklightMinPercent;
    } else if (value > 100) {
        value = 100;
    }
    UpdatePctLabel(s_ui.brightness_pct_label, value);
    if (s_ui.brightness_slider != nullptr) {
        lv_slider_set_value(s_ui.brightness_slider, value, LV_ANIM_OFF);
    }
    ApplyBrightness(value);
}

void OnBrightnessCardDrag(lv_event_t* e) {
    const lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t* indev = lv_indev_active();
    if (indev == nullptr) {
        return;
    }

    lv_point_t point{};
    lv_indev_get_point(indev, &point);

    if (code == LV_EVENT_PRESSED) {
        s_ui.brightness_drag_start_x = point.x;
        s_ui.brightness_drag_start_val =
            s_ui.brightness_slider != nullptr
                ? static_cast<int>(lv_slider_get_value(s_ui.brightness_slider))
                : ReadInitialBrightness();
        return;
    }

    if (code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED) {
        return;
    }

    const int card_w = kRoundLayout ? (kPanelW - kTabPadHor * 2) : (kPanelW - kTabPadHor * 2);
    const int span = card_w > 0 ? card_w : 280;
    const int delta = ((point.x - s_ui.brightness_drag_start_x) * 100) / span;
    SetBrightnessFromUi(s_ui.brightness_drag_start_val + delta);
}

void ApplyVolume(int volume) {
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }

    UpdatePctLabel(s_ui.volume_pct_label, volume);

    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr || codec->output_volume() == volume) {
        return;
    }
    codec->SetOutputVolume(volume);
}

void OnBrightnessSliderChanged(lv_event_t* e) {
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(e));
    int value = static_cast<int>(lv_slider_get_value(slider));
    if (value < static_cast<int>(kBacklightMinPercent)) {
        value = kBacklightMinPercent;
        lv_slider_set_value(slider, value, LV_ANIM_OFF);
    }
    UpdatePctLabel(s_ui.brightness_pct_label, value);
    ApplyBrightness(value);
}

void OnVolumeSliderChanged(lv_event_t* e) {
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(e));
    int value = static_cast<int>(lv_slider_get_value(slider));
    ApplyVolume(value);
    if (value != static_cast<int>(lv_slider_get_value(slider))) {
        lv_slider_set_value(slider, value, LV_ANIM_OFF);
    }
}

void StyleSlider(lv_obj_t* slider) {
    lv_obj_set_width(slider, LV_PCT(100));
    lv_obj_set_height(slider, 28);
    lv_obj_set_style_bg_color(slider, lv_color_hex(kColorSliderTrack), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(kColorAccent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_add_flag(slider, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_style_pad_hor(slider, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(slider, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(slider, 4, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, 10, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, 10, LV_PART_INDICATOR);
    screen_swipe_back_ignore(slider, true);
}

lv_obj_t* CreateSliderRow(lv_obj_t* parent, int min_value, int max_value,
                          int initial_value, lv_event_cb_t cb,
                          lv_obj_t** out_slider) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 52);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* slider = lv_slider_create(row);
    if (out_slider != nullptr) {
        *out_slider = slider;
    }
    StyleSlider(slider);
    lv_slider_set_range(slider, min_value, max_value);
    lv_slider_set_value(slider, initial_value, LV_ANIM_OFF);
    lv_obj_align(slider, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(slider, cb, LV_EVENT_VALUE_CHANGED, nullptr);
    return row;
}

void BuildSliderPanel(lv_obj_t* parent, const char* title, const char* hint,
                      const char* range_hint, int initial_value,
                      lv_obj_t** pct_label_out, lv_obj_t** slider_out,
                      int slider_min, int slider_max, lv_event_cb_t slider_cb,
                      int card_height = kSliderCardH) {
    lv_obj_set_style_pad_hor(parent, kTabPadHor, LV_PART_MAIN);
    lv_obj_set_style_pad_top(parent, kTabPadTop, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(parent, kTabPadBottom, LV_PART_MAIN);
    lv_obj_set_style_pad_row(parent, kRoundLayout ? 8 : 20, LV_PART_MAIN);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* card = lv_obj_create(parent);
    screen_strip_obj_chrome(card);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, card_height);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 24, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    screen_make_input_passive(card);

    lv_obj_t* pct = lv_label_create(card);
    if (pct_label_out != nullptr) {
        *pct_label_out = pct;
    }
    lv_obj_set_width(pct, LV_PCT(100));
    lv_label_set_long_mode(pct, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(pct, lv_color_hex(kColorValue), LV_PART_MAIN);
    lv_obj_set_style_text_font(
        pct, kRoundLayout ? &font_puhui_30_4 : &font_puhui_number_50_4,
        LV_PART_MAIN);
    lv_obj_set_style_text_align(pct, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    const int value_y = card_height <= 150 ? -10 : -16;
    const int hint_y = card_height <= 150 ? -12 : -20;
    lv_obj_align(pct, LV_ALIGN_CENTER, 0, value_y);
    UpdatePctLabel(pct, initial_value);

    lv_obj_t* card_hint = lv_label_create(card);
    lv_label_set_text(card_hint, hint);
    lv_obj_set_style_text_color(card_hint, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(card_hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(card_hint, LV_ALIGN_BOTTOM_MID, 0, hint_y);

    lv_obj_t* slider_hdr = lv_obj_create(parent);
    lv_obj_remove_style_all(slider_hdr);
    lv_obj_set_width(slider_hdr, LV_PCT(100));
    lv_obj_set_height(slider_hdr, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(slider_hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(slider_hdr, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(slider_hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* slider_title = lv_label_create(slider_hdr);
    lv_label_set_text(slider_title, title);
    lv_obj_set_style_text_color(slider_title, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(slider_title, &font_puhui_20_4, LV_PART_MAIN);

    lv_obj_t* range_lbl = lv_label_create(slider_hdr);
    lv_label_set_text(range_lbl, range_hint);
    lv_obj_set_style_text_color(range_lbl, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(range_lbl, &font_puhui_20_4, LV_PART_MAIN);

    CreateSliderRow(parent, slider_min, slider_max, initial_value, slider_cb,
                    slider_out);
}

void BuildBrightnessTab(lv_obj_t* tab, int initial_brightness) {
    char range_buf[24];
    std::snprintf(range_buf, sizeof(range_buf), "%d%% ~ 100%%",
                  static_cast<int>(kBacklightMinPercent));
    BuildSliderPanel(tab, I18n::T("左右滑动调节"), I18n::T("当前亮度"), range_buf,
                     initial_brightness, &s_ui.brightness_pct_label,
                     &s_ui.brightness_slider,
                     static_cast<int>(kBacklightMinPercent), 100,
                     OnBrightnessSliderChanged, kSliderCardH);

    lv_obj_t* card = lv_obj_get_child(tab, 0);
    if (card != nullptr) {
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, OnBrightnessCardDrag, LV_EVENT_PRESSED, nullptr);
        lv_obj_add_event_cb(card, OnBrightnessCardDrag, LV_EVENT_PRESSING, nullptr);
        lv_obj_add_event_cb(card, OnBrightnessCardDrag, LV_EVENT_RELEASED, nullptr);
        screen_swipe_back_ignore(card, true);
    }

    lv_obj_t* foot = lv_label_create(tab);
    lv_label_set_text(foot, I18n::T(kRoundLayout ? "左右滑动或拖动滑条，自动保存"
                                                 : "可在数值区左右滑动或拖动滑条，自动保存"));
    lv_obj_set_style_text_color(foot, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(foot, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(foot, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(foot, LV_PCT(100));
}

void UpdateMinutesLabel(lv_obj_t* label, int minutes, const char* never_text) {
    if (label == nullptr) {
        return;
    }
    char buf[24];
    if (minutes <= 0) {
        std::snprintf(buf, sizeof(buf), "%s", never_text);
    } else {
        std::snprintf(buf, sizeof(buf), I18n::T("%d 分钟"), minutes);
    }
    lv_label_set_text(label, buf);
}

void OnEnterStandbySliderChanged(lv_event_t* e) {
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(e));
    int value = static_cast<int>(lv_slider_get_value(slider));
    UpdateMinutesLabel(s_ui.enter_standby_min_label, value, I18n::T("永不进入"));
    HomeScreen::SetIdleStandbyMinutes(value);
}

void StyleStandbyFaceBtn(lv_obj_t* btn, bool selected) {
    if (btn == nullptr) {
        return;
    }
    lv_obj_set_style_border_color(
        btn, lv_color_hex(selected ? kColorAccent : kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_color(
        btn, lv_color_hex(selected ? 0x1E3A5F : kColorCard), LV_PART_MAIN);
}

void RefreshStandbyFaceButtons() {
    const StandbyFace face = StandbyScreen::GetPreferredFace();
    StyleStandbyFaceBtn(s_ui.standby_face_weather, face == StandbyFace::Weather);
    StyleStandbyFaceBtn(s_ui.standby_face_clock, face == StandbyFace::Clock);
    StyleStandbyFaceBtn(s_ui.standby_face_gallery, face == StandbyFace::Gallery);
}

void OnStandbyFaceClicked(lv_event_t* e) {
    const auto face = static_cast<StandbyFace>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    StandbyScreen::SetPreferredFace(face);
    RefreshStandbyFaceButtons();
}

lv_obj_t* CreateStandbyFaceBtn(lv_obj_t* parent, const char* title, StandbyFace face) {
    lv_obj_t* btn = lv_obj_create(parent);
    screen_strip_obj_chrome(btn);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_height(btn, kRoundLayout ? 44 : 56);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 14, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 2, LV_PART_MAIN);
    lv_obj_set_style_border_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, OnStandbyFaceClicked, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(face)));
    screen_swipe_back_ignore(btn, true);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_color(lbl, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(lbl);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return btn;
}

void BuildStandbyTab(lv_obj_t* tab) {
    const int initial_standby = HomeScreen::GetIdleStandbyMinutes();
    const int standby_card_h = kRoundLayout ? 100 : 132;

    BuildSliderPanel(tab, I18n::T("进入待机"),
                     I18n::T(kRoundLayout ? "无操作后进入待机"
                                         : "首页无操作后进入待机页"),
                     I18n::T(kRoundLayout ? "0 ~ 60 分" : "0 ~ 60 分钟"),
                     initial_standby, &s_ui.enter_standby_min_label,
                     &s_ui.enter_standby_slider, 0, 60,
                     OnEnterStandbySliderChanged, standby_card_h);
    UpdateMinutesLabel(s_ui.enter_standby_min_label, initial_standby,
                       I18n::T("永不进入"));

    lv_obj_t* face_title = lv_label_create(tab);
    lv_label_set_text(face_title, I18n::T("待机界面"));
    lv_obj_set_style_text_color(face_title, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(face_title, &font_puhui_20_4, LV_PART_MAIN);

    lv_obj_t* face_row = lv_obj_create(tab);
    screen_strip_obj_chrome(face_row);
    lv_obj_set_width(face_row, LV_PCT(100));
    lv_obj_set_height(face_row, kRoundLayout ? 44 : 56);
    lv_obj_set_style_bg_opa(face_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(face_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(face_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(face_row, 10, LV_PART_MAIN);
    lv_obj_remove_flag(face_row, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.standby_face_weather =
        CreateStandbyFaceBtn(face_row, I18n::T("天气"), StandbyFace::Weather);
    s_ui.standby_face_clock =
        CreateStandbyFaceBtn(face_row, I18n::T("时钟"), StandbyFace::Clock);
    s_ui.standby_face_gallery =
        CreateStandbyFaceBtn(face_row, I18n::T("相册"), StandbyFace::Gallery);
    RefreshStandbyFaceButtons();

    lv_obj_t* face_hint = lv_label_create(tab);
    lv_label_set_text(face_hint, I18n::T("左右滑动也可切换"));
    lv_obj_set_style_text_color(face_hint, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(face_hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(face_hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(face_hint, LV_PCT(100));

    lv_obj_set_style_pad_row(tab, kRoundLayout ? 8 : 14, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(tab, kTabPadBottom, LV_PART_MAIN);

    lv_obj_t* foot = lv_label_create(tab);
    lv_label_set_text(foot, I18n::T(kRoundLayout ? "自动保存" : "待机设置会自动保存"));
    lv_obj_set_style_text_color(foot, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(foot, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(foot, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(foot, LV_PCT(100));
}

void BuildVolumeTab(lv_obj_t* tab, int initial_volume) {
    BuildSliderPanel(tab, I18n::T("拖动调节"), I18n::T("当前音量"), "0% ~ 100%",
                     initial_volume, &s_ui.volume_pct_label, &s_ui.volume_slider,
                     0, 100, OnVolumeSliderChanged, kSliderCardH);

    lv_obj_t* foot = lv_label_create(tab);
    lv_label_set_text(foot, I18n::T(kRoundLayout ? "自动保存" : "音量设置会自动保存"));
    lv_obj_set_style_text_color(foot, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(foot, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(foot, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(foot, LV_PCT(100));
}

void BuildHeader(lv_obj_t* parent) {
    lv_obj_t* header = lv_obj_create(parent);
    screen_strip_obj_chrome(header);
    lv_obj_set_size(header, kPanelW, kHeaderH);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    if constexpr (!kRoundLayout) {
        // 方屏保留左上返回键；圆屏与网络配置一致：无箭头，右滑退出。
        lv_obj_t* back = lv_button_create(header);
        lv_obj_remove_style_all(back);
        lv_obj_set_size(back, kBackBtnSize, kBackBtnSize);
        lv_obj_align(back, LV_ALIGN_LEFT_MID, kHeaderSideInset, 0);
        lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_bg_color(back, lv_color_hex(0xFFFFFF),
                                  LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(back, LV_OPA_20, LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN);
        lv_obj_add_event_cb(back, OnBackClicked, LV_EVENT_CLICKED, nullptr);
        screen_swipe_back_ignore(back, true);

        lv_obj_t* back_icon = lv_image_create(back);
        lv_image_set_src(back_icon, "A:ic_app_back.spng");
        lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(back_icon);
    }

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, I18n::T("设置"));
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title,
                               kRoundLayout ? &font_puhui_20_4 : &font_puhui_30_4,
                               LV_PART_MAIN);
    if constexpr (kRoundLayout) {
        const int title_y = kHeaderTopInset + (kHeaderContentH - 20) / 2;
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, title_y);
    } else {
        lv_obj_align(title, LV_ALIGN_LEFT_MID,
                     kHeaderSideInset + kBackBtnSize + 16, 0);
    }
}

void GoHomeAfterLocaleChange() {
    HomeScreen::ResetToFirstPage();
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* home = HomeScreen::Create();
    lv_screen_load(home);
    if (old_scr != nullptr && old_scr != home) {
        lv_obj_delete_async(old_scr);
    }
}

void OnLanguageCardClicked(lv_event_t* e) {
    auto locale = static_cast<I18n::Locale>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    if (!I18n::SetLocale(locale)) {
        return;
    }
    ESP_LOGI(TAG, "language -> %s (rebuild home)", I18n::GetLocaleCode());
    GoHomeAfterLocaleChange();
}

void BuildLanguageTab(lv_obj_t* tab) {
    lv_obj_set_style_pad_hor(tab, kTabPadHor, LV_PART_MAIN);
    lv_obj_set_style_pad_top(tab, kTabPadTop, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(tab, kTabPadBottom, LV_PART_MAIN);
    lv_obj_set_style_pad_row(tab, kRoundLayout ? 10 : 16, LV_PART_MAIN);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_add_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* hint = lv_label_create(tab);
    lv_label_set_text(hint, I18n::T("选择界面显示语言"));
    lv_obj_set_style_text_color(hint, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);

    const I18n::Locale current = I18n::GetLocale();
    for (size_t i = 0; i < I18n::GetLocaleCount(); ++i) {
        const I18n::LocaleInfo* info =
            I18n::GetLocaleInfo(static_cast<I18n::Locale>(i));
        if (info == nullptr) {
            continue;
        }

        lv_obj_t* card = lv_obj_create(tab);
        screen_strip_obj_chrome(card);
        lv_obj_set_width(card, LV_PCT(100));
        lv_obj_set_height(card, kRoundLayout ? 64 : 88);
        lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(card, 20, LV_PART_MAIN);
        lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
        const bool selected = (info->id == current);
        lv_obj_set_style_border_color(
            card, lv_color_hex(selected ? kColorAccent : kColorCard),
            LV_PART_MAIN);
        lv_obj_set_style_border_opa(card, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, OnLanguageCardClicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<uintptr_t>(
                                static_cast<unsigned>(info->id))));
        screen_swipe_back_ignore(card, true);

        lv_obj_t* name = lv_label_create(card);
        // Native name stays in its own script (简体中文 / English).
        lv_label_set_text(name, info->native_name);
        lv_obj_set_style_text_color(name, lv_color_hex(kColorText),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_font(
            name, kRoundLayout ? &font_puhui_20_4 : &font_puhui_30_4,
            LV_PART_MAIN);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, kRoundLayout ? 12 : 20,
                     kRoundLayout ? -8 : -10);

        lv_obj_t* code = lv_label_create(card);
        lv_label_set_text(code, info->english_name);
        lv_obj_set_style_text_color(code, lv_color_hex(kColorSubtle),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_font(code, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_align(code, LV_ALIGN_LEFT_MID, 20, 18);

        if (selected) {
            lv_obj_t* mark = lv_label_create(card);
            lv_label_set_text(mark, I18n::T("当前语言"));
            lv_obj_set_style_text_color(mark, lv_color_hex(kColorValue),
                                        LV_PART_MAIN);
            lv_obj_set_style_text_font(mark, &font_puhui_20_4, LV_PART_MAIN);
            lv_obj_align(mark, LV_ALIGN_RIGHT_MID, -20, 0);
        }
    }

    lv_obj_t* foot = lv_label_create(tab);
    lv_label_set_text(foot,
                      I18n::T(kRoundLayout ? "立即生效并返回主页"
                                          : "切换后立即生效并返回主页"));
    lv_obj_set_style_text_color(foot, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(foot, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(foot, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(foot, LV_PCT(100));
}

void StyleSettingsSwitch(lv_obj_t* sw) {
    lv_obj_set_size(sw, kRoundLayout ? 40 : 52, kRoundLayout ? 22 : 28);
    lv_obj_set_style_bg_color(sw, lv_color_hex(kColorAccent), LV_PART_INDICATOR);
    screen_swipe_back_ignore(sw, true);
}

lv_obj_t* CreateInfoRow(lv_obj_t* parent, const char* title, const char* value,
                        lv_obj_t** value_out = nullptr) {
    lv_obj_t* card = lv_obj_create(parent);
    screen_strip_obj_chrome(card);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, kRoundLayout ? 56 : 72);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 16, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title_lbl = lv_label_create(card);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(title_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(title_lbl, LV_ALIGN_LEFT_MID, kRoundLayout ? 12 : 16, -10);

    lv_obj_t* val_lbl = lv_label_create(card);
    lv_label_set_text(val_lbl, value);
    if (value_out != nullptr) {
        *value_out = val_lbl;
    }
    lv_obj_set_style_text_color(val_lbl, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(val_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(val_lbl, LV_ALIGN_LEFT_MID, kRoundLayout ? 12 : 16, 12);
    return card;
}

void RefreshOtaLabels() {
    if (s_ui.ota_current_label != nullptr) {
        lv_label_set_text(s_ui.ota_current_label, ReadCurrentFirmwareVersion());
    }
    if (s_ui.ota_remote_label != nullptr) {
        const std::string remote = ReadCachedRemoteVersion();
        lv_label_set_text(s_ui.ota_remote_label,
                          remote.empty() ? I18n::T("尚未检查") : remote.c_str());
    }
    if (s_ui.ota_status_label != nullptr) {
        const char* status = I18n::T("已是最新版本");
        if (ReadCachedHasUpdate()) {
            status = I18n::T("发现新版本");
        } else if (ReadCachedRemoteVersion().empty()) {
            status = I18n::T("点击检查更新");
        }
        lv_label_set_text(s_ui.ota_status_label, status);
    }
    if (s_ui.ota_upgrade_btn != nullptr) {
        if (ReadCachedHasUpdate()) {
            lv_obj_remove_state(s_ui.ota_upgrade_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_ui.ota_upgrade_btn, LV_STATE_DISABLED);
        }
    }
}

#ifndef ESP_YUN_SIM
struct OtaCheckResult {
    esp_err_t err = ESP_OK;
    std::string remote_version;
    bool has_update = false;
    std::string firmware_url;
};

void OnOtaCheckDone(void* user_data) {
    std::unique_ptr<OtaCheckResult> result(static_cast<OtaCheckResult*>(user_data));
    s_ui.ota_checking = false;
    if (s_ui.ota_check_btn != nullptr) {
        lv_obj_remove_state(s_ui.ota_check_btn, LV_STATE_DISABLED);
    }
    if (result == nullptr) {
        return;
    }

    if (result->err != ESP_OK) {
        ESP_LOGW(TAG, "ota check failed: 0x%x", result->err);
        if (s_ui.ota_status_label != nullptr) {
            lv_label_set_text(s_ui.ota_status_label, I18n::T("检查失败，请稍后重试"));
        }
        return;
    }

    ESP_LOGI(TAG, "ota check done: remote=%s has_update=%d",
             result->remote_version.c_str(), result->has_update ? 1 : 0);
    SaveOtaCheckCache(result->remote_version, result->has_update,
                      result->firmware_url);
    RefreshOtaLabels();
}

void StartOtaVersionCheck() {
    if (s_ui.ota_checking) {
        return;
    }
    if (WifiRequired_ShouldBlock()) {
        WifiRequired_ShowDialog("请先连接 WiFi 后再检查更新");
        return;
    }

    s_ui.ota_checking = true;
    if (s_ui.ota_check_btn != nullptr) {
        lv_obj_add_state(s_ui.ota_check_btn, LV_STATE_DISABLED);
    }
    if (s_ui.ota_status_label != nullptr) {
        lv_label_set_text(s_ui.ota_status_label, I18n::T("正在检查更新..."));
    }
    ESP_LOGI(TAG, "start ota version check");

    Application::GetInstance().Schedule([]() {
        Ota ota;
        auto result = std::make_unique<OtaCheckResult>();
        result->err = ota.CheckVersion(/*pause_lvgl=*/false);
        if (result->err == ESP_OK) {
            result->remote_version = ota.GetFirmwareVersion();
            result->has_update = ota.HasNewVersion();
            result->firmware_url = ota.GetFirmwareUrl();
        }
        lv_async_call(OnOtaCheckDone, result.release());
    });
}

void OtaUpgradeTask() {
    Ota ota;
    if (ota.CheckVersion(/*pause_lvgl=*/false) != ESP_OK || !ota.HasNewVersion()) {
        lv_async_call(
            [](void* /*user_data*/) {
                if (s_ui.ota_status_label != nullptr) {
                    lv_label_set_text(s_ui.ota_status_label,
                                      I18n::T("暂无可升级版本"));
                }
                if (s_ui.ota_upgrade_btn != nullptr) {
                    lv_obj_add_state(s_ui.ota_upgrade_btn, LV_STATE_DISABLED);
                }
            },
            nullptr);
        return;
    }
    Application::GetInstance().UpgradeFirmware(ota);
}

void OnOtaCheckClicked(lv_event_t* /*e*/) { StartOtaVersionCheck(); }

void OnOtaUpgradeClicked(lv_event_t* /*e*/) {
    if (!ReadCachedHasUpdate()) {
        return;
    }
    if (WifiRequired_ShouldBlock()) {
        WifiRequired_ShowDialog("请先连接 WiFi 后再升级");
        return;
    }
    if (s_ui.ota_status_label != nullptr) {
        lv_label_set_text(s_ui.ota_status_label, I18n::T("准备升级..."));
    }
    Application::GetInstance().Schedule([]() { OtaUpgradeTask(); });
}
#endif

void OnAutoOtaSwitchChanged(lv_event_t* e) {
    lv_obj_t* sw = static_cast<lv_obj_t*>(lv_event_get_target(e));
    SaveAutoOtaEnabled(lv_obj_has_state(sw, LV_STATE_CHECKED));
}

void BuildOtaTab(lv_obj_t* tab) {
    s_ui.ota_tab = tab;
    s_ui.ota_current_label = nullptr;
    s_ui.ota_remote_label = nullptr;
    s_ui.ota_status_label = nullptr;
    s_ui.ota_auto_switch = nullptr;
    s_ui.ota_check_btn = nullptr;
    s_ui.ota_upgrade_btn = nullptr;

    lv_obj_set_style_pad_hor(tab, kTabPadHor, LV_PART_MAIN);
    lv_obj_set_style_pad_top(tab, kTabPadTop, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(tab, kTabPadBottom, LV_PART_MAIN);
    lv_obj_set_style_pad_row(tab, kRoundLayout ? 8 : 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_add_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* auto_card = lv_obj_create(tab);
    screen_strip_obj_chrome(auto_card);
    lv_obj_set_width(auto_card, LV_PCT(100));
    lv_obj_set_height(auto_card, kRoundLayout ? 56 : 72);
    lv_obj_set_style_bg_color(auto_card, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(auto_card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(auto_card, 16, LV_PART_MAIN);
    lv_obj_remove_flag(auto_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* auto_title = lv_label_create(auto_card);
    lv_label_set_text(auto_title, I18n::T("自动 OTA 升级"));
    lv_obj_set_style_text_color(auto_title, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(auto_title, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(auto_title, LV_ALIGN_LEFT_MID, kRoundLayout ? 12 : 16, 0);

    lv_obj_t* sw = lv_switch_create(auto_card);
    s_ui.ota_auto_switch = sw;
    StyleSettingsSwitch(sw);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -12, 0);
    if (ReadAutoOtaEnabled()) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, OnAutoOtaSwitchChanged, LV_EVENT_VALUE_CHANGED, nullptr);

    CreateInfoRow(tab, I18n::T("当前版本"), ReadCurrentFirmwareVersion(),
                  &s_ui.ota_current_label);
    CreateInfoRow(tab, I18n::T("最新版本"),
                  ReadCachedRemoteVersion().empty()
                      ? I18n::T("尚未检查")
                      : ReadCachedRemoteVersion().c_str(),
                  &s_ui.ota_remote_label);

    lv_obj_t* status_card = lv_obj_create(tab);
    screen_strip_obj_chrome(status_card);
    lv_obj_set_width(status_card, LV_PCT(100));
    lv_obj_set_height(status_card, kRoundLayout ? 44 : 56);
    lv_obj_set_style_bg_opa(status_card, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(status_card, LV_OBJ_FLAG_SCROLLABLE);
    s_ui.ota_status_label = lv_label_create(status_card);
    lv_label_set_text(s_ui.ota_status_label, I18n::T("点击检查更新"));
    lv_obj_set_style_text_color(s_ui.ota_status_label, lv_color_hex(kColorValue),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.ota_status_label, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_ui.ota_status_label, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_set_width(s_ui.ota_status_label, LV_PCT(100));

    auto make_btn = [&](const char* text, lv_event_cb_t cb, bool primary) {
        lv_obj_t* btn = lv_button_create(tab);
        lv_obj_set_width(btn, LV_PCT(100));
        lv_obj_set_height(btn, kRoundLayout ? 44 : 56);
        lv_obj_set_style_radius(btn, 14, LV_PART_MAIN);
        lv_obj_set_style_bg_color(
            btn, lv_color_hex(primary ? kColorAccent : kColorCard), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
        screen_swipe_back_ignore(btn, true);

        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_color(lbl, lv_color_hex(kColorText), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_center(lbl);
        return btn;
    };

#ifndef ESP_YUN_SIM
    s_ui.ota_check_btn = make_btn(I18n::T("检查更新"), OnOtaCheckClicked, false);
    s_ui.ota_upgrade_btn = make_btn(I18n::T("立即升级"), OnOtaUpgradeClicked, true);
#else
    s_ui.ota_check_btn =
        make_btn(I18n::T("检查更新"), static_cast<lv_event_cb_t>(nullptr), false);
    s_ui.ota_upgrade_btn =
        make_btn(I18n::T("立即升级"), static_cast<lv_event_cb_t>(nullptr), true);
    lv_obj_add_state(s_ui.ota_check_btn, LV_STATE_DISABLED);
#endif
    RefreshOtaLabels();

    lv_obj_t* foot = lv_label_create(tab);
    lv_label_set_text(foot, I18n::T("关闭自动升级后，启动时仅检查版本不自动安装"));
    lv_obj_set_style_text_color(foot, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(foot, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(foot, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(foot, LV_PCT(100));
    lv_label_set_long_mode(foot, LV_LABEL_LONG_WRAP);
}

void FixTabBarItemHeights(lv_obj_t* tabview) {
    lv_obj_t* bar = lv_tabview_get_tab_bar(tabview);
    if constexpr (kRoundLayout) {
        // 顶部 Tab 可横向滑动，Tab 多时也不挤。
        lv_obj_add_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(bar, LV_DIR_HOR);
        lv_obj_set_scrollbar_mode(bar, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(bar, kTabItemGap, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(bar, 16, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(bar, 2, LV_PART_MAIN);
    } else {
        lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(bar, kTabItemGap, LV_PART_MAIN);
        lv_obj_set_style_pad_top(bar, 20, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(bar, 8, LV_PART_MAIN);
    }

    const uint32_t count = lv_tabview_get_tab_count(tabview);
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t* btn = lv_obj_get_child_by_type(bar, i, &lv_button_class);
        if (btn == nullptr) {
            continue;
        }
        if constexpr (kRoundLayout) {
            lv_obj_set_flex_grow(btn, 0);
            lv_obj_set_width(btn, kTabBtnMinW);
            lv_obj_set_height(btn, kTabItemH);
        } else {
            lv_obj_set_flex_grow(btn, 0);
            lv_obj_set_width(btn, lv_pct(100));
            lv_obj_set_height(btn, kTabItemH);
        }
        lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
    }
}

void BuildBluetoothTab(lv_obj_t* tab) {
    lv_obj_set_style_bg_opa(tab, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tab, 0, LV_PART_MAIN);
    lv_obj_add_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
#if BOARD_HAS_EXTERNAL_BT
    BluetoothScreen::BuildInto(tab);
#else
    lv_obj_set_style_pad_all(tab, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_row(tab, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* title = lv_label_create(tab);
    lv_label_set_text(title, "片上蓝牙");
    lv_obj_set_style_text_color(title, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_20_4, LV_PART_MAIN);

    lv_obj_t* desc = lv_label_create(tab);
    lv_label_set_text(desc, "本机使用片上蓝牙，音乐 App 默认使用音响模式");
    lv_obj_set_width(desc, LV_PCT(100));
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(desc, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(desc, &font_puhui_20_4, LV_PART_MAIN);

    auto make_native_btn = [](lv_obj_t* parent, const char* text, NativeBluetoothAudio::Mode mode) {
        lv_obj_t* btn = lv_button_create(parent);
        lv_obj_set_width(btn, LV_PCT(100));
        lv_obj_set_height(btn, 56);
        lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
        lv_obj_set_style_bg_color(btn, lv_color_hex(kColorCard), LV_PART_MAIN);
        lv_obj_add_event_cb(
            btn,
            [](lv_event_t* e) {
                auto mode = static_cast<NativeBluetoothAudio::Mode>(
                    reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
                NativeBluetoothAudio::GetInstance().SetMode(mode);
            },
            LV_EVENT_CLICKED,
            reinterpret_cast<void*>(static_cast<intptr_t>(static_cast<uint8_t>(mode))));
        screen_swipe_back_ignore(btn, true);

        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_color(lbl, lv_color_hex(kColorText), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_center(lbl);
    };

    make_native_btn(tab, "音响模式：手机连接本机播放音乐",
                    NativeBluetoothAudio::Mode::kSpeakerSink);
    make_native_btn(tab, "音源模式：待接入本机播放链路",
                    NativeBluetoothAudio::Mode::kAudioSource);

    lv_obj_t* note = lv_label_create(tab);
    lv_label_set_text(note, "当前固件已接通音响模式；音源模式还需要本机播放器/扫描连接链路");
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(note, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(note, &font_puhui_20_4, LV_PART_MAIN);
#endif
}

void BuildChargeTab(lv_obj_t* tab);

void RebuildChargeTabAsync(void* /*user_data*/) {
    if (s_ui.charge_tab != nullptr) {
        BuildChargeTab(s_ui.charge_tab);
    }
}

void OnChargeModeClicked(lv_event_t* e) {
    const int ma =
        NormalizeChargeMa(static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
    if (ma == ReadSavedChargeMa()) {
        ApplyChargeMa(ma);
        return;
    }
    SaveChargeMa(ma);
    ApplyChargeMa(ma);
    // 不能在 CLICKED 回调里同步删掉被点击的 card，延后重建选中态。
    lv_async_call(RebuildChargeTabAsync, nullptr);
}

void BuildChargeTab(lv_obj_t* tab) {
    s_ui.charge_tab = tab;
    lv_obj_clean(tab);

    lv_obj_set_style_pad_hor(tab, kTabPadHor, LV_PART_MAIN);
    lv_obj_set_style_pad_top(tab, kTabPadTop, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(tab, kTabPadBottom, LV_PART_MAIN);
    lv_obj_set_style_pad_row(tab, kRoundLayout ? 10 : 16, LV_PART_MAIN);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_add_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

    if (!cx25601n_is_ready()) {
        // 无芯片时不应进入本 Tab；BuildTabView 已按 ready 决定是否添加。
        return;
    }

    lv_obj_t* hint = lv_label_create(tab);
    lv_label_set_text(hint, I18n::T("选择充电电流"));
    lv_obj_set_style_text_color(hint, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);

    const int current_ma = ReadSavedChargeMa();
    struct ChargeMode {
        int ma;
        const char* title;
        const char* subtitle;
    };
    const ChargeMode modes[] = {
        {kChargeFastMa, "快速充电", "1000 mA"},
        {kChargeNormalMa, "正常充电", "500 mA"},
    };

    for (const ChargeMode& mode : modes) {
        lv_obj_t* card = lv_obj_create(tab);
        screen_strip_obj_chrome(card);
        lv_obj_set_width(card, LV_PCT(100));
        lv_obj_set_height(card, kRoundLayout ? 64 : 88);
        lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(card, 20, LV_PART_MAIN);
        lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
        const bool selected = (mode.ma == current_ma);
        lv_obj_set_style_border_color(
            card, lv_color_hex(selected ? kColorAccent : kColorCard),
            LV_PART_MAIN);
        lv_obj_set_style_border_opa(card, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, OnChargeModeClicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(mode.ma)));
        screen_swipe_back_ignore(card, true);

        lv_obj_t* name = lv_label_create(card);
        lv_label_set_text(name, I18n::T(mode.title));
        lv_obj_set_style_text_color(name, lv_color_hex(kColorText), LV_PART_MAIN);
        lv_obj_set_style_text_font(
            name, kRoundLayout ? &font_puhui_20_4 : &font_puhui_30_4,
            LV_PART_MAIN);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, kRoundLayout ? 12 : 20,
                     kRoundLayout ? -8 : -10);

        lv_obj_t* sub = lv_label_create(card);
        lv_label_set_text(sub, I18n::T(mode.subtitle));
        lv_obj_set_style_text_color(sub, lv_color_hex(kColorSubtle), LV_PART_MAIN);
        lv_obj_set_style_text_font(sub, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_align(sub, LV_ALIGN_LEFT_MID, kRoundLayout ? 12 : 20,
                     kRoundLayout ? 14 : 18);

        if (selected) {
            lv_obj_t* mark = lv_label_create(card);
            lv_label_set_text(mark, I18n::T("当前档位"));
            lv_obj_set_style_text_color(mark, lv_color_hex(kColorValue),
                                        LV_PART_MAIN);
            lv_obj_set_style_text_font(mark, &font_puhui_20_4, LV_PART_MAIN);
            lv_obj_align(mark, LV_ALIGN_RIGHT_MID, -20, 0);
        }
    }

    lv_obj_t* foot = lv_label_create(tab);
    lv_label_set_text(foot, I18n::T("充电设置会自动保存"));
    lv_obj_set_style_text_color(foot, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(foot, &font_puhui_20_4, LV_PART_MAIN);
}

void BuildTabView(lv_obj_t* parent) {
    const int initial_brightness = ReadInitialBrightness();
    const int initial_volume = ReadInitialVolume();

    lv_obj_t* tv = lv_tabview_create(parent);
    s_ui.tabview = tv;
    lv_obj_set_size(tv, kPanelW, kBodyH);
    lv_obj_set_pos(tv, 0, kHeaderH);
    if constexpr (kRoundLayout) {
        lv_tabview_set_tab_bar_position(tv, LV_DIR_TOP);
    } else {
        lv_tabview_set_tab_bar_position(tv, LV_DIR_LEFT);
    }
    lv_tabview_set_tab_bar_size(tv, kTabBarSize);

    lv_obj_set_style_bg_color(tv, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tv, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(tv, 0, LV_PART_MAIN);

    lv_obj_t* bar = lv_tabview_get_tab_bar(tv);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColorTabBar), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(bar, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(bar, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 4, LV_PART_ITEMS);
    lv_obj_set_style_pad_hor(bar, 6, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColorAccent),
                              LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER,
                            LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(bar, lv_color_hex(kColorText),
                                LV_PART_ITEMS | LV_STATE_CHECKED);

    lv_obj_t* content = lv_tabview_get_content(tv);
    // 方屏：内容区忽略右滑，避免横滑切 Tab 误退出。
    // 圆屏：无返回箭头，允许内容区左右滑动切换 Tab。
    if constexpr (!kRoundLayout) {
        screen_swipe_back_ignore(content, true);
    } else {
        lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(content, LV_DIR_HOR);
        lv_obj_set_scroll_snap_x(content, LV_SCROLL_SNAP_CENTER);
        lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);
    }

    lv_obj_t* tab_brightness = lv_tabview_add_tab(tv, I18n::T("亮度"));
    BuildBrightnessTab(tab_brightness, initial_brightness);

    lv_obj_t* tab_standby = lv_tabview_add_tab(tv, I18n::T("待机"));
    BuildStandbyTab(tab_standby);

    lv_obj_t* tab_volume = lv_tabview_add_tab(tv, I18n::T("音量"));
    BuildVolumeTab(tab_volume, initial_volume);

    lv_obj_t* tab_language = lv_tabview_add_tab(tv, I18n::T("语言"));
    BuildLanguageTab(tab_language);

    lv_obj_t* tab_ota = lv_tabview_add_tab(tv, I18n::T("升级"));
    BuildOtaTab(tab_ota);

    // 老设备无 CX25601N（0x6B）时不显示充电 Tab
    if (cx25601n_is_ready()) {
        lv_obj_t* tab_charge = lv_tabview_add_tab(tv, I18n::T("充电"));
        BuildChargeTab(tab_charge);
    }

#if !defined(BOARD_ESP_VOCAT)
    lv_obj_t* tab_bluetooth = lv_tabview_add_tab(tv, I18n::T("蓝牙"));
    BuildBluetoothTab(tab_bluetooth);
#endif

    FixTabBarItemHeights(tv);
}

void OnSwipeBack() {
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
#if BOARD_HAS_EXTERNAL_BT
    BluetoothScreen::ResetUi();
#endif
    s_ui.screen = nullptr;
    s_ui.tabview = nullptr;
    s_ui.brightness_pct_label = nullptr;
    s_ui.brightness_slider = nullptr;
    s_ui.volume_pct_label = nullptr;
    s_ui.volume_slider = nullptr;
    s_ui.enter_standby_min_label = nullptr;
    s_ui.enter_standby_slider = nullptr;
    s_ui.standby_face_weather = nullptr;
    s_ui.standby_face_clock = nullptr;
    s_ui.standby_face_gallery = nullptr;
    s_ui.charge_tab = nullptr;
    s_ui.ota_tab = nullptr;
    s_ui.ota_current_label = nullptr;
    s_ui.ota_remote_label = nullptr;
    s_ui.ota_status_label = nullptr;
    s_ui.ota_auto_switch = nullptr;
    s_ui.ota_check_btn = nullptr;
    s_ui.ota_upgrade_btn = nullptr;
    s_ui.ota_checking = false;
}

}  // namespace

lv_obj_t* SettingsScreen::Create() {
    lv_obj_t* scr = lv_obj_create(nullptr);
    s_ui.screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelW, kPanelH);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    BuildHeader(scr);
    BuildTabView(scr);

    screen_attach_swipe_back(scr, OnSwipeBack);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);

    return scr;
}

void SettingsScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: settings_screen");
    } else {
        ESP_LOGI(TAG, "unload: settings_screen");
    }
#if BOARD_HAS_EXTERNAL_BT
    BluetoothScreen::LifecycleCallback(event);
#endif
}
