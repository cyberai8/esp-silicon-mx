#include "clock_screen.h"

#include "application.h"
#include "assets/lang_config.h"
#include "config.h"
#include "home_screen/home_screen.h"
#include "i18n.h"
#include "screen_util.h"
#include "settings.h"

#include <cstdio>
#include <atomic>
#include <cstring>
#include <ctime>
#include <mutex>
#include <new>
#include <string>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char* TAG = "ClockScreen";

#if defined(BOARD_ESP_VOCAT) || (DISPLAY_WIDTH == 360 && DISPLAY_HEIGHT == 360)
constexpr bool kRound = true;
constexpr int kPanel = DISPLAY_WIDTH;
// 圆屏内切圆安全区：返回键贴在 (28,28) 会被裁掉，内收到 (78,56)。
constexpr int32_t kBackBtnSize = 40;
constexpr int32_t kBackBtnX = 78;
constexpr int32_t kBackBtnY = 56;
// 底部 tab 原先 -18 会贴边裁字；上移后可用宽度够 270。
// 圆心 y=180，tab 中心约 278（ofs=-60,h=44）时半宽≈148，270 安全。
constexpr int32_t kTabBarW = 270;
constexpr int32_t kTabBarH = 44;
constexpr int32_t kTabBarBottomOfs = -60;
constexpr int32_t kAddBtnBottomOfs = -112;
constexpr int32_t kOverlayBtnBottomOfs = -56;
constexpr int32_t kAlarmListH = 145;
constexpr int32_t kSwBtnCenterY = 12;
constexpr int32_t kCdPresetY = 120;
constexpr int32_t kCdCustomY = 158;
#else
constexpr bool kRound = false;
constexpr int kPanel = 720;
constexpr int32_t kBackBtnSize = 44;
constexpr int32_t kBackBtnX = 16;
constexpr int32_t kBackBtnY = 16;
constexpr int32_t kTabBarW = 480;
constexpr int32_t kTabBarH = 64;
constexpr int32_t kTabBarBottomOfs = -24;
constexpr int32_t kAddBtnBottomOfs = -100;
constexpr int32_t kOverlayBtnBottomOfs = -36;
constexpr int32_t kAlarmListH = 360;
constexpr int32_t kSwBtnCenterY = 50;
constexpr int32_t kCdPresetY = 220;
constexpr int32_t kCdCustomY = 300;
#endif

constexpr uint32_t kBg = 0x081C1C;
constexpr uint32_t kAccent = 0x3DD6C6;
constexpr uint32_t kText = 0xFFFFFF;
constexpr uint32_t kMuted = 0x8A9A9A;
constexpr uint32_t kBtnDark = 0x1A2E2E;
constexpr uint32_t kRowBg = 0x142828;

enum class Tab : int { kAlarm = 0, kStopwatch = 1, kCountdown = 2 };

constexpr int kMaxAlarms = 5;

struct AlarmData {
    bool enabled = false;
    bool once = true;          // true=单次, false=重复
    uint8_t hour = 7;
    uint8_t minute = 30;
    uint8_t weekdays = 0x1F;   // bit0=周一 ... bit6=周日
    uint8_t ringtone = 0;
};

struct Ringtone {
    const char* name;
    const std::string_view* sound;
};

const Ringtone kRingtones[] = {
    {"默认铃声", &Lang::Sounds::OGG_ALARM_DEFAULT},
    {"轻柔", &Lang::Sounds::OGG_ALARM_GENTLE},
    {"鸟鸣", &Lang::Sounds::OGG_ALARM_BIRD},
    {"电子", &Lang::Sounds::OGG_ALARM_ELECTRONIC},
};
constexpr int kRingtoneCount = 4;
constexpr int kAlarmRingRepeatCount = 3;
constexpr uint32_t kAlarmRingRepeatGapMs = 1800;

AlarmData s_alarms[kMaxAlarms];
int s_alarm_count = 0;
int s_edit_index = -1;  // -1=新建，>=0=编辑列表项
Tab s_tab = Tab::kAlarm;

lv_obj_t* s_scr = nullptr;
lv_obj_t* s_page_alarm = nullptr;
lv_obj_t* s_page_sw = nullptr;
lv_obj_t* s_page_cd = nullptr;
lv_obj_t* s_tab_btns[3] = {};
lv_obj_t* s_overlay = nullptr;
lv_obj_t* s_overlay_back = nullptr;
lv_obj_t* s_overlay_title = nullptr;

// alarm list page
lv_obj_t* s_alarm_list = nullptr;
lv_obj_t* s_alarm_empty_lbl = nullptr;

// stopwatch
lv_obj_t* s_sw_lbl = nullptr;
lv_obj_t* s_sw_start_btn = nullptr;
int64_t s_sw_elapsed_ms = 0;
int64_t s_sw_last_tick = 0;
bool s_sw_running = false;

// countdown
lv_obj_t* s_cd_lbl = nullptr;
lv_obj_t* s_cd_start_btn = nullptr;
int s_cd_total_sec = 5 * 60;
int s_cd_left_sec = 5 * 60;
bool s_cd_running = false;
int64_t s_cd_deadline_ms = 0;

// 正计时和倒计时独立于页面运行；页面退出时只清理控件，不清理这些状态。
std::mutex s_runtime_mutex;
esp_timer_handle_t s_runtime_timer = nullptr;
bool s_runtime_timer_active = false;
std::atomic_bool s_clock_screen_loaded{false};
std::atomic_bool s_runtime_ui_update_pending{false};

// edit rollers
lv_obj_t* s_edit_hour = nullptr;
lv_obj_t* s_edit_min = nullptr;
lv_obj_t* s_edit_once_btn = nullptr;
lv_obj_t* s_edit_repeat_btn = nullptr;
lv_obj_t* s_edit_day_btns[7] = {};
lv_obj_t* s_edit_days_row = nullptr;
lv_obj_t* s_edit_once_hint = nullptr;
lv_obj_t* s_edit_ring_lbl = nullptr;
bool s_edit_once = true;
uint8_t s_edit_weekdays = 0x1F;
uint8_t s_edit_ringtone = 0;
bool s_ringtone_return_edit = false;
bool s_resume_draft = false;
AlarmData s_draft;

// custom countdown rollers
lv_obj_t* s_cd_h = nullptr;
lv_obj_t* s_cd_m = nullptr;
lv_obj_t* s_cd_s = nullptr;

esp_timer_handle_t s_alarm_poll = nullptr;
int s_last_fired_minute = -1;
std::atomic_bool s_alarm_ring_running{false};

void ShowTab(Tab tab);
void RefreshAlarmPage();
void RequestAlarmPageRefresh();
void HideOverlay();
void ShowAlarmEdit(int index);
void ShowRingtonePicker();
void ShowCountdownCustom();
void SaveAlarmFromEdit();
void DeleteEditingAlarm();
void LoadAlarms();
void PersistAlarms();
void EnsureAlarmPoller();
void StopAlarmPoller();
bool AnyAlarmEnabled();
std::string AlarmHint(const AlarmData& a);
void ShowAlarmLimitPopup();
void CloseAlarmLimitPopup();
void MaybeStopRuntimeTimer();

lv_obj_t* s_limit_popup = nullptr;

void GoHome() {
    lv_obj_t* old = lv_screen_active();
    lv_obj_t* home = HomeScreen::Create();
    lv_screen_load(home);
    if (old != nullptr && old != home) {
        lv_obj_delete_async(old);
    }
}

void OnSwipeBack() {
    if (s_limit_popup != nullptr) {
        CloseAlarmLimitPopup();
        return;
    }
    if (s_overlay != nullptr && !lv_obj_has_flag(s_overlay, LV_OBJ_FLAG_HIDDEN)) {
        HideOverlay();
        return;
    }
    GoHome();
}

const lv_font_t* FontBig() {
    return kRound ? &font_puhui_30_4 : &font_puhui_30_4;
}
const lv_font_t* FontSmall() {
    return &font_puhui_20_4;
}

void StyleScreen(lv_obj_t* scr) {
    lv_obj_set_style_bg_color(scr, lv_color_hex(kBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(scr, LV_DIR_NONE);
}

void StyleGhostBtn(lv_obj_t* btn, int size) {
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_hex(kAccent), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
}

void StyleFillBtn(lv_obj_t* btn, int size) {
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(kAccent), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
}

lv_obj_t* MakeLabel(lv_obj_t* parent, const char* text, uint32_t color,
                    const lv_font_t* font) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, font, LV_PART_MAIN);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return lbl;
}

