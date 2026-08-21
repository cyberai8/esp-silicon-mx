#include "standby_screen.h"
#include "config.h"
#include "i18n.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_lv_adapter.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#if defined(ESP_PLATFORM)
#include "freertos/idf_additions.h"
#endif

#include "Weather.hpp"
#include "application.h"
#include "board.h"
#include "home_screen/home_screen.h"
#include "idle_power_policy.h"
#include "pwr_key_handler.h"
#include "settings.h"
#include "weather_icon_map.h"
#include "https_request_lock.h"
#include <wifi_station.h>

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_puhui_number_120_4);

namespace {

constexpr const char* TAG = "StandbyScreen";
#if defined(BOARD_ESP_VOCAT) || (DISPLAY_WIDTH == 360 && DISPLAY_HEIGHT == 360)
constexpr bool kRoundSmall = true;
constexpr int kPanelSize = DISPLAY_WIDTH;
#else
constexpr bool kRoundSmall = false;
constexpr int kPanelSize = 720;
#endif

constexpr int kChargeFxW = kRoundSmall ? kPanelSize : 560;
constexpr int kChargeFxH = kRoundSmall ? 160 : 360;
constexpr int kParticleCount = kRoundSmall ? 28 : 52;
constexpr uint32_t kChargeBlueSoft = 0x59B2FF;
constexpr uint32_t kChargeBlueBright = 0x9AD0FF;
constexpr uint32_t kParticleTickMs = 33;

// 翻页时钟：大屏 HH MM SS；圆屏 HH MM，用 120 号数字放大填满安全区。
constexpr int kDigitCount = kRoundSmall ? 4 : 6;
constexpr int kDigitW = kRoundSmall ? 68 : 88;
constexpr int kDigitH = kRoundSmall ? 112 : 148;
constexpr int kDigitHalf = kDigitH / 2;
constexpr int kPairGap = kRoundSmall ? 3 : 6;
constexpr int kGroupGap = kRoundSmall ? 16 : 32;
constexpr uint32_t kCardBg = 0x1C1C1E;
constexpr uint32_t kCardBgTop = 0x2A2A2E;
constexpr uint32_t kHingeColor = 0x0A0A0A;
constexpr uint32_t kDigitColor = 0xF5F5F7;
constexpr int32_t kFlipProgressMax = kDigitHalf * 2;
constexpr uint32_t kFlipDurationMs = 480;
constexpr int kSwipeFaceThreshold = kRoundSmall ? 48 : 80;
constexpr int kTapSlop = 24;
// 资源为 128×128。显示边长需与缩放一致；图标 PNG 自带黑底。
constexpr int kWeatherIconSize = kRoundSmall ? 84 : 112;
constexpr int kWeatherPanelW = kRoundSmall ? 292 : (kPanelSize - 64);
constexpr int kWeatherCardW = kRoundSmall ? 276 : (kPanelSize - 96);
constexpr int kWeatherChipH = kRoundSmall ? 58 : 68;
constexpr uint32_t kWeatherCardBg = 0x171C24;
constexpr uint32_t kWeatherChipBg = 0x222833;
constexpr uint32_t kWeatherRefreshOkSec = 15 * 60;
constexpr uint32_t kWeatherRefreshFailSec = 60;
constexpr const char* kStandbyNvsNs = "standby";
constexpr const char* kStandbyFaceKey = "face";

const lv_font_t* StandbyDigitFont() {
    // 圆屏也用 120 字体，卡片高度 112 可容纳，视觉接近大屏。
    return &font_puhui_number_120_4;
}

// msgid 源文案（zh-CN）；显示时再 I18n::T，禁止在静态初始化里调用 T()。
constexpr const char* kWeekdayMsgIds[7] = {"日", "一", "二", "三", "四", "五", "六"};

struct Particle {
    lv_obj_t* obj = nullptr;
    float x = 0;
    float y = 0;
    float vx = 0;
    float vy = 0;
    float size = 6;
    float life = 0;      // 0..1，1 为新生
    float life_decay = 0;
};

// 静态上/下半区 + 同一片翻页叶（从上半翻过铰链落到下半）。
struct FlipDigit {
    lv_obj_t* card = nullptr;
    lv_obj_t* top_clip = nullptr;
    lv_obj_t* top_lbl = nullptr;
    lv_obj_t* bot_clip = nullptr;
    lv_obj_t* bot_lbl = nullptr;
    lv_obj_t* flap = nullptr;       // 唯一翻页叶
    lv_obj_t* flap_lbl = nullptr;
    lv_obj_t* hinge = nullptr;
    lv_obj_t* shade = nullptr;
    int32_t label_ofs_y = 0;
    char current = '0';
    char target = 0;   // 当前翻页动画的目标数字
    char pending = 0;  // 动画中又有新值时排队
    char old_ch = '0'; // 本轮翻页的旧数字（叶正面）
    bool flap_lower = false;  // 叶是否已翻到下半
    bool animating = false;
};

struct UiState {
    lv_obj_t* screen = nullptr;
    lv_obj_t* clock_panel = nullptr;
    lv_obj_t* clock_row = nullptr;
    FlipDigit digits[kDigitCount]{};
    lv_obj_t* date_lbl = nullptr;
    lv_obj_t* activation_title_lbl = nullptr;
    lv_obj_t* activation_code_lbl = nullptr;
    lv_timer_t* update_timer = nullptr;

    lv_obj_t* weather_panel = nullptr;
    lv_obj_t* weather_card = nullptr;
    lv_obj_t* weather_icon = nullptr;
    lv_obj_t* weather_temp_lbl = nullptr;
    lv_obj_t* weather_text_lbl = nullptr;
    lv_obj_t* weather_loc_lbl = nullptr;
    lv_obj_t* weather_chip_low_val = nullptr;
    lv_obj_t* weather_chip_high_val = nullptr;
    lv_obj_t* weather_chip_meta_title = nullptr;
    lv_obj_t* weather_chip_meta_val = nullptr;
    lv_obj_t* face_dots = nullptr;
    lv_obj_t* face_dot_weather = nullptr;
    lv_obj_t* face_dot_clock = nullptr;

    lv_obj_t* charge_root = nullptr;
    lv_obj_t* charge_tip = nullptr;
    Particle particles[kParticleCount]{};
    lv_timer_t* charge_tick_timer = nullptr;
    uint32_t charge_rng = 1;
    bool charge_playing = false;
    bool last_charging = false;
    bool charge_primed = false;
    bool clock_primed = false;

