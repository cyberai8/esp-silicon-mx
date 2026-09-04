#include "battery_alert.h"

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "config.h"
#include "home_screen/home_screen.h"
#include "i18n.h"
#include "screen_util.h"

#include <cstdio>

#include <font_awesome.h>
#include <esp_log.h>
#include <lvgl.h>

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_awesome_20_4);

namespace {

constexpr const char* TAG = "BatteryAlert";

constexpr int kLowPopupThresholdPct = 25;
constexpr int kLowPopupRecoverPct = 26;
constexpr int kAutoShutdownThresholdPct = 6;
constexpr uint32_t kPollPeriodMs = 2000;

#if defined(BOARD_ESP_VOCAT) || (DISPLAY_WIDTH == 360 && DISPLAY_HEIGHT == 360)
constexpr bool kRoundLayout = true;
constexpr int kPanelW = DISPLAY_WIDTH;
constexpr int kPanelH = DISPLAY_HEIGHT;
constexpr int kCardW = 280;
constexpr int kCardH = 220;
constexpr int kBtnW = 120;
constexpr int kBtnH = 44;
constexpr int kCardPad = 16;
#else
constexpr bool kRoundLayout = false;
constexpr int kPanelW = 720;
constexpr int kPanelH = 720;
constexpr int kCardW = 520;
constexpr int kCardH = 300;
constexpr int kBtnW = 200;
constexpr int kBtnH = 72;
constexpr int kCardPad = 28;
#endif

struct State {
    lv_timer_t* timer = nullptr;
    lv_obj_t* overlay = nullptr;
    bool low_popup_dismissed = false;
    bool shutdown_triggered = false;
};

State s;

void CloseDialog() {
    if (s.overlay != nullptr) {
        lv_obj_delete(s.overlay);
        s.overlay = nullptr;
    }
}

void OnOkClicked(lv_event_t* /*e*/) {
    s.low_popup_dismissed = true;
    CloseDialog();
}

void ShowLowBatteryDialog(int battery_level) {
    lv_obj_t* parent = lv_layer_top();
    if (parent == nullptr) {
        parent = lv_screen_active();
    }
    if (parent == nullptr || s.overlay != nullptr) {
        return;
    }

    lv_obj_t* mask = lv_obj_create(parent);
    lv_obj_remove_style_all(mask);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(mask, kPanelW, kPanelH);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_70, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(mask, true);
    s.overlay = mask;

    lv_obj_t* card = lv_obj_create(mask);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, kCardW, kCardH);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1B2030), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, kCardPad, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(card, true);

    lv_obj_t* icon = lv_label_create(card);
    lv_label_set_text(icon, FONT_AWESOME_BATTERY_EMPTY);
    lv_obj_set_style_text_font(icon, &font_awesome_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(icon, lv_color_hex(0xF87171), LV_PART_MAIN);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, kRoundLayout ? 8 : 12);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* title = lv_label_create(card);
    lv_label_set_text(title, I18n::T("电量不足"));
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, kRoundLayout ? 44 : 56);
    lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE);

    char body_buf[64];
    std::snprintf(body_buf, sizeof(body_buf),
                  I18n::T("当前电量 %d%%，请及时充电"), battery_level);
    lv_obj_t* body = lv_label_create(card);
    lv_label_set_text(body, body_buf);
    lv_obj_set_width(body, kCardW - kCardPad * 2);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(body, lv_color_hex(0x9AA3B2), LV_PART_MAIN);
    lv_obj_set_style_text_font(body, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(body, LV_ALIGN_CENTER, 0, -8);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* ok = lv_button_create(card);
    lv_obj_remove_style_all(ok);
    lv_obj_set_size(ok, kBtnW, kBtnH);
    lv_obj_set_style_bg_color(ok, lv_color_hex(0x2A2F3A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ok, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(ok, 16, LV_PART_MAIN);
    lv_obj_align(ok, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(ok, OnOkClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(ok, true);

    lv_obj_t* ok_lbl = lv_label_create(ok);
    lv_label_set_text(ok_lbl, I18n::T("知道了"));
    lv_obj_set_style_text_color(ok_lbl, lv_color_hex(0xE5E7EB), LV_PART_MAIN);
    lv_obj_set_style_text_font(ok_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(ok_lbl);
    lv_obj_remove_flag(ok_lbl, LV_OBJ_FLAG_CLICKABLE);

    Application::GetInstance().PlaySound(Lang::Sounds::OGG_LOW_BATTERY);
    ESP_LOGW(TAG, "low battery popup: %d%%", battery_level);
}

void OnBatteryTick(lv_timer_t* /*timer*/) {
    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    if (!Board::GetInstance().GetBatteryLevel(battery_level, charging,
                                              discharging)) {
        return;
    }

    if (battery_level < 0) {
        battery_level = 0;
    } else if (battery_level > 100) {
        battery_level = 100;
    }

    if (charging) {
        s.low_popup_dismissed = false;
        s.shutdown_triggered = false;
        CloseDialog();
        return;
    }

    if (battery_level <= kAutoShutdownThresholdPct) {
        if (!s.shutdown_triggered) {
            s.shutdown_triggered = true;
            CloseDialog();
            ESP_LOGW(TAG, "auto shutdown at %d%%", battery_level);
            HomeScreen::RequestSystemShutdown(I18n::T("电量过低自动关机"));
        }
        return;
    }

    if (battery_level >= kLowPopupRecoverPct) {
        s.low_popup_dismissed = false;
        CloseDialog();
        return;
    }

    if (battery_level < kLowPopupThresholdPct && !s.low_popup_dismissed &&
        s.overlay == nullptr) {
        ShowLowBatteryDialog(battery_level);
    }
}

void EnsureTimer() {
    if (s.timer != nullptr) {
        return;
    }
    s.timer = lv_timer_create(OnBatteryTick, kPollPeriodMs, nullptr);
}

void StartOnLvglThread(void* /*user_data*/) {
    EnsureTimer();
    ESP_LOGI(TAG, "battery alert started (popup<%d%%, shutdown<=%d%%)",
             kLowPopupThresholdPct, kAutoShutdownThresholdPct);
}

}  // namespace

void BatteryAlert_Start() {
    screen_async_call(StartOnLvglThread, nullptr);
}