void CloseAlarmLimitPopup() {
    if (s_limit_popup == nullptr) return;
    lv_obj_delete(s_limit_popup);
    s_limit_popup = nullptr;
}

void ShowAlarmLimitPopup() {
    if (s_scr == nullptr) return;
    CloseAlarmLimitPopup();

    s_limit_popup = lv_obj_create(s_scr);
    screen_strip_obj_chrome(s_limit_popup);
    lv_obj_set_size(s_limit_popup, kPanel, kPanel);
    lv_obj_set_style_bg_color(s_limit_popup, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_limit_popup, LV_OPA_70, LV_PART_MAIN);
    lv_obj_add_flag(s_limit_popup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(s_limit_popup);

    lv_obj_t* card = lv_obj_create(s_limit_popup);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, kRound ? 240 : 360, kRound ? 150 : 200);
    lv_obj_center(card);
    lv_obj_set_style_radius(card, 18, LV_PART_MAIN);
    lv_obj_set_style_bg_color(card, lv_color_hex(kBtnDark), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(kAccent), LV_PART_MAIN);
    lv_obj_set_scroll_dir(card, LV_DIR_NONE);

    char msg[48];
    snprintf(msg, sizeof(msg), I18n::T("最多添加 %d 个闹钟"), kMaxAlarms);
    lv_obj_t* title = MakeLabel(card, msg, kText, FontSmall());
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(title, kRound ? 200 : 300);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, kRound ? 28 : 40);

    lv_obj_t* ok = lv_btn_create(card);
    lv_obj_set_size(ok, kRound ? 100 : 140, kRound ? 36 : 48);
    lv_obj_align(ok, LV_ALIGN_BOTTOM_MID, 0, kRound ? -18 : -24);
    lv_obj_set_style_radius(ok, 18, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ok, lv_color_hex(kAccent), LV_PART_MAIN);
    lv_obj_set_style_border_width(ok, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(ok, 0, LV_PART_MAIN);
    lv_obj_t* ol = lv_label_create(ok);
    lv_label_set_text(ol, I18n::T("知道了"));
    lv_obj_set_style_text_font(ol, FontSmall(), LV_PART_MAIN);
    lv_obj_set_style_text_color(ol, lv_color_hex(kBg), LV_PART_MAIN);
    lv_obj_center(ol);
    lv_obj_add_event_cb(ok, [](lv_event_t*) { CloseAlarmLimitPopup(); },
                        LV_EVENT_CLICKED, nullptr);
}

std::string AlarmHint(const AlarmData& a) {
    if (!a.enabled) return I18n::T("已关闭");
    if (a.once) return I18n::T("单次");
    static const char* kDays[] = {"一", "二", "三", "四", "五", "六", "日"};
    if (a.weekdays == 0x1F) return I18n::T("工作日");
    if (a.weekdays == 0x7F) return I18n::T("每天");
    if (a.weekdays == 0x60) return I18n::T("周末");
    std::string out;
    for (int i = 0; i < 7; ++i) {
        if (a.weekdays & (1u << i)) {
            if (!out.empty()) out += " ";
            out += I18n::T(kDays[i]);
        }
    }
    if (out.empty()) return I18n::T("未选重复日");
    return out;
}

uint32_t PackAlarm(const AlarmData& a) {
    return (a.enabled ? 1u : 0u) | ((a.once ? 1u : 0u) << 1) |
           (static_cast<uint32_t>(a.hour) << 2) |
           (static_cast<uint32_t>(a.minute) << 8) |
           (static_cast<uint32_t>(a.weekdays) << 16) |
           (static_cast<uint32_t>(a.ringtone) << 24);
}

AlarmData UnpackAlarm(uint32_t v) {
    AlarmData a;
    a.enabled = (v & 1u) != 0;
    a.once = ((v >> 1) & 1u) != 0;
    a.hour = static_cast<uint8_t>((v >> 2) & 0x3Fu);
    a.minute = static_cast<uint8_t>((v >> 8) & 0x3Fu);
    a.weekdays = static_cast<uint8_t>((v >> 16) & 0x7Fu);
    a.ringtone = static_cast<uint8_t>((v >> 24) & 0x0Fu);
    if (a.hour > 23) a.hour = 7;
    if (a.minute > 59) a.minute = 30;
    if (a.ringtone >= kRingtoneCount) a.ringtone = 0;
    if (!a.once && a.weekdays == 0) a.weekdays = 0x1F;
    return a;
}

void LoadAlarms() {
    Settings settings("clock", false);
    s_alarm_count = settings.GetInt("cnt", -1);
    if (s_alarm_count < 0) {
        // 兼容旧版单闹钟字段
        s_alarm_count = 0;
        if (settings.GetInt("hh", -1) >= 0) {
            AlarmData a;
            a.enabled = settings.GetBool("en", false);
            a.once = settings.GetBool("once", true);
            a.hour = static_cast<uint8_t>(settings.GetInt("hh", 7));
            a.minute = static_cast<uint8_t>(settings.GetInt("mm", 30));
            a.weekdays = static_cast<uint8_t>(settings.GetInt("wd", 0x1F));
            a.ringtone = static_cast<uint8_t>(settings.GetInt("ring", 0));
            if (a.hour > 23) a.hour = 7;
            if (a.minute > 59) a.minute = 30;
            if (a.ringtone >= kRingtoneCount) a.ringtone = 0;
            s_alarms[0] = a;
            s_alarm_count = 1;
        }
        return;
    }
    if (s_alarm_count > kMaxAlarms) s_alarm_count = kMaxAlarms;
    for (int i = 0; i < s_alarm_count; ++i) {
        char key[8];
        snprintf(key, sizeof(key), "a%d", i);
        s_alarms[i] = UnpackAlarm(static_cast<uint32_t>(settings.GetInt(key, 0)));
    }
}

void PersistAlarms() {
    Settings settings("clock", true);
    settings.SetInt("cnt", s_alarm_count);
    for (int i = 0; i < s_alarm_count; ++i) {
        char key[8];
        snprintf(key, sizeof(key), "a%d", i);
        settings.SetInt(key, static_cast<int32_t>(PackAlarm(s_alarms[i])));
    }
}

bool AnyAlarmEnabled() {
    for (int i = 0; i < s_alarm_count; ++i) {
        if (s_alarms[i].enabled) return true;
    }
    return false;
}

void PlayRingtone(uint8_t idx) {
    if (idx >= kRingtoneCount) idx = 0;
    const std::string_view* sound = kRingtones[idx].sound;
    Application::GetInstance().Schedule([sound]() {
        Application::GetInstance().PlaySound(*sound);
    });
}

struct AlarmRingRequest {
    uint8_t ringtone;
};