    StandbyFace face = StandbyFace::Weather;
    uint32_t weather_ticks = 0;
    bool weather_ok = false;
    bool weather_fetching = false;
    int16_t press_x = 0;
    int16_t press_y = 0;
    bool press_tracking = false;
};

UiState s_ui;
std::atomic<uint32_t> s_weather_session{0};

StandbyFace LoadPreferredFace() {
    Settings settings(kStandbyNvsNs, false);
    const int stored = settings.GetInt(kStandbyFaceKey, static_cast<int>(StandbyFace::Weather));
    return stored == static_cast<int>(StandbyFace::Clock) ? StandbyFace::Clock
                                                          : StandbyFace::Weather;
}

void SavePreferredFace(StandbyFace face) {
    Settings settings(kStandbyNvsNs, true);
    settings.SetInt(kStandbyFaceKey, static_cast<int>(face));
}

void ApplyFaceVisibility();
void TriggerWeatherFetch();
void ApplyWeatherData(const WeatherDistrictData& data, bool ok);

void FlipDigitSetChar(lv_obj_t* lbl, char ch) {
    if (lbl == nullptr) {
        return;
    }
    char buf[2] = {ch, '\0'};
    lv_label_set_text(lbl, buf);
}

void FlipDigitRaiseDecor(FlipDigit* d) {
    if (d == nullptr) {
        return;
    }
    if (d->shade != nullptr) {
        lv_obj_move_foreground(d->shade);
    }
    if (d->hinge != nullptr) {
        lv_obj_move_foreground(d->hinge);
    }
}

// progress: 0 = 上半叶全开（旧数字正面）
//           kDigitHalf = 贴住铰链并翻面
//           kFlipProgressMax = 下半叶全开（新数字背面落下）
void FlipDigitApplyProgress(FlipDigit* d, int32_t progress) {
    if (d == nullptr || d->flap == nullptr) {
        return;
    }
    if (progress < 0) {
        progress = 0;
    }
    if (progress > kFlipProgressMax) {
        progress = kFlipProgressMax;
    }

    const bool lower = progress > kDigitHalf;
    if (lower != d->flap_lower) {
        d->flap_lower = lower;
        if (lower) {
            // 过铰链：同一片叶翻到背面，显示新数字下半，从铰链往下展开。
            FlipDigitSetChar(d->flap_lbl, d->target);
            lv_obj_set_style_bg_color(d->flap, lv_color_hex(kCardBg),
                                      LV_PART_MAIN);
        } else {
            FlipDigitSetChar(d->flap_lbl, d->old_ch);
            lv_obj_set_style_bg_color(d->flap, lv_color_hex(kCardBgTop),
                                      LV_PART_MAIN);
        }
    }

    int32_t h;
    int32_t y;
    int32_t lbl_y;
    if (!lower) {
        // 上半：底部贴铰链，高度 half→0（叶面向下翻）
        h = kDigitHalf - progress;
        if (h < 1) {
            h = 1;
        }
        y = kDigitHalf - h;
        lbl_y = d->label_ofs_y - y;
    } else {
        // 下半：顶部贴铰链，高度 0→half（叶背继续翻下）
        h = progress - kDigitHalf;
        if (h < 1) {
            h = 1;
        }
        y = kDigitHalf;
        lbl_y = d->label_ofs_y - kDigitHalf;
    }

    lv_obj_set_pos(d->flap, 0, y);
    lv_obj_set_height(d->flap, h);
    lv_obj_set_y(d->flap_lbl, lbl_y);

    // 越接近铰链越暗，模拟翻页透视阴影。
    const int32_t dist =
        lower ? (kFlipProgressMax - progress) : progress;  // 0 at open, half at hinge
    const lv_opa_t shade =
        static_cast<lv_opa_t>(dist * 160 / kDigitHalf);
    if (d->shade != nullptr) {
        lv_obj_set_pos(d->shade, 0, y);
        lv_obj_set_height(d->shade, h);
        lv_obj_set_style_bg_opa(d->shade, shade, LV_PART_MAIN);
        lv_obj_remove_flag(d->shade, LV_OBJ_FLAG_HIDDEN);
    }
}

void FlipDigitFinish(FlipDigit* d, char ch);
void FlipDigitStartFlip(FlipDigit* d, char next);

void FlipDigitFinish(FlipDigit* d, char ch) {
    if (d == nullptr) {
        return;
    }
    FlipDigitSetChar(d->top_lbl, ch);
    FlipDigitSetChar(d->bot_lbl, ch);
    d->current = ch;
    d->target = 0;
    d->flap_lower = false;
    if (d->flap != nullptr) {
        lv_obj_add_flag(d->flap, LV_OBJ_FLAG_HIDDEN);
    }
    if (d->shade != nullptr) {
        lv_obj_add_flag(d->shade, LV_OBJ_FLAG_HIDDEN);
    }
    d->animating = false;

    const char queued = d->pending;
    d->pending = 0;
    if (queued != 0 && queued != d->current) {
        FlipDigitStartFlip(d, queued);
    }
}

void FlipDigitStartFlip(FlipDigit* d, char next) {
    if (d == nullptr || d->card == nullptr || d->flap == nullptr) {
        return;
    }
    if (next == d->current && !d->animating) {
        return;
    }
    if (d->animating) {
        d->pending = next;
        return;
    }

    d->old_ch = d->current;
    d->target = next;
    d->animating = true;
    d->flap_lower = false;

    // 静态：上半已是新数字（被叶盖住），下半仍是旧数字（等叶翻下来盖住）。
    FlipDigitSetChar(d->top_lbl, next);
    FlipDigitSetChar(d->bot_lbl, d->old_ch);
    FlipDigitSetChar(d->flap_lbl, d->old_ch);
    lv_obj_set_style_bg_color(d->flap, lv_color_hex(kCardBgTop), LV_PART_MAIN);

    lv_obj_remove_flag(d->flap, LV_OBJ_FLAG_HIDDEN);
    FlipDigitApplyProgress(d, 0);
    FlipDigitRaiseDecor(d);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, d);
    lv_anim_set_values(&a, 0, kFlipProgressMax);
    lv_anim_set_duration(&a, kFlipDurationMs);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_user_data(&a, d);
    lv_anim_set_exec_cb(&a, [](void* var, int32_t v) {
        FlipDigitApplyProgress(static_cast<FlipDigit*>(var), v);
    });
    lv_anim_set_completed_cb(&a, [](lv_anim_t* anim) {
        FlipDigit* digit = static_cast<FlipDigit*>(lv_anim_get_user_data(anim));
        if (digit == nullptr) {
            return;
        }
        FlipDigitFinish(digit, digit->target);
    });
    lv_anim_start(&a);
}

