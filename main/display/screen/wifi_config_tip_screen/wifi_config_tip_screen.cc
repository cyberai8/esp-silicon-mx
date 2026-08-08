#include "wifi_config_tip_screen.h"

#include <esp_log.h>

#include "assets/lang_config.h"
#include "config.h"
#include "esp_lv_adapter.h"
#include "screen_util.h"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char* TAG = "WifiConfigTip";

#if defined(BOARD_ESP_VOCAT) || (DISPLAY_WIDTH == 360 && DISPLAY_HEIGHT == 360)
constexpr bool kRoundLayout = true;
constexpr int kPanelW = DISPLAY_WIDTH;
constexpr int kPanelH = DISPLAY_HEIGHT;
constexpr int kContentW = DISPLAY_WIDTH - 72;
constexpr int kTitleY = 56;
constexpr int kGuideY = 108;
constexpr int kSsidY = 148;
constexpr int kBrowserY = 200;
constexpr int kUrlY = 240;
#else
constexpr bool kRoundLayout = false;
constexpr int kPanelW = 720;
constexpr int kPanelH = 720;
constexpr int kContentW = 560;
constexpr int kTitleY = 140;
constexpr int kGuideY = 240;
constexpr int kSsidY = 300;
constexpr int kBrowserY = 400;
constexpr int kUrlY = 460;
#endif

struct TipUi {
    lv_obj_t* screen = nullptr;
    bool active = false;
};

TipUi s_ui;

void OnScreenDeleted(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    if (lv_event_get_target_obj(e) == s_ui.screen) {
        s_ui = {};
    }
}

lv_obj_t* MakeLabel(lv_obj_t* parent, const char* text, lv_color_t color,
                    const lv_font_t* font, int y) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text != nullptr ? text : "");
    lv_obj_set_width(lbl, kContentW);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, color, LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, font, LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, y);
    return lbl;
}

// Alert 拼接串里 ACCESS_VIA_BROWSER 常带前导「，」/空格；单独成行时去掉。
const char* BrowserGuideText() {
    const char* text = Lang::Strings::ACCESS_VIA_BROWSER;
    while (text != nullptr && *text != '\0') {
        const unsigned char c = static_cast<unsigned char>(*text);
        if (c == ' ' || c == ',' || c == ':') {
            ++text;
            continue;
        }
        // UTF-8 「，」(E3 80 81) / 「：」(EF BC 9A) 前缀
        if ((c == 0xE3 && static_cast<unsigned char>(text[1]) == 0x80 &&
             static_cast<unsigned char>(text[2]) == 0x81) ||
            (c == 0xEF && static_cast<unsigned char>(text[1]) == 0xBC &&
             static_cast<unsigned char>(text[2]) == 0x9A)) {
            text += 3;
            continue;
        }
        break;
    }
    return text != nullptr ? text : "";
}

lv_obj_t* BuildScreen(const char* title, const char* ssid, const char* url) {
    lv_obj_t* screen = lv_obj_create(nullptr);
    screen_strip_obj_chrome(screen);
    lv_obj_set_size(screen, kPanelW, kPanelH);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0A0D12), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    const lv_font_t* title_font =
        kRoundLayout ? &font_puhui_20_4 : &font_puhui_30_4;
    const lv_font_t* body_font = &font_puhui_20_4;
    const lv_font_t* accent_font =
        kRoundLayout ? &font_puhui_20_4 : &font_puhui_30_4;

    MakeLabel(screen,
              title != nullptr ? title : Lang::Strings::WIFI_CONFIG_MODE,
              lv_color_white(), title_font, kTitleY);

    MakeLabel(screen, Lang::Strings::CONNECT_TO_HOTSPOT,
              lv_color_hex(0x9AA3B2), body_font, kGuideY);

    MakeLabel(screen, ssid != nullptr ? ssid : "",
              lv_color_hex(0x60A5FA), accent_font, kSsidY);

    MakeLabel(screen, BrowserGuideText(), lv_color_hex(0x9AA3B2), body_font,
              kBrowserY);

    MakeLabel(screen, url != nullptr ? url : "",
              lv_color_hex(0x34D399), accent_font, kUrlY);

    lv_obj_add_event_cb(screen, OnScreenDeleted, LV_EVENT_DELETE, nullptr);
    return screen;
}

}  // namespace

void WifiConfigTipScreen::Show(const char* title, const char* ssid, const char* url) {
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }

    lv_obj_t* old_scr = lv_screen_active();

    if (s_ui.active && s_ui.screen != nullptr && s_ui.screen != old_scr) {
        lv_obj_delete(s_ui.screen);
        s_ui = {};
    }

    s_ui.screen = BuildScreen(title, ssid, url);
    s_ui.active = true;
    lv_screen_load(s_ui.screen);

    // 结束开机动画：切走后删除旧屏（通常是 BootScreen）。
    if (old_scr != nullptr && old_scr != s_ui.screen) {
        lv_obj_delete(old_scr);
    }

    esp_lv_adapter_refresh_now(nullptr);
    esp_lv_adapter_unlock();
    ESP_LOGI(TAG, "WiFi config tip shown ssid=%s url=%s",
             ssid != nullptr ? ssid : "", url != nullptr ? url : "");
}

bool WifiConfigTipScreen::IsActive() {
    return s_ui.active;
}