void AlarmRingTask(void* arg) {
    auto* request = static_cast<AlarmRingRequest*>(arg);
    uint8_t ringtone = request->ringtone;
    delete request;

    for (int i = 0; i < kAlarmRingRepeatCount; ++i) {
        PlayRingtone(ringtone);
        if (i + 1 < kAlarmRingRepeatCount) {
            vTaskDelay(pdMS_TO_TICKS(kAlarmRingRepeatGapMs));
        }
    }

    s_alarm_ring_running.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

void PlayAlarmRingtone(uint8_t idx) {
    if (idx >= kRingtoneCount) idx = 0;
    if (s_alarm_ring_running.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    auto* request = new (std::nothrow) AlarmRingRequest{idx};
    if (request == nullptr) {
        s_alarm_ring_running.store(false, std::memory_order_release);
        PlayRingtone(idx);
        return;
    }

    BaseType_t ret = xTaskCreate(AlarmRingTask, "alarm_ring", 4096, request, 4, nullptr);
    if (ret != pdPASS) {
        delete request;
        s_alarm_ring_running.store(false, std::memory_order_release);
        ESP_LOGW(TAG, "failed to create alarm ring task");
        PlayRingtone(idx);
    }
}

void RefreshAlarmPageAsync(void* /*arg*/) {
    if (s_scr != nullptr && lv_screen_active() == s_scr) {
        RefreshAlarmPage();
    }
}

void RequestAlarmPageRefresh() {
    if (s_scr == nullptr) return;
    screen_async_call(RefreshAlarmPageAsync, nullptr);
}

int LocalWeekdayMon0() {
    time_t now = time(nullptr);
    struct tm t {};
    localtime_r(&now, &t);
    return (t.tm_wday + 6) % 7;
}

int FindFiringAlarmIndex() {
    time_t now = time(nullptr);
    struct tm t {};
    localtime_r(&now, &t);
    int key = t.tm_yday * 24 * 60 + t.tm_hour * 60 + t.tm_min;
    if (key == s_last_fired_minute) return -1;
    int w = LocalWeekdayMon0();
    for (int i = 0; i < s_alarm_count; ++i) {
        const AlarmData& a = s_alarms[i];
        if (!a.enabled) continue;
        if (t.tm_hour != a.hour || t.tm_min != a.minute) continue;
        if (!a.once && (a.weekdays & (1u << w)) == 0) continue;
        s_last_fired_minute = key;
        return i;
    }
    return -1;
}

void OnAlarmFired(int idx) {
    if (idx < 0 || idx >= s_alarm_count) return;
    AlarmData& a = s_alarms[idx];
    ESP_LOGI(TAG, "alarm[%d] fired %02u:%02u", idx, a.hour, a.minute);
    PlayAlarmRingtone(a.ringtone);
    if (a.once) {
        a.enabled = false;
        PersistAlarms();
        RequestAlarmPageRefresh();
        if (!AnyAlarmEnabled()) StopAlarmPoller();
    }
}

void AlarmPollCb(void* /*arg*/) {
    int idx = FindFiringAlarmIndex();
    if (idx >= 0) {
        Application::GetInstance().Schedule([idx]() {
            OnAlarmFired(idx);
        });
    }
}

void EnsureAlarmPoller() {
    if (!AnyAlarmEnabled()) {
        StopAlarmPoller();
        return;
    }
    if (s_alarm_poll != nullptr) return;
    esp_timer_create_args_t args = {
        .callback = &AlarmPollCb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_alarm",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &s_alarm_poll) == ESP_OK) {
        esp_timer_start_periodic(s_alarm_poll, 5 * 1000 * 1000ULL);
    }
}

void StopAlarmPoller() {
    if (s_alarm_poll == nullptr) return;
    esp_timer_stop(s_alarm_poll);
    esp_timer_delete(s_alarm_poll);
    s_alarm_poll = nullptr;
}

void RefreshAlarmPage() {
    if (s_alarm_list == nullptr) return;
    lv_obj_clean(s_alarm_list);

    if (s_alarm_empty_lbl) {
        if (s_alarm_count == 0) {
            lv_obj_remove_flag(s_alarm_empty_lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_alarm_empty_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }

    const int row_h = kRound ? 56 : 72;
    const int row_w = kRound ? 260 : 420;
    for (int i = 0; i < s_alarm_count; ++i) {
        const AlarmData& a = s_alarms[i];
        lv_obj_t* row = lv_btn_create(s_alarm_list);
        lv_obj_set_size(row, row_w, row_h);
        lv_obj_set_style_radius(row, 14, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, lv_color_hex(kRowBg), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);

        char tbuf[8];
        snprintf(tbuf, sizeof(tbuf), "%02u:%02u", a.hour, a.minute);
        lv_obj_t* time_lbl = lv_label_create(row);
        lv_label_set_text(time_lbl, tbuf);
        lv_obj_set_style_text_font(time_lbl, FontBig(), LV_PART_MAIN);
        lv_obj_set_style_text_color(
            time_lbl, lv_color_hex(a.enabled ? kText : kMuted), LV_PART_MAIN);
        lv_obj_align(time_lbl, LV_ALIGN_LEFT_MID, 12, kRound ? -8 : -10);

        lv_obj_t* hint = lv_label_create(row);
        lv_label_set_text(hint, AlarmHint(a).c_str());
        lv_obj_set_style_text_font(hint, FontSmall(), LV_PART_MAIN);
        lv_obj_set_style_text_color(hint, lv_color_hex(kMuted), LV_PART_MAIN);
        lv_obj_align(hint, LV_ALIGN_LEFT_MID, 12, kRound ? 14 : 16);

        lv_obj_t* sw = lv_switch_create(row);
        lv_obj_set_size(sw, kRound ? 40 : 52, kRound ? 22 : 28);
        lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -12, 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(kAccent), LV_PART_INDICATOR);
        if (a.enabled) lv_obj_add_state(sw, LV_STATE_CHECKED);
        else lv_obj_remove_state(sw, LV_STATE_CHECKED);
        screen_swipe_back_ignore(sw, true);
        lv_obj_add_event_cb(
            sw,
            [](lv_event_t* e) {
                int idx = static_cast<int>(
                    reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
                if (idx < 0 || idx >= s_alarm_count) return;
                lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
                s_alarms[idx].enabled = lv_obj_has_state(obj, LV_STATE_CHECKED);
                PersistAlarms();
                EnsureAlarmPoller();
                RefreshAlarmPage();
            },
            LV_EVENT_VALUE_CHANGED,
            reinterpret_cast<void*>(static_cast<intptr_t>(i)));

        // 点行编辑；开关自己消费点击
        lv_obj_add_event_cb(
            row,
            [](lv_event_t* e) {
                lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
                lv_obj_t* row_obj =
                    static_cast<lv_obj_t*>(lv_event_get_current_target(e));
                // 点到开关时不进编辑
                if (target != row_obj &&
                    lv_obj_check_type(target, &lv_switch_class)) {
                    return;
                }
                // 点到开关的子对象也不进
                lv_obj_t* p = target;
                while (p != nullptr && p != row_obj) {
                    if (lv_obj_check_type(p, &lv_switch_class)) return;
                    p = lv_obj_get_parent(p);
                }
                int idx = static_cast<int>(
                    reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
                ShowAlarmEdit(idx);
            },
            LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
    }
}

void SetTabStyle(lv_obj_t* btn, bool on) {
    if (btn == nullptr) return;
    lv_obj_t* lbl = lv_obj_get_child(btn, 0);
    if (on) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(kAccent), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
        if (lbl) {
            lv_obj_set_style_text_color(lbl, lv_color_hex(kBg), LV_PART_MAIN);
        }
    } else {
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
        if (lbl) {
            lv_obj_set_style_text_color(lbl, lv_color_hex(kMuted), LV_PART_MAIN);
        }
    }
}

void ShowTab(Tab tab) {
    s_tab = tab;
    if (s_page_alarm) {
        if (tab == Tab::kAlarm) lv_obj_remove_flag(s_page_alarm, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_page_alarm, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_page_sw) {
        if (tab == Tab::kStopwatch) lv_obj_remove_flag(s_page_sw, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_page_sw, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_page_cd) {
        if (tab == Tab::kCountdown) lv_obj_remove_flag(s_page_cd, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_page_cd, LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < 3; ++i) {
        SetTabStyle(s_tab_btns[i], static_cast<int>(tab) == i);
    }
}

void OnTabClicked(lv_event_t* e) {
    auto tab = static_cast<Tab>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    HideOverlay();
    ShowTab(tab);
}

// ---------- stopwatch ----------
void RefreshStopwatchLabel() {
    if (s_sw_lbl == nullptr) return;
    int64_t elapsed_ms = 0;
    {
        std::lock_guard<std::mutex> lock(s_runtime_mutex);
        elapsed_ms = s_sw_elapsed_ms;
    }
    int ms = static_cast<int>(elapsed_ms % 1000);
    int total = static_cast<int>(elapsed_ms / 1000);
    int sec = total % 60;
    int min = (total / 60) % 60;
    int hour = total / 3600;
    char buf[32];
    if (hour > 0) {
        snprintf(buf, sizeof(buf), "%d:%02d:%02d", hour, min, sec);
    } else {
        snprintf(buf, sizeof(buf), "%02d:%02d.%02d", min, sec, ms / 10);
    }
    lv_label_set_text(s_sw_lbl, buf);
}

void RefreshCountdownLabel() {
    if (s_cd_lbl == nullptr) return;
    int left_sec = 0;
    {
        std::lock_guard<std::mutex> lock(s_runtime_mutex);
        left_sec = s_cd_left_sec;
    }
    int sec = left_sec;
    int h = sec / 3600;
    int m = (sec % 3600) / 60;
    int s = sec % 60;
    char buf[32];
    if (h > 0) snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
    else snprintf(buf, sizeof(buf), "%02d:%02d", m, s);
    lv_label_set_text(s_cd_lbl, buf);
}

void UpdateRuntimeButtonLabels() {
    bool sw_running = false;
    bool cd_running = false;
    {
        std::lock_guard<std::mutex> lock(s_runtime_mutex);
        sw_running = s_sw_running;
        cd_running = s_cd_running;
    }
    if (s_sw_start_btn != nullptr) {
        lv_obj_t* lbl = lv_obj_get_child(s_sw_start_btn, 0);
        if (lbl) lv_label_set_text(lbl, I18n::T(sw_running ? "暂停" : "开始"));
    }
    if (s_cd_start_btn != nullptr) {
        lv_obj_t* lbl = lv_obj_get_child(s_cd_start_btn, 0);
        if (lbl) lv_label_set_text(lbl, I18n::T(cd_running ? "暂停" : "开始"));
    }
}

void RefreshRuntimeUiAsync(void* /*arg*/) {
    s_runtime_ui_update_pending.store(false, std::memory_order_release);
    if (!s_clock_screen_loaded.load(std::memory_order_acquire)) return;
    RefreshStopwatchLabel();
    RefreshCountdownLabel();
    UpdateRuntimeButtonLabels();
}

void RequestRuntimeUiRefresh() {
    if (!s_clock_screen_loaded.load(std::memory_order_acquire)) return;
    if (s_runtime_ui_update_pending.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (screen_async_call(RefreshRuntimeUiAsync, nullptr) != LV_RESULT_OK) {
        s_runtime_ui_update_pending.store(false, std::memory_order_release);
    }
}

void RuntimeTimerCb(void* /*arg*/) {
    const int64_t now_ms = esp_timer_get_time() / 1000;
    bool refresh_ui = false;
    bool countdown_finished = false;
    {
        std::lock_guard<std::mutex> lock(s_runtime_mutex);

        if (s_sw_running) {
            if (s_sw_last_tick <= 0) s_sw_last_tick = now_ms;
            const int64_t delta_ms = now_ms - s_sw_last_tick;
            if (delta_ms > 0) {
                s_sw_elapsed_ms += delta_ms;
                s_sw_last_tick = now_ms;
                refresh_ui = true;
            }
        }

        if (s_cd_running) {
            const int64_t remain_ms = s_cd_deadline_ms - now_ms;
            const int remain_sec = remain_ms > 0
                                       ? static_cast<int>((remain_ms + 999) / 1000)
                                       : 0;
            if (remain_sec != s_cd_left_sec) {
                s_cd_left_sec = remain_sec;
                refresh_ui = true;
            }
            if (remain_sec <= 0) {
                s_cd_running = false;
                s_cd_deadline_ms = 0;
                countdown_finished = true;
                refresh_ui = true;
            }
        }
    }

    if (countdown_finished) {
        PlayRingtone(0);
        MaybeStopRuntimeTimer();
    }
    if (refresh_ui) RequestRuntimeUiRefresh();
}

void EnsureRuntimeTimer() {
    std::lock_guard<std::mutex> lock(s_runtime_mutex);
    if (s_runtime_timer == nullptr) {
        esp_timer_create_args_t args = {
            .callback = &RuntimeTimerCb,
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "clock_runtime",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &s_runtime_timer) != ESP_OK) {
            ESP_LOGE(TAG, "failed to create runtime timer");
            return;
        }
    }
    if (!s_runtime_timer_active) {
        if (esp_timer_start_periodic(s_runtime_timer, 50 * 1000) == ESP_OK) {
            s_runtime_timer_active = true;
        } else {
            ESP_LOGE(TAG, "failed to start runtime timer");
        }
    }
}

void MaybeStopRuntimeTimer() {
    std::lock_guard<std::mutex> lock(s_runtime_mutex);
    if (s_runtime_timer == nullptr || !s_runtime_timer_active ||
        s_sw_running || s_cd_running) {
        return;
    }
    esp_timer_stop(s_runtime_timer);
    s_runtime_timer_active = false;
}

void OnSwStart(lv_event_t* e) {
    lv_obj_t* btn = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
    bool running = false;
    {
        std::lock_guard<std::mutex> lock(s_runtime_mutex);
        if (!s_sw_running) {
            s_sw_running = true;
            s_sw_last_tick = esp_timer_get_time() / 1000;
            running = true;
        } else {
            s_sw_running = false;
        }
    }
    if (running) EnsureRuntimeTimer();
    else MaybeStopRuntimeTimer();
    if (btn) {
        lv_obj_t* lbl = lv_obj_get_child(btn, 0);
        if (lbl) lv_label_set_text(lbl, I18n::T(running ? "暂停" : "开始"));
    }
}

void OnSwReset(lv_event_t* /*e*/) {
    {
        std::lock_guard<std::mutex> lock(s_runtime_mutex);
        s_sw_running = false;
        s_sw_elapsed_ms = 0;
        s_sw_last_tick = 0;
    }
    MaybeStopRuntimeTimer();
    RefreshStopwatchLabel();
    UpdateRuntimeButtonLabels();
}

void OnCdStart(lv_event_t* /*e*/) {
    bool running = false;
    {
        std::lock_guard<std::mutex> lock(s_runtime_mutex);
        if (!s_cd_running) {
            if (s_cd_left_sec <= 0) s_cd_left_sec = s_cd_total_sec;
            s_cd_deadline_ms = esp_timer_get_time() / 1000 +
                               static_cast<int64_t>(s_cd_left_sec) * 1000;
            s_cd_running = true;
            running = true;
        } else {
            const int64_t remain_ms = s_cd_deadline_ms - esp_timer_get_time() / 1000;
            s_cd_left_sec = remain_ms > 0
                                ? static_cast<int>((remain_ms + 999) / 1000)
                                : 0;
            s_cd_deadline_ms = 0;
            s_cd_running = false;
        }
    }
    if (running) EnsureRuntimeTimer();
    else MaybeStopRuntimeTimer();
    RefreshCountdownLabel();
    UpdateRuntimeButtonLabels();
}

void SetCountdownSeconds(int sec) {
    if (sec < 1) sec = 1;
    if (sec > 23 * 3600 + 59 * 60 + 59) sec = 23 * 3600 + 59 * 60 + 59;
    {
        std::lock_guard<std::mutex> lock(s_runtime_mutex);
        s_cd_running = false;
        s_cd_deadline_ms = 0;
        s_cd_total_sec = sec;
        s_cd_left_sec = sec;
    }
    MaybeStopRuntimeTimer();
    RefreshCountdownLabel();
    UpdateRuntimeButtonLabels();
}

void OnPreset(lv_event_t* e) {
    int sec = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    SetCountdownSeconds(sec);
}

// ---------- overlay helpers ----------
void ClearOverlay() {
    if (s_overlay == nullptr) return;
    lv_obj_clean(s_overlay);
}

void HideOverlay() {
    if (s_overlay == nullptr) return;
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    ClearOverlay();
}

lv_obj_t* MakeOverlayBackButton(lv_obj_t* parent) {
    lv_obj_t* back = lv_button_create(parent);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, kBackBtnSize, kBackBtnSize);
    if (kRound) {
        lv_obj_set_style_bg_color(back, lv_color_hex(kRowBg), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(back, LV_OPA_70, LV_PART_MAIN);
        lv_obj_set_style_border_width(back, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(back, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_border_opa(back, LV_OPA_30, LV_PART_MAIN);
    } else {
        lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(back, 0, LV_PART_MAIN);
    }
    lv_obj_set_style_bg_color(back, lv_color_hex(0xFFFFFF),
                              static_cast<lv_style_selector_t>(LV_PART_MAIN |
                                                               LV_STATE_PRESSED));
    lv_obj_set_style_bg_opa(back, LV_OPA_30,
                            static_cast<lv_style_selector_t>(LV_PART_MAIN |
                                                             LV_STATE_PRESSED));
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, kBackBtnX, kBackBtnY);
    lv_obj_set_ext_click_area(back, 12);
    screen_swipe_back_ignore(back, true);

    if (kRound) {
        lv_obj_t* icon = lv_image_create(back);
        lv_image_set_src(icon, "A:ic_app_back.spng");
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(icon);
    } else {
        lv_obj_t* bl = lv_label_create(back);
        lv_label_set_text(bl, "<");
        lv_obj_set_style_text_font(bl, FontBig(), LV_PART_MAIN);
        lv_obj_set_style_text_color(bl, lv_color_hex(kText), LV_PART_MAIN);
        lv_obj_center(bl);
    }

    lv_obj_add_event_cb(back, [](lv_event_t*) { HideOverlay(); }, LV_EVENT_CLICKED,
                      nullptr);
    s_overlay_back = back;
    return back;
}

void RaiseOverlayChrome() {
    if (s_overlay_title != nullptr) {
        lv_obj_move_foreground(s_overlay_title);
    }
    if (s_overlay_back != nullptr) {
        lv_obj_move_foreground(s_overlay_back);
    }
}

lv_obj_t* BeginOverlay(const char* title) {
    if (s_overlay == nullptr) return nullptr;
    ClearOverlay();
    s_overlay_back = nullptr;
    s_overlay_title = nullptr;
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_overlay);

    MakeOverlayBackButton(s_overlay);

    s_overlay_title = MakeLabel(s_overlay, I18n::T(title), kText, FontSmall());
    lv_obj_align(s_overlay_title, LV_ALIGN_TOP_MID, 0, kRound ? 20 : 24);
    return s_overlay;
}

lv_obj_t* MakeRoller(lv_obj_t* parent, const char* options, int selected) {
    lv_obj_t* roller = lv_roller_create(parent);
    lv_roller_set_options(roller, options, LV_ROLLER_MODE_INFINITE);
    lv_roller_set_visible_row_count(roller, 3);
    lv_obj_set_style_text_font(roller, FontBig(), LV_PART_MAIN);
    lv_obj_set_style_text_font(roller, FontBig(), LV_PART_SELECTED);
    lv_obj_set_style_bg_color(roller, lv_color_hex(kBtnDark), LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(roller, LV_OPA_COVER, LV_PART_SELECTED);
    lv_obj_set_style_text_color(roller, lv_color_hex(kAccent), LV_PART_SELECTED);
    lv_obj_set_style_text_color(roller, lv_color_hex(kMuted), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(roller, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(roller, 0, LV_PART_MAIN);
    lv_roller_set_selected(roller, selected, LV_ANIM_OFF);
    screen_swipe_back_ignore(roller, true);
    return roller;
}

std::string BuildHourOpts() {
    std::string s;
    for (int i = 0; i < 24; ++i) {
        char b[8];
        snprintf(b, sizeof(b), "%02d", i);
        if (i) s += "\n";
        s += b;
    }
    return s;
}
std::string BuildMinOpts() {
    std::string s;
    for (int i = 0; i < 60; ++i) {
        char b[8];
        snprintf(b, sizeof(b), "%02d", i);
        if (i) s += "\n";
        s += b;
    }
    return s;
}

void SyncEditRepeatUi() {
    if (s_edit_once_btn && s_edit_repeat_btn) {
        auto paint = [](lv_obj_t* b, bool on) {
            lv_obj_t* l = lv_obj_get_child(b, 0);
            if (on) {
                lv_obj_set_style_bg_color(b, lv_color_hex(kAccent), LV_PART_MAIN);
                lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
                lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
                if (l) lv_obj_set_style_text_color(l, lv_color_hex(kBg), LV_PART_MAIN);
            } else {
                lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, LV_PART_MAIN);
                lv_obj_set_style_border_width(b, 1, LV_PART_MAIN);
                lv_obj_set_style_border_color(b, lv_color_hex(kAccent), LV_PART_MAIN);
                if (l) lv_obj_set_style_text_color(l, lv_color_hex(kAccent), LV_PART_MAIN);
            }
        };
        paint(s_edit_once_btn, s_edit_once);
        paint(s_edit_repeat_btn, !s_edit_once);
    }
    if (s_edit_days_row) {
        if (s_edit_once) lv_obj_add_flag(s_edit_days_row, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(s_edit_days_row, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_edit_once_hint) {
        if (s_edit_once) lv_obj_remove_flag(s_edit_once_hint, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_edit_once_hint, LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < 7; ++i) {
        if (!s_edit_day_btns[i]) continue;
        bool on = (s_edit_weekdays & (1u << i)) != 0;
        lv_obj_t* l = lv_obj_get_child(s_edit_day_btns[i], 0);
        if (on) {
            lv_obj_set_style_bg_color(s_edit_day_btns[i], lv_color_hex(kAccent),
                                      LV_PART_MAIN);
            lv_obj_set_style_bg_opa(s_edit_day_btns[i], LV_OPA_COVER, LV_PART_MAIN);
            if (l) lv_obj_set_style_text_color(l, lv_color_hex(kBg), LV_PART_MAIN);
        } else {
            lv_obj_set_style_bg_opa(s_edit_day_btns[i], LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_style_border_width(s_edit_day_btns[i], 1, LV_PART_MAIN);
            lv_obj_set_style_border_color(s_edit_day_btns[i], lv_color_hex(kAccent),
                                          LV_PART_MAIN);
            if (l) lv_obj_set_style_text_color(l, lv_color_hex(kAccent), LV_PART_MAIN);
        }
    }
}

void ShowAlarmEdit(int index) {
    s_edit_index = index;
    AlarmData draft;
    if (s_resume_draft) {
        draft = s_draft;
        s_resume_draft = false;
    } else if (index >= 0 && index < s_alarm_count) {
        draft = s_alarms[index];
    } else {
        draft = AlarmData{};
        draft.enabled = true;
        draft.once = true;
        draft.hour = 7;
        draft.minute = 30;
        draft.weekdays = 0x1F;
        draft.ringtone = 0;
    }

    lv_obj_t* ov = BeginOverlay(index >= 0 ? "编辑闹钟" : "添加闹钟");
    if (ov == nullptr) return;

    s_edit_once = draft.once;
    s_edit_weekdays = draft.weekdays ? draft.weekdays : 0x1F;
    s_edit_ringtone = draft.ringtone;

    static std::string hour_opts;
    static std::string min_opts;
    hour_opts = BuildHourOpts();
    min_opts = BuildMinOpts();

    const int roller_w = kRound ? 72 : 120;
    const int roller_h = kRound ? 100 : 160;
    s_edit_hour = MakeRoller(ov, hour_opts.c_str(), draft.hour);
    lv_obj_set_size(s_edit_hour, roller_w, roller_h);
    lv_obj_align(s_edit_hour, LV_ALIGN_TOP_MID, kRound ? -40 : -70, kRound ? 82 : 90);

    auto colon = MakeLabel(ov, ":", kText, FontBig());
    lv_obj_align(colon, LV_ALIGN_TOP_MID, 0, kRound ? 112 : 140);

    s_edit_min = MakeRoller(ov, min_opts.c_str(), draft.minute);
    lv_obj_set_size(s_edit_min, roller_w, roller_h);
    lv_obj_align(s_edit_min, LV_ALIGN_TOP_MID, kRound ? 40 : 70, kRound ? 82 : 90);

    // 单次 / 重复：两个独立胶囊，不要外套一层描边（会双边框、挤在一起）。
    lv_obj_t* seg = lv_obj_create(ov);
    screen_strip_obj_chrome(seg);
    const int seg_h = kRound ? 36 : 48;
    lv_obj_set_size(seg, kRound ? 220 : 340, seg_h);
    lv_obj_align(seg, LV_ALIGN_TOP_MID, 0, kRound ? 178 : 270);
    lv_obj_set_style_bg_opa(seg, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(seg, kRound ? 16 : 20, LV_PART_MAIN);
    lv_obj_set_flex_align(seg, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    auto make_seg = [&](const char* txt, bool once_side) {
        lv_obj_t* b = lv_btn_create(seg);
        lv_obj_set_size(b, kRound ? 90 : 148, seg_h);
        lv_obj_set_style_radius(b, seg_h / 2, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(b, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(b, lv_color_hex(kAccent), LV_PART_MAIN);
        lv_obj_t* l = lv_label_create(b);
        lv_label_set_text(l, I18n::T(txt));
        lv_obj_set_style_text_font(l, FontSmall(), LV_PART_MAIN);
        lv_obj_center(l);
        lv_obj_add_event_cb(
            b,
            [](lv_event_t* e) {
                s_edit_once = lv_event_get_user_data(e) != nullptr;
                SyncEditRepeatUi();
            },
            LV_EVENT_CLICKED, once_side ? reinterpret_cast<void*>(1) : nullptr);
        return b;
    };
    s_edit_once_btn = make_seg("单次", true);
    s_edit_repeat_btn = make_seg("重复", false);

    s_edit_once_hint = MakeLabel(ov, I18n::T("响铃一次后关闭"), kMuted, FontSmall());
    lv_obj_align(s_edit_once_hint, LV_ALIGN_TOP_MID, 0, kRound ? 218 : 330);

    s_edit_days_row = lv_obj_create(ov);
    screen_strip_obj_chrome(s_edit_days_row);
    lv_obj_set_size(s_edit_days_row, kRound ? 280 : 480, kRound ? 40 : 56);
    lv_obj_align(s_edit_days_row, LV_ALIGN_TOP_MID, 0, kRound ? 218 : 330);
    lv_obj_set_style_bg_opa(s_edit_days_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_edit_days_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_edit_days_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    static const char* kDayLab[] = {"一", "二", "三", "四", "五", "六", "日"};
    for (int i = 0; i < 7; ++i) {
        lv_obj_t* b = lv_btn_create(s_edit_days_row);
        const int ds = kRound ? 30 : 44;
        lv_obj_set_size(b, ds, ds);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);
        lv_obj_t* l = lv_label_create(b);
        lv_label_set_text(l, I18n::T(kDayLab[i]));
        lv_obj_set_style_text_font(l, FontSmall(), LV_PART_MAIN);
        lv_obj_center(l);
        lv_obj_add_event_cb(
            b,
            [](lv_event_t* e) {
                int idx = static_cast<int>(
                    reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
                s_edit_weekdays ^= static_cast<uint8_t>(1u << idx);
                SyncEditRepeatUi();
            },
            LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        s_edit_day_btns[i] = b;
    }

    // 铃声
    lv_obj_t* ring = lv_btn_create(ov);
    lv_obj_set_size(ring, kRound ? 240 : 400, kRound ? 36 : 48);
    lv_obj_align(ring, LV_ALIGN_TOP_MID, 0, kRound ? 262 : 400);
    lv_obj_set_style_radius(ring, 18, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ring, lv_color_hex(kRowBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ring, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(ring, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(ring, 0, LV_PART_MAIN);
    s_edit_ring_lbl = lv_label_create(ring);
    lv_label_set_text(s_edit_ring_lbl, I18n::T(kRingtones[s_edit_ringtone].name));
    lv_obj_set_style_text_font(s_edit_ring_lbl, FontSmall(), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_edit_ring_lbl, lv_color_hex(kText), LV_PART_MAIN);
    lv_obj_align(s_edit_ring_lbl, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_t* chev = lv_label_create(ring);
    lv_label_set_text(chev, ">");
    lv_obj_set_style_text_color(chev, lv_color_hex(kMuted), LV_PART_MAIN);
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_add_event_cb(ring,
                        [](lv_event_t*) {
                            s_draft.hour = s_edit_hour ? static_cast<uint8_t>(
                                lv_roller_get_selected(s_edit_hour)) : 7;
                            s_draft.minute = s_edit_min ? static_cast<uint8_t>(
                                lv_roller_get_selected(s_edit_min)) : 30;
                            s_draft.once = s_edit_once;
                            s_draft.weekdays = s_edit_weekdays;
                            s_draft.ringtone = s_edit_ringtone;
                            s_draft.enabled = true;
                            s_ringtone_return_edit = true;
                            ShowRingtonePicker();
                        },
                        LV_EVENT_CLICKED, nullptr);

    if (s_edit_index >= 0) {
        lv_obj_t* del = lv_btn_create(ov);
        lv_obj_set_size(del, kRound ? 90 : 120, kRound ? 32 : 40);
        lv_obj_align(del, LV_ALIGN_BOTTOM_MID, kRound ? -70 : -100,
                     kOverlayBtnBottomOfs);
        lv_obj_set_style_radius(del, 16, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(del, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(del, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(del, lv_color_hex(0xFF6B6B), LV_PART_MAIN);
        lv_obj_set_style_shadow_width(del, 0, LV_PART_MAIN);
        lv_obj_t* dl = lv_label_create(del);
        lv_label_set_text(dl, I18n::T("删除"));
        lv_obj_set_style_text_font(dl, FontSmall(), LV_PART_MAIN);
        lv_obj_set_style_text_color(dl, lv_color_hex(0xFF6B6B), LV_PART_MAIN);
        lv_obj_center(dl);
        lv_obj_add_event_cb(del, [](lv_event_t*) { DeleteEditingAlarm(); },
                            LV_EVENT_CLICKED, nullptr);
    }

    lv_obj_t* save = lv_btn_create(ov);
    lv_obj_set_size(save, kRound ? (s_edit_index >= 0 ? 100 : 200) : 320,
                    kRound ? 40 : 56);
    if (s_edit_index >= 0) {
        lv_obj_align(save, LV_ALIGN_BOTTOM_MID, kRound ? 60 : 100,
                     kOverlayBtnBottomOfs);
    } else {
        lv_obj_align(save, LV_ALIGN_BOTTOM_MID, 0, kOverlayBtnBottomOfs);
    }
    lv_obj_set_style_radius(save, 22, LV_PART_MAIN);
    lv_obj_set_style_bg_color(save, lv_color_hex(kAccent), LV_PART_MAIN);
    lv_obj_set_style_border_width(save, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(save, 0, LV_PART_MAIN);
    lv_obj_t* sl = lv_label_create(save);
    lv_label_set_text(sl, I18n::T("保存"));
    lv_obj_set_style_text_font(sl, FontSmall(), LV_PART_MAIN);
    lv_obj_set_style_text_color(sl, lv_color_hex(kBg), LV_PART_MAIN);
    lv_obj_center(sl);
    lv_obj_add_event_cb(save, [](lv_event_t*) { SaveAlarmFromEdit(); },
                        LV_EVENT_CLICKED, nullptr);

    SyncEditRepeatUi();
    RaiseOverlayChrome();
}

void DeleteEditingAlarm() {
    if (s_edit_index < 0 || s_edit_index >= s_alarm_count) {
        HideOverlay();
        return;
    }
    for (int i = s_edit_index; i < s_alarm_count - 1; ++i) {
        s_alarms[i] = s_alarms[i + 1];
    }
    --s_alarm_count;
    s_edit_index = -1;
    PersistAlarms();
    EnsureAlarmPoller();
    HideOverlay();
    RefreshAlarmPage();
}

void SaveAlarmFromEdit() {
    AlarmData a;
    a.hour = s_edit_hour
                 ? static_cast<uint8_t>(lv_roller_get_selected(s_edit_hour))
                 : 7;
    a.minute = s_edit_min
                   ? static_cast<uint8_t>(lv_roller_get_selected(s_edit_min))
                   : 30;
    a.once = s_edit_once;
    a.weekdays = s_edit_weekdays ? s_edit_weekdays : 0x1F;
    a.ringtone = s_edit_ringtone;
    a.enabled = true;

    if (s_edit_index >= 0 && s_edit_index < s_alarm_count) {
        a.enabled = s_alarms[s_edit_index].enabled;
        s_alarms[s_edit_index] = a;
    } else {
        if (s_alarm_count >= kMaxAlarms) {
            ShowAlarmLimitPopup();
            HideOverlay();
            return;
        }
        s_alarms[s_alarm_count++] = a;
    }
    PersistAlarms();
    EnsureAlarmPoller();
    HideOverlay();
    RefreshAlarmPage();
}

void ShowRingtonePicker() {
    lv_obj_t* ov = BeginOverlay("选择铃声");
    if (ov == nullptr) return;

    for (int i = 0; i < kRingtoneCount; ++i) {
        lv_obj_t* row = lv_btn_create(ov);
        lv_obj_set_size(row, kRound ? 260 : 420, kRound ? 40 : 52);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, (kRound ? 80 : 100) + i * (kRound ? 46 : 60));
        lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, lv_color_hex(kRowBg), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(row, 0, LV_PART_MAIN);
        lv_obj_t* name = lv_label_create(row);
        lv_label_set_text(name, I18n::T(kRingtones[i].name));
        lv_obj_set_style_text_font(name, FontSmall(), LV_PART_MAIN);
        bool sel = (i == static_cast<int>(s_edit_ringtone));
        lv_obj_set_style_text_color(name, lv_color_hex(sel ? kAccent : kText),
                                    LV_PART_MAIN);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 16, 0);
        if (sel) {
            lv_obj_t* ck = lv_label_create(row);
            lv_label_set_text(ck, "✓");
            lv_obj_set_style_text_color(ck, lv_color_hex(kAccent), LV_PART_MAIN);
            lv_obj_align(ck, LV_ALIGN_RIGHT_MID, -16, 0);
        }
        lv_obj_add_event_cb(
            row,
            [](lv_event_t* e) {
                int idx = static_cast<int>(
                    reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
                s_edit_ringtone = static_cast<uint8_t>(idx);
                PlayRingtone(s_edit_ringtone);
                if (s_ringtone_return_edit) {
                    s_draft.ringtone = s_edit_ringtone;
                    s_resume_draft = true;
                    ShowAlarmEdit(s_edit_index);
                } else {
                    HideOverlay();
                }
            },
            LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
    }
    RaiseOverlayChrome();
}

void ShowCountdownCustom() {
    lv_obj_t* ov = BeginOverlay("自定义时长");
    if (ov == nullptr) return;

    static std::string hour_opts;
    static std::string min_opts;
    static std::string sec_opts;
    hour_opts = BuildHourOpts();
    min_opts = BuildMinOpts();
    sec_opts = BuildMinOpts();

    int total_sec = 0;
    {
        std::lock_guard<std::mutex> lock(s_runtime_mutex);
        total_sec = s_cd_total_sec;
    }
    int h = total_sec / 3600;
    int m = (total_sec % 3600) / 60;
    int s = total_sec % 60;

    const int rw = kRound ? 70 : 110;
    const int rh = kRound ? 110 : 180;
    s_cd_h = MakeRoller(ov, hour_opts.c_str(), h);
    lv_obj_set_size(s_cd_h, rw, rh);
    lv_obj_align(s_cd_h, LV_ALIGN_CENTER, kRound ? -90 : -140, kRound ? -10 : -20);
    auto uh = MakeLabel(ov, I18n::T("时"), kMuted, FontSmall());
    lv_obj_align_to(uh, s_cd_h, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);

    s_cd_m = MakeRoller(ov, min_opts.c_str(), m);
    lv_obj_set_size(s_cd_m, rw, rh);
    lv_obj_align(s_cd_m, LV_ALIGN_CENTER, 0, kRound ? -10 : -20);
    auto um = MakeLabel(ov, I18n::T("分"), kMuted, FontSmall());
    lv_obj_align_to(um, s_cd_m, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);

    s_cd_s = MakeRoller(ov, sec_opts.c_str(), s);
    lv_obj_set_size(s_cd_s, rw, rh);
    lv_obj_align(s_cd_s, LV_ALIGN_CENTER, kRound ? 90 : 140, kRound ? -10 : -20);
    auto us = MakeLabel(ov, I18n::T("秒"), kMuted, FontSmall());
    lv_obj_align_to(us, s_cd_s, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);

    lv_obj_t* ok = lv_btn_create(ov);
    lv_obj_set_size(ok, kRound ? 200 : 320, kRound ? 40 : 56);
    lv_obj_align(ok, LV_ALIGN_BOTTOM_MID, 0, kOverlayBtnBottomOfs);
    lv_obj_set_style_radius(ok, 22, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ok, lv_color_hex(kAccent), LV_PART_MAIN);
    lv_obj_set_style_border_width(ok, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(ok, 0, LV_PART_MAIN);
    lv_obj_t* ol = lv_label_create(ok);
    lv_label_set_text(ol, I18n::T("确定"));
    lv_obj_set_style_text_font(ol, FontSmall(), LV_PART_MAIN);
    lv_obj_set_style_text_color(ol, lv_color_hex(kBg), LV_PART_MAIN);
    lv_obj_center(ol);
    lv_obj_add_event_cb(
        ok,
        [](lv_event_t*) {
            int hh = s_cd_h ? lv_roller_get_selected(s_cd_h) : 0;
            int mm = s_cd_m ? lv_roller_get_selected(s_cd_m) : 0;
            int ss = s_cd_s ? lv_roller_get_selected(s_cd_s) : 0;
            int total = hh * 3600 + mm * 60 + ss;
            if (total < 1) total = 1;
            SetCountdownSeconds(total);
            HideOverlay();
        },
        LV_EVENT_CLICKED, nullptr);
    RaiseOverlayChrome();
}

// ---------- page builders ----------
void BuildTabBar(lv_obj_t* parent) {
    lv_obj_t* bar = lv_obj_create(parent);
    screen_strip_obj_chrome(bar);
    lv_obj_set_size(bar, kTabBarW, kTabBarH);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, kTabBarBottomOfs);
    lv_obj_set_style_radius(bar, kTabBarH / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kBtnDark), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bar, 8, LV_PART_MAIN);
    screen_swipe_back_ignore(bar, true);

    const char* labels[3] = {"闹钟", "计时", "倒计时"};
    for (int i = 0; i < 3; ++i) {
        lv_obj_t* b = lv_btn_create(bar);
        lv_obj_set_size(b, kRound ? 80 : 140, kRound ? 32 : 44);
        lv_obj_set_style_radius(b, 16, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
        lv_obj_t* l = lv_label_create(b);
        lv_label_set_text(l, I18n::T(labels[i]));
        lv_obj_set_style_text_font(l, FontSmall(), LV_PART_MAIN);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, OnTabClicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        s_tab_btns[i] = b;
    }
}

void BuildAlarmPage(lv_obj_t* parent) {
    s_page_alarm = lv_obj_create(parent);
    screen_strip_obj_chrome(s_page_alarm);
    lv_obj_set_size(s_page_alarm, kPanel, kPanel);
    lv_obj_set_style_bg_opa(s_page_alarm, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_scroll_dir(s_page_alarm, LV_DIR_NONE);

    MakeLabel(s_page_alarm, I18n::T("闹钟"), kText, FontSmall());
    lv_obj_align(lv_obj_get_child(s_page_alarm, 0), LV_ALIGN_TOP_MID, 0,
                 kRound ? 36 : 48);

    s_alarm_empty_lbl =
        MakeLabel(s_page_alarm, I18n::T("暂无闹钟，点击添加"), kMuted, FontSmall());
    lv_obj_align(s_alarm_empty_lbl, LV_ALIGN_CENTER, 0, kRound ? -20 : -30);

    s_alarm_list = lv_obj_create(s_page_alarm);
    screen_strip_obj_chrome(s_alarm_list);
    const int list_h = kAlarmListH;
    lv_obj_set_size(s_alarm_list, kRound ? 280 : 460, list_h);
    lv_obj_align(s_alarm_list, LV_ALIGN_TOP_MID, 0, kRound ? 62 : 90);
    lv_obj_set_style_bg_opa(s_alarm_list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_alarm_list, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_alarm_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_alarm_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(s_alarm_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_alarm_list, LV_SCROLLBAR_MODE_OFF);
    screen_swipe_back_ignore(s_alarm_list, true);

    lv_obj_t* add = lv_btn_create(s_page_alarm);
    const int bs = kRound ? 48 : 72;
    StyleFillBtn(add, bs);
    lv_obj_align(add, LV_ALIGN_BOTTOM_MID, 0, kAddBtnBottomOfs);
    lv_obj_t* al = lv_label_create(add);
    lv_label_set_text(al, "+");
    lv_obj_set_style_text_font(al, FontBig(), LV_PART_MAIN);
    lv_obj_set_style_text_color(al, lv_color_hex(kBg), LV_PART_MAIN);
    lv_obj_center(al);
    lv_obj_add_event_cb(
        add,
        [](lv_event_t*) {
            if (s_alarm_count >= kMaxAlarms) {
                ShowAlarmLimitPopup();
                return;
            }
            ShowAlarmEdit(-1);
        },
        LV_EVENT_CLICKED, nullptr);

    RefreshAlarmPage();
}

void BuildStopwatchPage(lv_obj_t* parent) {
    s_page_sw = lv_obj_create(parent);
    screen_strip_obj_chrome(s_page_sw);
    lv_obj_set_size(s_page_sw, kPanel, kPanel);
    lv_obj_set_style_bg_opa(s_page_sw, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(s_page_sw, LV_OBJ_FLAG_HIDDEN);

    MakeLabel(s_page_sw, I18n::T("正计时"), kAccent, FontSmall());
    lv_obj_align(lv_obj_get_child(s_page_sw, 0), LV_ALIGN_TOP_MID, 0,
                 kRound ? 56 : 90);

    s_sw_lbl = MakeLabel(s_page_sw, "00:00.00", kText, FontBig());
    lv_obj_align(s_sw_lbl, LV_ALIGN_TOP_MID, 0, kRound ? 100 : 160);

    const int bs = kRound ? 52 : 80;
    lv_obj_t* reset = lv_btn_create(s_page_sw);
    StyleGhostBtn(reset, bs);
    lv_obj_align(reset, LV_ALIGN_CENTER, kRound ? -70 : -110, kSwBtnCenterY);
    lv_obj_t* rl = lv_label_create(reset);
    lv_label_set_text(rl, I18n::T("复位"));
    lv_obj_set_style_text_font(rl, FontSmall(), LV_PART_MAIN);
    lv_obj_set_style_text_color(rl, lv_color_hex(kAccent), LV_PART_MAIN);
    lv_obj_center(rl);
    lv_obj_add_event_cb(reset, OnSwReset, LV_EVENT_CLICKED, nullptr);

    s_sw_start_btn = lv_btn_create(s_page_sw);
    StyleFillBtn(s_sw_start_btn, bs);
    lv_obj_align(s_sw_start_btn, LV_ALIGN_CENTER, 0, kSwBtnCenterY);
    lv_obj_t* sl = lv_label_create(s_sw_start_btn);
    lv_label_set_text(sl, I18n::T("开始"));
    lv_obj_set_style_text_font(sl, FontSmall(), LV_PART_MAIN);
    lv_obj_set_style_text_color(sl, lv_color_hex(kBg), LV_PART_MAIN);
    lv_obj_center(sl);
    lv_obj_add_event_cb(s_sw_start_btn, OnSwStart, LV_EVENT_CLICKED,
                        s_sw_start_btn);

    lv_obj_t* lap = lv_btn_create(s_page_sw);
    StyleGhostBtn(lap, bs);
    lv_obj_align(lap, LV_ALIGN_CENTER, kRound ? 70 : 110, kSwBtnCenterY);
    lv_obj_t* ll = lv_label_create(lap);
    lv_label_set_text(ll, I18n::T("计次"));
    lv_obj_set_style_text_font(ll, FontSmall(), LV_PART_MAIN);
    lv_obj_set_style_text_color(ll, lv_color_hex(kAccent), LV_PART_MAIN);
    lv_obj_center(ll);
    // 计次：当前版本先记录日志，后续可加列表
    lv_obj_add_event_cb(
        lap,
        [](lv_event_t*) {
            int64_t elapsed_ms = 0;
            {
                std::lock_guard<std::mutex> lock(s_runtime_mutex);
                elapsed_ms = s_sw_elapsed_ms;
            }
            ESP_LOGI(TAG, "lap at %lld ms", static_cast<long long>(elapsed_ms));
        },
        LV_EVENT_CLICKED, nullptr);

    RefreshStopwatchLabel();
    UpdateRuntimeButtonLabels();
}

void BuildCountdownPage(lv_obj_t* parent) {
    s_page_cd = lv_obj_create(parent);
    screen_strip_obj_chrome(s_page_cd);
    lv_obj_set_size(s_page_cd, kPanel, kPanel);
    lv_obj_set_style_bg_opa(s_page_cd, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(s_page_cd, LV_OBJ_FLAG_HIDDEN);

    s_cd_lbl = MakeLabel(s_page_cd, "05:00", kText, FontBig());
    lv_obj_align(s_cd_lbl, LV_ALIGN_TOP_MID, 0, kRound ? 80 : 130);

    lv_obj_t* presets = lv_obj_create(s_page_cd);
    screen_strip_obj_chrome(presets);
    lv_obj_set_size(presets, kRound ? 280 : 420, kRound ? 36 : 48);
    lv_obj_align(presets, LV_ALIGN_TOP_MID, 0, kCdPresetY);
    lv_obj_set_style_bg_opa(presets, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(presets, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(presets, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    struct Preset {
        const char* label;
        int sec;
    };
    const Preset ps[] = {{"1分", 60}, {"5分", 300}, {"10分", 600}, {"15分", 900}};
    for (const auto& p : ps) {
        lv_obj_t* b = lv_btn_create(presets);
        lv_obj_set_size(b, kRound ? 56 : 80, kRound ? 28 : 36);
        lv_obj_set_style_radius(b, 14, LV_PART_MAIN);
        lv_obj_set_style_bg_color(b, lv_color_hex(kRowBg), LV_PART_MAIN);
        lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);
        lv_obj_t* l = lv_label_create(b);
        lv_label_set_text(l, I18n::T(p.label));
        lv_obj_set_style_text_font(l, FontSmall(), LV_PART_MAIN);
        lv_obj_set_style_text_color(l, lv_color_hex(kText), LV_PART_MAIN);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, OnPreset, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(p.sec)));
    }

    // 快捷预设下沿约 176。两个主按钮都用 TOP_MID，中间留 12px，
    // 避免自定义底边和开始顶边叠在一起（原先一个 TOP、一个 CENTER）。
    const int btn_w = kRound ? 140 : 200;
    const int custom_h = kRound ? 36 : 44;
    const int start_h = kRound ? 40 : 56;
    const int custom_y = kCdCustomY;
    const int start_y = custom_y + custom_h + 10;

    lv_obj_t* custom = lv_btn_create(s_page_cd);
    lv_obj_set_size(custom, btn_w, custom_h);
    lv_obj_align(custom, LV_ALIGN_TOP_MID, 0, custom_y);
    lv_obj_set_style_radius(custom, custom_h / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(custom, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(custom, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(custom, lv_color_hex(kAccent), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(custom, 0, LV_PART_MAIN);
    lv_obj_t* cl = lv_label_create(custom);
    lv_label_set_text(cl, I18n::T("自定义"));
    lv_obj_set_style_text_font(cl, FontSmall(), LV_PART_MAIN);
    lv_obj_set_style_text_color(cl, lv_color_hex(kAccent), LV_PART_MAIN);
    lv_obj_center(cl);
    lv_obj_add_event_cb(custom, [](lv_event_t*) { ShowCountdownCustom(); },
                        LV_EVENT_CLICKED, nullptr);

    s_cd_start_btn = lv_btn_create(s_page_cd);
    lv_obj_set_size(s_cd_start_btn, btn_w, start_h);
    lv_obj_align(s_cd_start_btn, LV_ALIGN_TOP_MID, 0, start_y);
    lv_obj_set_style_radius(s_cd_start_btn, start_h / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_cd_start_btn, lv_color_hex(kAccent), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_cd_start_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_cd_start_btn, 0, LV_PART_MAIN);
    lv_obj_t* sl = lv_label_create(s_cd_start_btn);
    lv_label_set_text(sl, I18n::T("开始"));
    lv_obj_set_style_text_font(sl, FontSmall(), LV_PART_MAIN);
    lv_obj_set_style_text_color(sl, lv_color_hex(kBg), LV_PART_MAIN);
    lv_obj_center(sl);
    lv_obj_add_event_cb(s_cd_start_btn, OnCdStart, LV_EVENT_CLICKED, nullptr);

    RefreshCountdownLabel();
    UpdateRuntimeButtonLabels();
}

void OnScrDeleted(lv_event_t* e) {
    // 旧页面异步删除时不能清空新页面的全局控件指针。
    if (lv_event_get_current_target(e) != s_scr) return;
    s_clock_screen_loaded.store(false, std::memory_order_release);
    s_runtime_ui_update_pending.store(false, std::memory_order_release);
    s_limit_popup = nullptr;
    s_scr = nullptr;
    s_page_alarm = s_page_sw = s_page_cd = nullptr;
    s_overlay = nullptr;
    s_alarm_list = nullptr;
    s_alarm_empty_lbl = nullptr;
    s_sw_lbl = nullptr;
    s_sw_start_btn = nullptr;
    s_cd_lbl = s_cd_start_btn = nullptr;
    for (int i = 0; i < 3; ++i) s_tab_btns[i] = nullptr;
}

}  // namespace

lv_obj_t* ClockScreen::Create() {
    LoadAlarms();

    s_scr = lv_obj_create(nullptr);
    StyleScreen(s_scr);
    lv_obj_add_event_cb(s_scr, OnScrDeleted, LV_EVENT_DELETE, nullptr);
    screen_attach_swipe_back(s_scr, OnSwipeBack);

    BuildAlarmPage(s_scr);
    BuildStopwatchPage(s_scr);
    BuildCountdownPage(s_scr);
    BuildTabBar(s_scr);

    s_overlay = lv_obj_create(s_scr);
    screen_strip_obj_chrome(s_overlay);
    lv_obj_set_size(s_overlay, kPanel, kPanel);
    StyleScreen(s_overlay);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_scroll_dir(s_overlay, LV_DIR_NONE);

    ShowTab(Tab::kAlarm);
    EnsureAlarmPoller();
    return s_scr;
}

void ClockScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        s_clock_screen_loaded.store(true, std::memory_order_release);
        EnsureAlarmPoller();
        bool runtime_active = false;
        {
            std::lock_guard<std::mutex> lock(s_runtime_mutex);
            runtime_active = s_sw_running || s_cd_running;
        }
        if (runtime_active) EnsureRuntimeTimer();
        RefreshRuntimeUiAsync(nullptr);
    } else {
        s_clock_screen_loaded.store(false, std::memory_order_release);
        // 退出页面只释放 LVGL 控件，正计时/倒计时状态和后台计时继续保留。
    }
}