void FlipDigitSet(FlipDigit* d, char ch, bool animate) {
    if (d == nullptr || ch < '0' || ch > '9') {
        return;
    }
    if (!animate || d->card == nullptr) {
        lv_anim_delete(d, nullptr);
        d->pending = 0;
        d->target = 0;
        d->animating = false;
        d->flap_lower = false;
        FlipDigitSetChar(d->top_lbl, ch);
        FlipDigitSetChar(d->bot_lbl, ch);
        d->current = ch;
        if (d->flap != nullptr) {
            lv_obj_add_flag(d->flap, LV_OBJ_FLAG_HIDDEN);
        }
        if (d->shade != nullptr) {
            lv_obj_add_flag(d->shade, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    if (ch == d->current && !d->animating) {
        return;
    }
    FlipDigitStartFlip(d, ch);
}

lv_obj_t* CreateHalfClip(lv_obj_t* parent, int y, uint32_t bg) {
    lv_obj_t* clip = lv_obj_create(parent);
    lv_obj_remove_style_all(clip);
    lv_obj_set_size(clip, kDigitW, kDigitHalf);
    lv_obj_set_pos(clip, 0, y);
    lv_obj_set_style_bg_color(clip, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(clip, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(clip, true, LV_PART_MAIN);
    lv_obj_set_style_radius(clip, 0, LV_PART_MAIN);
    lv_obj_remove_flag(clip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(clip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(clip, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    return clip;
}

lv_obj_t* CreateDigitLabel(lv_obj_t* parent, int32_t y) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, "0");
    lv_obj_set_style_text_font(lbl, StandbyDigitFont(), LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(kDigitColor), LV_PART_MAIN);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(lbl, kDigitW);
    lv_obj_set_pos(lbl, 0, y);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return lbl;
}

void CreateFlipDigit(lv_obj_t* parent, FlipDigit* d) {
    const int32_t line_h = StandbyDigitFont()->line_height;
    d->label_ofs_y = (kDigitH - line_h) / 2;

    d->card = lv_obj_create(parent);
    lv_obj_remove_style_all(d->card);
    lv_obj_set_size(d->card, kDigitW, kDigitH);
    lv_obj_set_style_radius(d->card, kRoundSmall ? 8 : 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(d->card, lv_color_hex(kCardBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(d->card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(d->card, true, LV_PART_MAIN);
    lv_obj_remove_flag(d->card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(d->card, LV_OBJ_FLAG_CLICKABLE);

    d->top_clip = CreateHalfClip(d->card, 0, kCardBgTop);
    d->top_lbl = CreateDigitLabel(d->top_clip, d->label_ofs_y);

    d->bot_clip = CreateHalfClip(d->card, kDigitHalf, kCardBg);
    d->bot_lbl = CreateDigitLabel(d->bot_clip, d->label_ofs_y - kDigitHalf);

    // 唯一翻页叶：先盖住上半（旧数字），过铰链后落到下半（新数字）。
    d->flap = CreateHalfClip(d->card, 0, kCardBgTop);
    d->flap_lbl = CreateDigitLabel(d->flap, d->label_ofs_y);
    lv_obj_add_flag(d->flap, LV_OBJ_FLAG_HIDDEN);

    d->shade = lv_obj_create(d->card);
    lv_obj_remove_style_all(d->shade);
    lv_obj_set_size(d->shade, kDigitW, kDigitHalf);
    lv_obj_set_style_bg_color(d->shade, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(d->shade, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(d->shade, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(d->shade, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(d->shade, LV_OBJ_FLAG_HIDDEN);

    d->hinge = lv_obj_create(d->card);
    lv_obj_remove_style_all(d->hinge);
    lv_obj_set_size(d->hinge, kDigitW, 3);
    lv_obj_set_pos(d->hinge, 0, kDigitHalf - 1);
    lv_obj_set_style_bg_color(d->hinge, lv_color_hex(kHingeColor), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(d->hinge, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(d->hinge, LV_OBJ_FLAG_CLICKABLE);

    d->current = '0';
    d->target = 0;
    d->pending = 0;
    d->old_ch = '0';
    d->flap_lower = false;
    d->animating = false;
}

lv_obj_t* CreateFlipClockRow(lv_obj_t* parent) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 0, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < kDigitCount; ++i) {
        if (i > 0) {
            const bool group_break = (i % 2 == 0);
            lv_obj_t* gap = lv_obj_create(row);
            lv_obj_remove_style_all(gap);
            lv_obj_set_size(gap, group_break ? kGroupGap : kPairGap, 1);
            lv_obj_set_style_bg_opa(gap, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_remove_flag(gap, LV_OBJ_FLAG_CLICKABLE);
        }
        CreateFlipDigit(row, &s_ui.digits[i]);
    }
    return row;
}

lv_obj_t* MakeStandbyLabel(lv_obj_t* parent, const char* text, const lv_font_t* font,
                           uint32_t color) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return lbl;
}

void SetStandbyWeatherIcon(lv_obj_t* icon, const WeatherDistrictData& data) {
    if (icon == nullptr) {
        return;
    }

    auto remap_missing = [](const std::string& raw) -> const char* {
        if (raw.empty()) {
            return nullptr;
        }
        // 资源包无 102/103/夜码等，映射到最接近的已有图。
        if (raw == "102" || raw == "151" || raw == "152") {
            return "101";
        }
        if (raw == "103" || raw == "153") {
            // 无「晴间多云」专图；104 资源实为日+云，比纯晴 100 更贴切。
            return "104";
        }
        if (raw == "150") {
            return "100";
        }
        if (raw == "154") {
            return "104";
        }
        return raw.c_str();
    };

    auto has_asset = [](const char* code) -> bool {
        if (code == nullptr || code[0] == '\0') {
            return false;
        }
        // 与 main/xingzhi-assets/ic_s_weather_*.png 对齐的常用码。
        static constexpr const char* kCodes[] = {
            "100", "101", "104", "300", "302", "304", "305", "306", "307",
            "310", "311", "312", "313", "314", "315", "316", "317", "318",
            "399", "400", "401", "402", "403", "404", "407", "408", "409",
            "410", "499", "500", "501", "502", "503", "504", "507", "508",
            "509", "510", "511", "512", "513", "514", "515",
        };
        for (const char* c : kCodes) {
            if (std::strcmp(c, code) == 0) {
                return true;
            }
        }
        return false;
    };

    const char* code = nullptr;
    // 优先中文实况（device/weather 的 wea 比粗粒度 weaImg 更准，如 阴 vs yun）
    if (!data.text.empty()) {
        code = WeatherIconCodeForText(data.text);
    }
    // 其次接口 icon / 已映射的 weaImg 码
    if ((code == nullptr || !has_asset(code)) && !data.icon.empty()) {
        const char* mapped = remap_missing(data.icon);
        if (has_asset(mapped)) {
            code = mapped;
        }
    }
    // 今日预报文案
    if ((code == nullptr || !has_asset(code)) && !data.forecasts.empty() &&
        !data.forecasts.front().text_day.empty()) {
        code = WeatherIconCodeForText(data.forecasts.front().text_day);
    }
    if (code == nullptr || !has_asset(code)) {
        code = "100";
    }

    char path[48];
    std::snprintf(path, sizeof(path), "A:ic_s_weather_%s.spng", code);
    // 与天气页一致：不 set_scale。圆屏缓冲偏小，变换层常画不出导致「空框」。
    lv_obj_set_size(icon, kWeatherIconSize, kWeatherIconSize);
    lv_image_set_src(icon, path);
    lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CENTER);
    lv_obj_center(icon);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "weather icon code=%s text=%s api_icon=%s path=%s", code,
             data.text.c_str(), data.icon.c_str(), path);
}

void StyleFaceDot(lv_obj_t* dot, bool active) {
    if (dot == nullptr) {
        return;
    }
    lv_obj_set_style_bg_opa(dot, active ? LV_OPA_COVER : LV_OPA_40, LV_PART_MAIN);
}

void UpdateFaceDots() {
    const bool weather = s_ui.face == StandbyFace::Weather;
    StyleFaceDot(s_ui.face_dot_weather, weather);
    StyleFaceDot(s_ui.face_dot_clock, !weather);
}

void ApplyFaceVisibility() {
    auto& app = Application::GetInstance();
    const bool activating = app.HasPendingActivation();
    if (s_ui.activation_title_lbl != nullptr) {
        if (activating) {
            lv_label_set_text(s_ui.activation_code_lbl,
                              app.GetPendingActivationCode().c_str());
            lv_obj_remove_flag(s_ui.activation_title_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_ui.activation_code_lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_ui.activation_title_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_ui.activation_code_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }

    const bool show_weather = !activating && s_ui.face == StandbyFace::Weather;
    const bool show_clock = !activating && s_ui.face == StandbyFace::Clock;
    if (s_ui.weather_panel != nullptr) {
        if (show_weather) {
            lv_obj_remove_flag(s_ui.weather_panel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_ui.weather_panel, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_ui.clock_panel != nullptr) {
        if (show_clock) {
            lv_obj_remove_flag(s_ui.clock_panel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_ui.clock_panel, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_ui.face_dots != nullptr) {
        if (activating || s_ui.charge_playing) {
            lv_obj_add_flag(s_ui.face_dots, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_ui.face_dots, LV_OBJ_FLAG_HIDDEN);
        }
    }
    UpdateFaceDots();
}

void SwitchStandbyFace(StandbyFace face) {
    if (s_ui.face == face) {
        ApplyFaceVisibility();
        return;
    }
    s_ui.face = face;
    SavePreferredFace(face);
    ApplyFaceVisibility();
}

void ApplyWeatherData(const WeatherDistrictData& data, bool ok) {
    if (s_ui.weather_temp_lbl == nullptr) {
        return;
    }
    s_ui.weather_ok = ok && data.valid;
    SetStandbyWeatherIcon(s_ui.weather_icon, data);

    if (!s_ui.weather_ok) {
        lv_label_set_text(s_ui.weather_temp_lbl, "--°");
        lv_label_set_text(s_ui.weather_text_lbl, I18n::T("暂无天气数据"));
        lv_label_set_text(s_ui.weather_loc_lbl, "");
        lv_label_set_text(s_ui.weather_chip_low_val, "--");
        lv_label_set_text(s_ui.weather_chip_high_val, "--");
        lv_label_set_text(s_ui.weather_chip_meta_title, I18n::T("状态"));
        lv_label_set_text(s_ui.weather_chip_meta_val, I18n::T("刷新中"));
        return;
    }

    char temp[24];
    std::snprintf(temp, sizeof(temp), "%d°", static_cast<int>(data.temp));
    lv_label_set_text(s_ui.weather_temp_lbl, temp);

    const char* weather_text = data.text.c_str();
    if (data.text.empty() && !data.forecasts.empty() &&
        !data.forecasts.front().text_day.empty()) {
        weather_text = data.forecasts.front().text_day.c_str();
    }
    lv_label_set_text(s_ui.weather_text_lbl,
                      (weather_text == nullptr || weather_text[0] == '\0')
                          ? I18n::T("天气")
                          : weather_text);

    char loc[96];
    if (!data.district.empty() && !data.city.empty() && data.district != data.city) {
        std::snprintf(loc, sizeof(loc), "%s · %s", data.city.c_str(),
                      data.district.c_str());
    } else if (!data.city.empty()) {
        std::snprintf(loc, sizeof(loc), "%s", data.city.c_str());
    } else if (!data.district.empty()) {
        std::snprintf(loc, sizeof(loc), "%s", data.district.c_str());
    } else {
        loc[0] = '\0';
    }
    lv_label_set_text(s_ui.weather_loc_lbl, loc);

    if (!data.forecasts.empty()) {
        const WeatherForecastDay& today = data.forecasts.front();
        char low[16];
        char high[16];
        std::snprintf(low, sizeof(low), "%d°", static_cast<int>(today.low));
        std::snprintf(high, sizeof(high), "%d°", static_cast<int>(today.high));
        lv_label_set_text(s_ui.weather_chip_low_val, low);
        lv_label_set_text(s_ui.weather_chip_high_val, high);
    } else {
        lv_label_set_text(s_ui.weather_chip_low_val, "--");
        lv_label_set_text(s_ui.weather_chip_high_val, "--");
    }

    if (data.rh > 0) {
        lv_label_set_text(s_ui.weather_chip_meta_title, I18n::T("湿度"));
        char rh[16];
        std::snprintf(rh, sizeof(rh), "%d%%", static_cast<int>(data.rh));
        lv_label_set_text(s_ui.weather_chip_meta_val, rh);
    } else if (!data.air.empty()) {
        lv_label_set_text(s_ui.weather_chip_meta_title, I18n::T("空气"));
        lv_label_set_text(s_ui.weather_chip_meta_val, data.air.c_str());
    } else if (!data.wind_dir.empty()) {
        lv_label_set_text(s_ui.weather_chip_meta_title, I18n::T("风向"));
        lv_label_set_text(s_ui.weather_chip_meta_val,
                          I18n::T(data.wind_dir.c_str()));
    } else {
        lv_label_set_text(s_ui.weather_chip_meta_title, I18n::T("空气"));
        lv_label_set_text(s_ui.weather_chip_meta_val, "--");
    }
}

void WeatherFetchTask(void* arg) {
    const uint32_t my_session = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));

    auto finish_without_fetch = [&]() {
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            if (s_weather_session.load() == my_session && s_ui.screen != nullptr) {
                s_ui.weather_fetching = false;
            }
            esp_lv_adapter_unlock();
        }
        vTaskDelete(nullptr);
    };

    if (!WifiStation::GetInstance().IsConnected()) {
        finish_without_fetch();
        return;
    }

    for (int i = 0; i < 120; ++i) {
        if (s_weather_session.load() != my_session || s_ui.screen == nullptr) {
            finish_without_fetch();
            return;
        }
        if (Application::GetInstance().IsBackgroundNetworkReady()) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!Application::GetInstance().IsBackgroundNetworkReady()) {
        finish_without_fetch();
        return;
    }
    if (s_weather_session.load() != my_session || s_ui.screen == nullptr ||
        Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
        finish_without_fetch();
        return;
    }

    if (!HttpsInternalRamReady()) {
        ESP_LOGW(TAG, "weather deferred, largest_int=%u internal=%u",
                 static_cast<unsigned>(HttpsLargestInternalBlock()),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
        finish_without_fetch();
        return;
    }

    bool restore_wake = false;
    if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
        restore_wake =
            Application::GetInstance().GetAudioService().ReleaseWakeWordDetection();
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    WeatherDistrictData data;
    const esp_err_t err = WeatherService::Instance().FetchByDevice(data);

    if (restore_wake &&
        Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
        Application::GetInstance().GetAudioService().EnableWakeWordDetection(true);
    }
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        if (s_weather_session.load() == my_session && s_ui.screen != nullptr) {
            s_ui.weather_fetching = false;
            ApplyWeatherData(data, err == ESP_OK);
        }
        esp_lv_adapter_unlock();
    }
    vTaskDelete(nullptr);
}

void TriggerWeatherFetch() {
    if (s_ui.weather_fetching || s_ui.screen == nullptr) {
        return;
    }
    if (!WifiStation::GetInstance().IsConnected()) {
        return;
    }
    // 注意：IsBackgroundNetworkReady 的检查移到任务内部完成，
    // 任务会等待最多 60 秒，避免待机屏比网络初始化更早创建时 fetch 被直接丢弃。
    if (!HttpsInternalRamReady()) {
        return;
    }
    if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
        return;
    }
    s_ui.weather_fetching = true;
    const uint32_t session =
        s_weather_session.fetch_add(1, std::memory_order_relaxed) + 1;
    void* arg = reinterpret_cast<void*>(static_cast<uintptr_t>(session));

    // HTTPS 栈放 PSRAM：待机页刚建完 LVGL 对象后，内部 DRAM 最大连续块
    // 往往不够 12KB，普通 xTaskCreate 会一直失败。
    BaseType_t ok = pdFAIL;
#if defined(ESP_PLATFORM)
    static constexpr uint32_t kStackBytes[] = {12 * 1024, 10 * 1024, 8 * 1024};
    for (uint32_t bytes : kStackBytes) {
        if (xTaskCreateWithCaps(WeatherFetchTask, "stby_weather", bytes, arg, 4,
                                nullptr, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) ==
            pdPASS) {
            ok = pdPASS;
            break;
        }
    }
#else
    ok = xTaskCreate(WeatherFetchTask, "stby_weather", 12 * 1024, arg, 4,
                     nullptr);
#endif
    if (ok != pdPASS) {
        s_ui.weather_fetching = false;
        ESP_LOGW(TAG,
                 "weather fetch task failed heap=%u internal=%u largest_int=%u spiram=%u",
                 static_cast<unsigned>(esp_get_free_heap_size()),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(
                     heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    }
}

lv_obj_t* MakeMetricChip(lv_obj_t* parent, const char* title, lv_obj_t** out_val) {
    lv_obj_t* chip = lv_obj_create(parent);
    lv_obj_remove_style_all(chip);
    lv_obj_set_flex_grow(chip, 1);
    lv_obj_set_height(chip, kWeatherChipH);
    lv_obj_set_style_radius(chip, kRoundSmall ? 12 : 14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(chip, lv_color_hex(kWeatherChipBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(chip, kRoundSmall ? 4 : 6, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(chip, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(chip, 0, LV_PART_MAIN);
    lv_obj_remove_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(chip, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* title_lbl =
        MakeStandbyLabel(chip, title, &font_puhui_20_4, 0x8B95A8);
    lv_obj_set_style_text_opa(title_lbl, LV_OPA_80, LV_PART_MAIN);

    // 中文可用字体仅到 30；数值尽量用大号。
    lv_obj_t* val_lbl = MakeStandbyLabel(chip, "--", &font_puhui_30_4, 0xF8FAFC);
    if (out_val != nullptr) {
        *out_val = val_lbl;
    }
    return chip;
}

lv_obj_t* CreateWeatherPanel(lv_obj_t* parent) {
    lv_obj_t* box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, kWeatherPanelW, LV_SIZE_CONTENT);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, kRoundSmall ? -10 : -2);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, kRoundSmall ? 10 : 14, LV_PART_MAIN);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);

    // 主信息卡：左图标框 + 右温度/天气/地点
    s_ui.weather_card = lv_obj_create(box);
    lv_obj_remove_style_all(s_ui.weather_card);
    lv_obj_set_size(s_ui.weather_card, kWeatherCardW, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(s_ui.weather_card, kRoundSmall ? 18 : 22, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.weather_card, lv_color_hex(kWeatherCardBg),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.weather_card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(s_ui.weather_card, false, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ui.weather_card, kRoundSmall ? 10 : 14, LV_PART_MAIN);
    lv_obj_set_style_pad_column(s_ui.weather_card, kRoundSmall ? 10 : 14,
                                LV_PART_MAIN);
    lv_obj_set_flex_flow(s_ui.weather_card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_ui.weather_card, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(s_ui.weather_card, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_remove_flag(s_ui.weather_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_ui.weather_card, LV_OBJ_FLAG_CLICKABLE);

    // 图标框：控件与框同尺寸，图案 STRETCH 后正好居中铺满。
    lv_obj_t* icon_wrap = lv_obj_create(s_ui.weather_card);
    lv_obj_remove_style_all(icon_wrap);
    lv_obj_set_size(icon_wrap, kWeatherIconSize, kWeatherIconSize);
    lv_obj_set_style_radius(icon_wrap, kRoundSmall ? 14 : 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(icon_wrap, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(icon_wrap, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(icon_wrap, true, LV_PART_MAIN);
    lv_obj_remove_flag(icon_wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(icon_wrap, LV_OBJ_FLAG_CLICKABLE);

    s_ui.weather_icon = lv_image_create(icon_wrap);
    lv_obj_set_size(s_ui.weather_icon, kWeatherIconSize, kWeatherIconSize);
    lv_obj_center(s_ui.weather_icon);
    lv_image_set_inner_align(s_ui.weather_icon, LV_IMAGE_ALIGN_CENTER);
    lv_obj_remove_flag(s_ui.weather_icon, LV_OBJ_FLAG_CLICKABLE);
    SetStandbyWeatherIcon(s_ui.weather_icon, WeatherDistrictData{});

    lv_obj_t* info_col = lv_obj_create(s_ui.weather_card);
    lv_obj_remove_style_all(info_col);
    lv_obj_set_flex_grow(info_col, 1);
    lv_obj_set_height(info_col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(info_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(info_col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(info_col, kRoundSmall ? 2 : 4, LV_PART_MAIN);
    lv_obj_remove_flag(info_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(info_col, LV_OBJ_FLAG_CLICKABLE);

    s_ui.weather_temp_lbl =
        MakeStandbyLabel(info_col, "--°", &font_puhui_30_4, 0xFFFFFF);
    lv_obj_set_style_text_align(s_ui.weather_temp_lbl, LV_TEXT_ALIGN_LEFT,
                                LV_PART_MAIN);
    s_ui.weather_text_lbl =
        MakeStandbyLabel(info_col, I18n::T("加载中..."), &font_puhui_30_4,
                         0xE5E7EB);
    lv_obj_set_style_text_align(s_ui.weather_text_lbl, LV_TEXT_ALIGN_LEFT,
                                LV_PART_MAIN);
    s_ui.weather_loc_lbl =
        MakeStandbyLabel(info_col, "", &font_puhui_20_4, 0x9CA3AF);
    lv_obj_set_style_text_align(s_ui.weather_loc_lbl, LV_TEXT_ALIGN_LEFT,
                                LV_PART_MAIN);
    lv_obj_set_width(s_ui.weather_temp_lbl, LV_PCT(100));
    lv_obj_set_width(s_ui.weather_text_lbl, LV_PCT(100));
    lv_obj_set_width(s_ui.weather_loc_lbl, LV_PCT(100));
    lv_label_set_long_mode(s_ui.weather_text_lbl, LV_LABEL_LONG_CLIP);
    lv_label_set_long_mode(s_ui.weather_loc_lbl, LV_LABEL_LONG_DOT);

    // 底部三枚指标卡：最低 / 最高 / 湿度(或空气)
    lv_obj_t* metrics = lv_obj_create(box);
    lv_obj_remove_style_all(metrics);
    lv_obj_set_size(metrics, kWeatherCardW, kWeatherChipH);
    lv_obj_set_flex_flow(metrics, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(metrics, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(metrics, kRoundSmall ? 8 : 10, LV_PART_MAIN);
    lv_obj_remove_flag(metrics, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(metrics, LV_OBJ_FLAG_CLICKABLE);

    MakeMetricChip(metrics, I18n::T("最低"), &s_ui.weather_chip_low_val);
    MakeMetricChip(metrics, I18n::T("最高"), &s_ui.weather_chip_high_val);
    lv_obj_t* meta_chip =
        MakeMetricChip(metrics, I18n::T("湿度"), &s_ui.weather_chip_meta_val);
    // 取芯片里第一个 label 作为可改标题（湿度/空气/风向）
    s_ui.weather_chip_meta_title = lv_obj_get_child(meta_chip, 0);

    return box;
}

lv_obj_t* CreateFaceDots(lv_obj_t* parent) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, 10);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, kRoundSmall ? -18 : -28);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    auto make_dot = [&]() {
        lv_obj_t* d = lv_obj_create(row);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 8, 8);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(d, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(d, LV_OPA_40, LV_PART_MAIN);
        lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
        return d;
    };
    s_ui.face_dot_weather = make_dot();
    s_ui.face_dot_clock = make_dot();
    return row;
}

void FormatDateText(char* buf, size_t len, const struct tm& tm_info, bool have_time) {
    if (!have_time) {
        std::snprintf(buf, len, "%s",
                      kRoundSmall ? I18n::T("--/-- 周-")
                                  : I18n::T("----年--月--日 星期-"));
        return;
    }
    const int wday = tm_info.tm_wday;
    const char* weekday =
        (wday >= 0 && wday < 7) ? I18n::T(kWeekdayMsgIds[wday]) : "-";
    if (kRoundSmall) {
        std::snprintf(buf, len, I18n::T("%02d/%02d 周%s"), tm_info.tm_mon + 1,
                      tm_info.tm_mday, weekday);
    } else {
        std::snprintf(buf, len, I18n::T("%04d年%02d月%02d日 星期%s"),
                      tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
                      weekday);
    }
}

void StopChargeEffect();
void StartChargeEffect(int battery_level);

uint32_t ChargeRand() {
    // xorshift32
    uint32_t x = s_ui.charge_rng ? s_ui.charge_rng : 1u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_ui.charge_rng = x;
    return x;
}

float ChargeRand01() {
    return static_cast<float>(ChargeRand() & 0xFFFFu) / 65535.0f;
}

void RespawnParticle(Particle* p, bool birth_at_bottom) {
    if (p == nullptr || p->obj == nullptr) {
        return;
    }
    // 更宽的出生带 + 略快衰减，同屏活跃点更多、翻滚更密。
    const float spread = kRoundSmall ? 140.0f : 260.0f;
    p->x = kChargeFxW * 0.5f + (ChargeRand01() - 0.5f) * spread;
    p->y = birth_at_bottom ? (kChargeFxH - 8.0f - ChargeRand01() * 36.0f)
                           : (kChargeFxH * (0.30f + ChargeRand01() * 0.60f));
    p->vx = (ChargeRand01() - 0.5f) * 2.2f;
    p->vy = -(1.6f + ChargeRand01() * 3.8f);  // 向上
    p->size = 3.0f + ChargeRand01() * 9.0f;
    p->life = 0.70f + ChargeRand01() * 0.30f;
    p->life_decay = 0.014f + ChargeRand01() * 0.022f;

    const int sz = static_cast<int>(p->size);
    lv_obj_set_size(p->obj, sz, sz);
    lv_obj_set_pos(p->obj, static_cast<int>(p->x - p->size * 0.5f),
                   static_cast<int>(p->y - p->size * 0.5f));

    const uint32_t color = (ChargeRand() & 1u) ? kChargeBlueBright : kChargeBlueSoft;
    lv_obj_set_style_bg_color(p->obj, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(p->obj, LV_OPA_COVER, LV_PART_MAIN);
}

void StopChargeEffect() {
    if (s_ui.charge_tick_timer != nullptr) {
        lv_timer_delete(s_ui.charge_tick_timer);
        s_ui.charge_tick_timer = nullptr;
    }
    if (s_ui.charge_root != nullptr) {
        lv_obj_delete(s_ui.charge_root);
    }
    s_ui.charge_root = nullptr;
    s_ui.charge_tip = nullptr;
    for (Particle& p : s_ui.particles) {
        p = Particle{};
    }
    s_ui.charge_playing = false;
}

void UpdateChargeTip(int battery_level) {
    if (s_ui.charge_tip == nullptr) {
        return;
    }
    char tip_buf[32];
    if (battery_level >= 0 && battery_level <= 100) {
        std::snprintf(tip_buf, sizeof(tip_buf), I18n::T("充电中  %d%%"),
                      battery_level);
    } else {
        std::snprintf(tip_buf, sizeof(tip_buf), I18n::T("充电中"));
    }
    lv_label_set_text(s_ui.charge_tip, tip_buf);
}

void OnParticleTick(lv_timer_t* /*timer*/) {
    if (!s_ui.charge_playing || s_ui.charge_root == nullptr) {
        return;
    }

    for (Particle& p : s_ui.particles) {
        if (p.obj == nullptr) {
            continue;
        }

        p.x += p.vx;
        p.y += p.vy;
        // 轻微横向扩散，向上加速感。
        p.vx *= 0.992f;
        p.vy -= 0.035f;
        p.life -= p.life_decay;

        if (p.life <= 0.0f || p.y < -20.0f) {
            RespawnParticle(&p, true);
            continue;
        }

        const int sz_raw = static_cast<int>(p.size * (0.55f + 0.45f * p.life));
        const int sz = sz_raw < 2 ? 2 : sz_raw;
        int opa_i = static_cast<int>(p.life * 220.0f);
        if (opa_i < 0) {
            opa_i = 0;
        }
        if (opa_i > 220) {
            opa_i = 220;
        }
        lv_obj_set_size(p.obj, sz, sz);
        lv_obj_set_pos(p.obj, static_cast<int>(p.x - sz * 0.5f),
                       static_cast<int>(p.y - sz * 0.5f));
        lv_obj_set_style_bg_opa(p.obj, static_cast<lv_opa_t>(opa_i),
                                LV_PART_MAIN);
    }
}

void StartChargeEffect(int battery_level) {
    if (s_ui.screen == nullptr || s_ui.charge_playing) {
        return;
    }
    s_ui.charge_playing = true;
    s_ui.charge_rng = static_cast<uint32_t>(esp_log_timestamp()) | 1u;

    lv_obj_t* root = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, kChargeFxW, kChargeFxH);
    lv_obj_align(root, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(root, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(root, true, LV_PART_MAIN);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_CLICKABLE);
    s_ui.charge_root = root;

    for (int i = 0; i < kParticleCount; ++i) {
        Particle& p = s_ui.particles[i];
        p.obj = lv_obj_create(root);
        lv_obj_remove_style_all(p.obj);
        lv_obj_set_style_radius(p.obj, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_border_width(p.obj, 0, LV_PART_MAIN);
        lv_obj_remove_flag(p.obj, LV_OBJ_FLAG_CLICKABLE);
        RespawnParticle(&p, true);
        // 错开初始高度，看起来像在向上蔓延。
        p.y = kChargeFxH - 20.0f - ChargeRand01() * (kChargeFxH * 0.75f);
        p.life = ChargeRand01();
        lv_obj_set_pos(p.obj, static_cast<int>(p.x - p.size * 0.5f),
                       static_cast<int>(p.y - p.size * 0.5f));
    }

    lv_obj_t* tip = lv_label_create(root);
    s_ui.charge_tip = tip;
    UpdateChargeTip(battery_level);
    lv_obj_set_style_text_color(tip, lv_color_hex(kChargeBlueSoft),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(tip, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(tip, LV_ALIGN_BOTTOM_MID, 0, kRoundSmall ? -28 : -52);
    lv_obj_remove_flag(tip, LV_OBJ_FLAG_CLICKABLE);

    s_ui.charge_tick_timer =
        lv_timer_create(OnParticleTick, kParticleTickMs, nullptr);

    ESP_LOGI(TAG, "charge effect start (%d%%), until unplug", battery_level);
}

void SyncChargeEffect(bool charging, int battery_level) {
    if (!s_ui.charge_primed) {
        s_ui.last_charging = charging;
        s_ui.charge_primed = true;
    }

    if (charging) {
        if (!s_ui.charge_playing) {
            StartChargeEffect(battery_level);
        } else {
            UpdateChargeTip(battery_level);
        }
    } else if (s_ui.charge_playing) {
        ESP_LOGI(TAG, "charge unplugged, stop effect");
        StopChargeEffect();
    }
    s_ui.last_charging = charging;
}

void UpdateClockLabels() {
    time_t now = time(nullptr);
    struct tm tm_info = {};
    const bool have_time =
        localtime_r(&now, &tm_info) != nullptr && tm_info.tm_year >= 2025 - 1900;

    char digits[7] = "000000";
    if (have_time) {
        if (kDigitCount == 4) {
            std::snprintf(digits, sizeof(digits), "%02d%02d", tm_info.tm_hour,
                          tm_info.tm_min);
        } else {
            std::snprintf(digits, sizeof(digits), "%02d%02d%02d", tm_info.tm_hour,
                          tm_info.tm_min, tm_info.tm_sec);
        }
    }

    char date_str[64];
    FormatDateText(date_str, sizeof(date_str), tm_info, have_time);
    if (s_ui.date_lbl != nullptr) {
        lv_label_set_text(s_ui.date_lbl, date_str);
    }

    if (s_ui.clock_row != nullptr) {
        const bool animate = s_ui.clock_primed;
        for (int i = 0; i < kDigitCount; ++i) {
            FlipDigitSet(&s_ui.digits[i], digits[i], animate);
        }
        s_ui.clock_primed = true;
    }

    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    if (Board::GetInstance().GetBatteryLevel(battery_level, charging,
                                             discharging)) {
        if (battery_level < 0) {
            battery_level = 0;
        }
        if (battery_level > 100) {
            battery_level = 100;
        }
        SyncChargeEffect(charging, battery_level);
    }

    ApplyFaceVisibility();
}

void OnClockTimer(lv_timer_t* /*timer*/) {
    UpdateClockLabels();
    s_ui.weather_ticks++;
    const uint32_t interval =
        s_ui.weather_ok ? kWeatherRefreshOkSec : kWeatherRefreshFailSec;
    if (s_ui.weather_ticks > 0 && (s_ui.weather_ticks % interval) == 0) {
        TriggerWeatherFetch();
    }
}

void ScheduleInitialWeatherFetch() {
    // 每 3 秒检查一次：
    //  1) 如果 application.cc 的预拉取已把天气缓存到 WeatherService，直接上屏
    //  2) 否则尝试 TriggerWeatherFetch() 自行拉取
    // 最多重试 40 次（2 分钟），成功后停止。
    struct RetryCtx { int remaining; };
    auto* ctx = new RetryCtx{240};  // 500ms × 240 ≈ 2 分钟
    lv_timer_t* timer = lv_timer_create(
        [](lv_timer_t* timer) {
            auto* c = static_cast<RetryCtx*>(lv_timer_get_user_data(timer));

            // 优先检查缓存：application.cc 的预拉取可能已完成
            if (!s_ui.weather_ok && s_ui.screen != nullptr) {
                const auto& cached = WeatherService::Instance().DeviceCached();
                if (cached.valid) {
                    ApplyWeatherData(cached, true);
                    delete c;
                    lv_timer_delete(timer);
                    return;
                }
            }

            if (s_ui.weather_ok) {
                delete c;
                lv_timer_delete(timer);
                return;
            }

            if (!s_ui.weather_fetching) {
                TriggerWeatherFetch();
            }
            if (s_ui.weather_fetching || --c->remaining <= 0) {
                delete c;
                lv_timer_delete(timer);
            }
        },
        500, ctx);
}

void OnScreenUnloaded(lv_event_t* /*e*/) {
    IdlePower_Detach(IdlePowerSession::Standby);
    s_weather_session.fetch_add(1, std::memory_order_relaxed);
    StopChargeEffect();
    if (s_ui.update_timer != nullptr) {
        lv_timer_delete(s_ui.update_timer);
        s_ui.update_timer = nullptr;
    }
    for (int i = 0; i < kDigitCount; ++i) {
        lv_anim_delete(&s_ui.digits[i], nullptr);
    }
    s_ui = UiState{};
}

void OnStandbyPointer(lv_event_t* e) {
    lv_indev_t* indev = lv_indev_active();
    if (indev == nullptr) {
        return;
    }
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    const lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        s_ui.press_x = point.x;
        s_ui.press_y = point.y;
        s_ui.press_tracking = true;
        return;
    }
    if (code != LV_EVENT_RELEASED || !s_ui.press_tracking) {
        return;
    }
    s_ui.press_tracking = false;
    const int dx = point.x - s_ui.press_x;
    const int dy = point.y - s_ui.press_y;
    const int adx = dx < 0 ? -dx : dx;
    const int ady = dy < 0 ? -dy : dy;

    if (adx > kSwipeFaceThreshold && adx > ady) {
        lv_indev_wait_release(indev);
        if (dx < 0) {
            SwitchStandbyFace(StandbyFace::Clock);
        } else {
            SwitchStandbyFace(StandbyFace::Weather);
        }
        return;
    }
    if (adx < kTapSlop && ady < kTapSlop) {
        StandbyScreen::ReturnHome();
    }
}

void standby_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("standby", event);
    StandbyScreen::LifecycleCallback(event);
}

}  // namespace

lv_obj_t* StandbyScreen::Create() {
    lv_obj_t* screen = lv_obj_create(NULL);
    lv_obj_set_size(screen, kPanelSize, kPanelSize);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(screen, OnStandbyPointer, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(screen, OnStandbyPointer, LV_EVENT_RELEASED, nullptr);
    s_ui.screen = screen;
    s_ui.face = LoadPreferredFace();

    s_ui.weather_panel = CreateWeatherPanel(screen);

    lv_obj_t* box = lv_obj_create(screen);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_row(box, kRoundSmall ? 18 : 28, LV_PART_MAIN);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    s_ui.clock_panel = box;

    s_ui.clock_row = CreateFlipClockRow(box);

    s_ui.date_lbl = lv_label_create(box);
    lv_label_set_text(s_ui.date_lbl,
                      kRoundSmall ? I18n::T("--/-- 周-")
                                  : I18n::T("----年--月--日 星期-"));
    lv_obj_set_style_text_color(s_ui.date_lbl, lv_color_hex(0xE5E7EB),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.date_lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_ui.date_lbl, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    if (kRoundSmall) {
        lv_obj_set_width(s_ui.date_lbl, kPanelSize - 80);
        lv_label_set_long_mode(s_ui.date_lbl, LV_LABEL_LONG_CLIP);
    }
    lv_obj_remove_flag(s_ui.date_lbl, LV_OBJ_FLAG_CLICKABLE);

    s_ui.activation_title_lbl = lv_label_create(screen);
    lv_label_set_text(s_ui.activation_title_lbl, I18n::T("请绑定设备"));
    lv_obj_set_style_text_color(s_ui.activation_title_lbl, lv_color_hex(0xFFFFFF),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.activation_title_lbl, &font_puhui_20_4,
                               LV_PART_MAIN);
    lv_obj_set_style_text_align(s_ui.activation_title_lbl, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_align(s_ui.activation_title_lbl, LV_ALIGN_CENTER, 0,
                 kRoundSmall ? -28 : -40);
    lv_obj_add_flag(s_ui.activation_title_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_ui.activation_title_lbl, LV_OBJ_FLAG_CLICKABLE);

    s_ui.activation_code_lbl = lv_label_create(screen);
    lv_label_set_text(s_ui.activation_code_lbl, "");
    lv_obj_set_style_text_color(s_ui.activation_code_lbl, lv_color_hex(0xFBBF24),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.activation_code_lbl, &font_puhui_30_4,
                               LV_PART_MAIN);
    lv_obj_set_style_text_align(s_ui.activation_code_lbl, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_align(s_ui.activation_code_lbl, LV_ALIGN_CENTER, 0,
                 kRoundSmall ? 8 : 12);
    lv_obj_add_flag(s_ui.activation_code_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_ui.activation_code_lbl, LV_OBJ_FLAG_CLICKABLE);

    s_ui.face_dots = CreateFaceDots(screen);
    screen_make_input_passive(s_ui.weather_panel);
    screen_make_input_passive(s_ui.clock_panel);
    screen_make_input_passive(s_ui.face_dots);

    UpdateClockLabels();
    ApplyFaceVisibility();
    if (WeatherService::Instance().DeviceCached().valid) {
        ApplyWeatherData(WeatherService::Instance().DeviceCached(), true);
    }
    ScheduleInitialWeatherFetch();
    s_ui.update_timer = lv_timer_create(OnClockTimer, 1000, nullptr);

    lv_obj_add_event_cb(screen, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);
    return screen;
}

void StandbyScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: standby_screen");
        // reset_activity=false：自动待机保留累计；手动进入时 Prepare 未置位也会
        // 在 Attach 里因 keep=false 且 reset=false 保留旧 tick——手动路径应重置。
        IdlePower_Attach(IdlePowerSession::Standby, /*reset_activity=*/true);
    } else {
        ESP_LOGI(TAG, "unload: standby_screen");
    }
}

void StandbyScreen::Show() {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = StandbyScreen::Create();
    screen_attach_lifecycle(app, standby_lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void StandbyScreen::ReturnHome() {
    IdlePower_NotifyActivity();
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* home = HomeScreen::Create();
    lv_screen_load(home);
    if (old_scr != nullptr && old_scr != home) {
        lv_obj_delete_async(old_scr);
    }
}

bool StandbyScreen::IsActive() {
    return s_ui.screen != nullptr;
}

StandbyFace StandbyScreen::GetPreferredFace() { return LoadPreferredFace(); }

void StandbyScreen::SetPreferredFace(StandbyFace face) {
    SavePreferredFace(face);
    if (s_ui.screen != nullptr) {
        SwitchStandbyFace(face);
    }
}
