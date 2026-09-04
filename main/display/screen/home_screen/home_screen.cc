#include "home_screen.h"
#include "i18n.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <ctime>
#include <string>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <font_awesome.h>

#include "application.h"
#include "board.h"
#include "config.h"
#include "display_refresh_blank.h"
#include "dual_network_board.h"
#include "nt26_board.h"
#include "IOExpander.hpp"
#include "bq27220_gauge.h"

#ifndef BOARD_HAS_DUAL_SIM
#define BOARD_HAS_DUAL_SIM 0
#endif

#if CONFIG_BOARD_TYPE_ESP_VOCAT
extern "C" void board_release_power_hold_if_supported();
#endif
#include "settings.h"
#include "settings_screen/settings_screen.h"
#include "call_screen/call_screen.h"
#include "album_screen/album_screen.h"
#include "clock_screen/clock_screen.h"
#include "digital_people_screen/digital_people_screen.h"
#include "game_2048_screen/game_2048_screen.h"
#include "gps_screen/gps_screen.h"
#include "level_screen/level_screen.h"
#include "magnet_screen/magnet_screen.h"
#include "music_screen/music_screen.h"
#include "recording_screen/recording_screen.h"
#include "pwr_key_handler.h"
#include "screen_util.h"
#include "idle_power_policy.h"
#include "vibrate_screen/vibrate_screen.h"
#if !defined(BOARD_ESP_SHOW)
#include "weather_screen/weather_screen.h"
#endif
#include "network_screen/network_screen.h"
#include "pin_test_screen/pin_test_screen.h"
#include "test_screen/test_screen.h"
#include "sd_card_screen/sd_card_screen.h"
#include "info_screen/info_screen.h"
#include "wifi_required_dialog.h"
#include "badge_screen/badge_screen.h"
#include "bagclip_screen/bagclip_screen.h"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_awesome_20_4);

// ???????? = ???Font Awesome ?????????0 = ??????
#define HOME_STATUS_SHOW_BATTERY_ICON 1

namespace {

constexpr const char* TAG_HOME = "HomeScreen";

// ---------------------------------------------------------------------------
// Per-app lifecycle callbacks
//
// Each launcher hands its callback to screen_attach_lifecycle() so we get a
// LOAD notification right after the new screen becomes active, and an
// UNLOAD notification when LVGL switches away from it.  For now we only log
// the transitions -- but this is the right place to hang start / stop
// behaviour that should track a specific app's lifetime (e.g. pausing the
// audio player when the player screen is dismissed).
//
// All callbacks share the same shape so they can all sit in the AppEntry
// table below.  A nullptr callback simply skips logging for that app.
// ---------------------------------------------------------------------------

void game_2048_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("game_2048", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: game_2048");
    } else {
        ESP_LOGI(TAG_HOME, "unload: game_2048");
    }
}

void call_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("call", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: call_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: call_screen");
    }
    // ????CallScreen ????
// PA_SWITCH ????????????
CallScreen::LifecycleCallback(event);
}

void clock_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("alarm", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: clock_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: clock_screen");
    }
    ClockScreen::LifecycleCallback(event);
}

void album_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("album", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: album_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: album_screen");
    }
    AlbumScreen::LifecycleCallback(event);
}

// ??????????????BT ????3????/ ??UART ????
// ??MusicScreen::LifecycleCallback ????????
void music_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("music", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: music_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: music_screen");
    }
    MusicScreen::LifecycleCallback(event);
}

void recording_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("recording", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: recording_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: recording_screen");
    }
    RecordingScreen::LifecycleCallback(event);
}

#if !defined(BOARD_ESP_SHOW)
void weather_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("weather", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: weather_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: weather_screen");
    }
}
#endif

// GPS ????GPS_POWER ???????GpsScreen::LifecycleCallback????// ??????+ ???? vibrate / bluetooth ??????????
void gps_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("gps", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: gps_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: gps_screen");
    }
    GpsScreen::LifecycleCallback(event);
}

// ??????????
// VibrateScreen::LifecycleCallback??// LOAD ????LEDC ????duty=0?UNLOAD ????pattern timer ?? duty=0??// ?????????????????
void vibrate_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("vibrate", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: vibrate_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: vibrate_screen");
    }
    VibrateScreen::LifecycleCallback(event);
}

// ????????????WifiStation ??????STA ??????/ ????// ????????????NetworkScreen::LifecycleCallback ???
void wifi_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("network", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: network_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: network_screen");
    }
    NetworkScreen::LifecycleCallback(event);
}

void digital_people_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("digital_people", event);
    DigitalPeopleScreen::LifecycleCallback(event);
}

// SD ??????????SdCardScreen::LifecycleCallback??// LOAD ????SD ?????????UNLOAD ??????SD ???// ??????
void sd_card_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("sd_card", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: sd_card_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: sd_card_screen");
    }
    SdCardScreen::LifecycleCallback(event);
}

// 像章/背包扣：UNLOAD 时由各自屏幕释放图片缓冲和定时器，此处只记日志。
void badge_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("badge", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: badge_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: badge_screen");
    }
    BadgeScreen::LifecycleCallback(event);
}

void bagclip_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("bagclip", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: bagclip_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: bagclip_screen");
    }
    BagclipScreen::LifecycleCallback(event);
}

void pin_test_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("pin_test", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: pin_test_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: pin_test_screen");
    }
    PinTestScreen::LifecycleCallback(event);
}

void test_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("test", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: test_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: test_screen");
    }
    TestScreen::LifecycleCallback(event);
}

// ????????????LevelScreen::LifecycleCallback?LOAD ????// SC7A20H ?? probe + configure ??UNLOAD ??
// LevelScreen ????
// sample timer?OnScreenUnloaded ??????
void level_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("level", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: level_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: level_screen");
    }
    LevelScreen::LifecycleCallback(event);
}

// ??????????
// MagnetScreen::LifecycleCallback?LOAD ??
// QMC6309
// ????probe + configure ???UNLOAD ??MagnetScreen ????// OnScreenUnloaded ??????timer??
void magnet_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("magnet", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: magnet_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: magnet_screen");
    }
    MagnetScreen::LifecycleCallback(event);
}

constexpr int kPanelW = DISPLAY_WIDTH;
constexpr int kPanelH = DISPLAY_HEIGHT;
// Claw4 方形 720；S31 Korvo-1 横屏 800x480。按分辨率选布局档。
constexpr int kPanelSize = (kPanelW < kPanelH) ? kPanelW : kPanelH;
constexpr bool kLayoutTall = (kPanelH >= 700);                 // 720x720
constexpr bool kLayoutWide = (kPanelW >= 750 && kPanelH < 700);  // 800x480
constexpr bool kLayoutRoundSmall = (kPanelW == 360 && kPanelH == 360);
// 360 圆屏：每页 2 个 App；图标保持资源原生 128x128（与图一一致，禁止缩小拉伸）。
constexpr int kIconAssetSize = 128;
constexpr int kStatusBarHeight =
    kLayoutTall ? 48 : (kLayoutWide ? 28 : (kLayoutRoundSmall ? 36 : 32));
constexpr int kIndicatorAreaHeight =
    kLayoutTall ? 40 : (kLayoutWide ? 0 : (kLayoutRoundSmall ? 30 : 28));
constexpr int kPagerHeight =
    kPanelH - kStatusBarHeight - kIndicatorAreaHeight;
constexpr int kPageCols = kLayoutRoundSmall ? 2 : 3;
constexpr int kPageRows = kLayoutRoundSmall ? 1 : 3;
constexpr int kAppsPerPage = kPageCols * kPageRows;
constexpr int kIconSize = kIconAssetSize;
constexpr int kCellWidth =
    kLayoutTall ? 160 : (kLayoutWide ? 168 : (kLayoutRoundSmall ? 148 : 150));
constexpr int kNameGap = kLayoutTall ? 6 : (kLayoutWide ? 2 : (kLayoutRoundSmall ? 6 : 4));
constexpr int kNameAreaH =
    kLayoutTall ? 24 : (kLayoutWide ? 20 : (kLayoutRoundSmall ? 22 : 22));
constexpr int kCellHeight = kIconSize + kNameGap + kNameAreaH;
constexpr int kGridColGap =
    kLayoutTall ? 60 : (kLayoutWide ? 40 : (kLayoutRoundSmall ? 12 : 12));
constexpr int kGridRowGap =
    kLayoutTall ? 36 : (kLayoutWide ? 0 : 8);
constexpr int kPagePadHor = kLayoutRoundSmall
    ? (kPanelW - kPageCols * kCellWidth - (kPageCols - 1) * kGridColGap) / 2
    : (kPanelW - kPageCols * kCellWidth - (kPageCols - 1) * kGridColGap) / 2;
constexpr int kPageContentH =
    kPageRows * kCellHeight + (kPageRows - 1) * kGridRowGap;
constexpr int kPagePadVer =
    (kPagerHeight > kPageContentH) ? (kPagerHeight - kPageContentH) / 2 : 0;

constexpr uint32_t kStatusBarBg = 0x000000;
constexpr int kMaxPages = 16;  // round 每页 2 个 App，页数更多

// Grid descriptors -- static so the array pointers passed to LVGL outlive
// the call.  Initialized at namespace scope; LVGL reads them lazily during
// each page's relayout, so we never have to refresh them.
int32_t s_col_dsc[4] = {
    kCellWidth,
    kPageCols >= 2 ? kCellWidth : LV_GRID_TEMPLATE_LAST,
    kPageCols >= 3 ? kCellWidth : LV_GRID_TEMPLATE_LAST,
    LV_GRID_TEMPLATE_LAST,
};
int32_t s_row_dsc[4] = {
    kCellHeight,
    kPageRows >= 2 ? kCellHeight : LV_GRID_TEMPLATE_LAST,
    kPageRows >= 3 ? kCellHeight : LV_GRID_TEMPLATE_LAST,
    LV_GRID_TEMPLATE_LAST,
};

// Indicator dot geometry
constexpr int kDotSize = kLayoutRoundSmall ? 6 : 8;
constexpr int kDotGap = kLayoutRoundSmall ? 8 : 12;
constexpr int kIndicatorPadHor = kLayoutRoundSmall ? 10 : 14;
constexpr int kIndicatorPadVer = kLayoutRoundSmall ? 6 : 8;
constexpr int kIndicatorYOffset = kLayoutRoundSmall ? 18 : 12;  // distance from panel bottom
constexpr uint32_t kIndicatorBg = 0x000000;
constexpr uint32_t kDotColor = 0xFFFFFF;

// Launchers take the lifecycle callback as an argument so we can attach it
// to the screen object before lv_screen_load() fires LV_EVENT_SCREEN_LOADED
// -- otherwise the first LOAD event would be missed.  Passing nullptr is
// supported (screen_attach_lifecycle no-ops in that case).
typedef void (*LaunchFn)(screen_lifecycle_cb_t lifecycle_cb);

struct AppEntry {
    // icon_suffix 拼到固定主题前缀，得到 A:ic_app_home_theme{N}_{suffix}.spng
    // 路径在 EnsureIconPathsBuilt() 里生成，cell 只持有指针。
    const char* icon_suffix;
    const char* name;                    // display name shown under the icon
    LaunchFn launch;                     // tapped -> launch this app (nullptr = no action)
    screen_lifecycle_cb_t lifecycle_cb;  // load / unload observer
    // WiFi ????????????WiFi??G ???????????
bool requires_wifi;
};

void LaunchGame2048(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* game = Game2048::Create();
    screen_attach_lifecycle(game, lifecycle_cb);
    lv_screen_load(game);
    if (old_scr != nullptr && old_scr != game) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchCall(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = CallScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchClock(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = ClockScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchAlbum(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = AlbumScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchMusic(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = MusicScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchRecording(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = RecordingScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

#if !defined(BOARD_ESP_SHOW)
void LaunchWeather(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = WeatherScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}
#endif

void LaunchGps(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = GpsScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchVibrate(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = VibrateScreen::Create();
    if (app == nullptr) {
        ESP_LOGE(TAG_HOME, "VibrateScreen::Create() failed");
        return;
    }
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchWifi(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = NetworkScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchDigitalPeople(screen_lifecycle_cb_t lifecycle_cb) {
    // QSPI PARTIAL：进页首帧条带刷藏在黑屏里，避免从上往下露出来。
    DisplayRefreshBlank blank;
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = DigitalPeopleScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    lv_refr_now(lv_display_get_default());
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchLevel(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = LevelScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchMagnet(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = MagnetScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchSdCard(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = SdCardScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchBadge(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = BadgeScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchBagclip(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = BagclipScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchPinTest(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = PinTestScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchTest(screen_lifecycle_cb_t lifecycle_cb) {
    TestScreen::LaunchFromHome(lifecycle_cb);
}

// ESPClaw???? ??????????ota_1 ?????? edge_agent??
bool s_espclaw_switching = false;
lv_obj_t* s_espclaw_overlay = nullptr;
lv_obj_t* s_espclaw_msg_lbl = nullptr;
lv_timer_t* s_espclaw_fail_timer = nullptr;

void StopHomeIdleTimer();  // defined later in this TU

void CloseEspClawPopup() {
    if (s_espclaw_fail_timer != nullptr) {
        lv_timer_delete(s_espclaw_fail_timer);
        s_espclaw_fail_timer = nullptr;
    }
    if (s_espclaw_overlay != nullptr) {
        lv_obj_delete(s_espclaw_overlay);
        s_espclaw_overlay = nullptr;
    }
    s_espclaw_msg_lbl = nullptr;
    s_espclaw_switching = false;
}

void EspClawFailCloseTimer(lv_timer_t* /*timer*/) {
    s_espclaw_fail_timer = nullptr;
    CloseEspClawPopup();
}

void EspClawSwitchFailAsync(void* user_data) {
    const char* msg = static_cast<const char*>(user_data);
    if (s_espclaw_msg_lbl != nullptr && msg != nullptr) {
        lv_label_set_text(s_espclaw_msg_lbl, msg);
    }
    s_espclaw_switching = false;
    if (s_espclaw_fail_timer != nullptr) {
        lv_timer_delete(s_espclaw_fail_timer);
    }
    // ??????2.5s ??????????
    s_espclaw_fail_timer =
        lv_timer_create(EspClawFailCloseTimer, 2500, nullptr);
    lv_timer_set_repeat_count(s_espclaw_fail_timer, 1);
}

void EspClawSwitchTask(void* /*arg*/) {
    // ??????????
vTaskDelay(pdMS_TO_TICKS(1500));

    const esp_partition_t* ota1 = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
    if (ota1 == nullptr) {
        ESP_LOGE(TAG_HOME, "ESPClaw: ota_1 partition not found");
        screen_async_call(EspClawSwitchFailAsync,
                      const_cast<char*>(I18n::T("未找到 ESPClaw\n请确认是否已安装到分区")));
        vTaskDelete(nullptr);
        return;
    }

    esp_err_t err = esp_ota_set_boot_partition(ota1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_HOME, "ESPClaw: set boot to %s failed: %s", ota1->label,
                 esp_err_to_name(err));
        screen_async_call(EspClawSwitchFailAsync,
                      const_cast<char*>(I18n::T("未找到 ESPClaw\n请确认是否已安装到分区")));
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGW(TAG_HOME, "ESPClaw: boot -> %s, rebooting", ota1->label);
    Application::GetInstance().Reboot();
    vTaskDelete(nullptr);
}

void ShowEspClawSwitchPopup() {
    lv_obj_t* scr = lv_screen_active();
    if (scr == nullptr) {
        return;
    }
    CloseEspClawPopup();
    s_espclaw_switching = true;

    constexpr int kCardW = 520;
    constexpr int kCardH = 320;

    lv_obj_t* mask = lv_obj_create(scr);
    lv_obj_remove_style_all(mask);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(mask, kPanelW, kPanelH);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_80, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(mask, true);
    s_espclaw_overlay = mask;

    lv_obj_t* card = lv_obj_create(mask);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, kCardW, kCardH);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1B2030), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 24, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(card, true);

    lv_obj_t* head = lv_label_create(card);
    lv_label_set_text(head, "ESPClaw");
    lv_obj_set_style_text_color(head, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(head, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(head, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* body = lv_label_create(card);
    s_espclaw_msg_lbl = body;
    lv_label_set_text(body, I18n::T("即将进入 ESPClaw..."));
    lv_obj_set_width(body, kCardW - 48);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(body, lv_color_hex(0xE5E7EB), LV_PART_MAIN);
    lv_obj_set_style_text_font(body, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(body, LV_ALIGN_CENTER, 0, 20);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_CLICKABLE);
}

void LaunchEspClaw(screen_lifecycle_cb_t /*lifecycle_cb*/) {
    if (s_espclaw_switching) {
        return;
    }
    StopHomeIdleTimer();
    ShowEspClawSwitchPopup();
    if (xTaskCreate(EspClawSwitchTask, "espclaw_sw", 4096, nullptr, 5,
                    nullptr) != pdPASS) {
        ESP_LOGE(TAG_HOME, "ESPClaw: failed to create switch task");
        EspClawSwitchFailAsync(
            const_cast<char*>(I18n::T("未找到 ESPClaw\n请确认是否已安装到分区")));
    }
}

void LaunchSettings(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = SettingsScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchInfo(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = InfoScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void settings_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("settings", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: settings_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: settings_screen");
    }
    SettingsScreen::LifecycleCallback(event);
}

void info_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("info", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: info_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: info_screen");
    }
    InfoScreen::LifecycleCallback(event);
}

void LaunchWake(screen_lifecycle_cb_t /*lifecycle_cb*/) {
    LaunchDigitalPeople(digital_people_lifecycle_cb);
}

// ??
// app ??icon_suffix ?? ic_app_home_theme{N}_{suffix}.spng ??// ?? suffix ???name ????????????????????// ?? "??" ??suffix ??"gps"??????????"map"????" ??
// ????????ic_app_home_themeN_magnet.spng ????????????// name ??zh-CN msgid??????????
// I18n::T(entry.name)??
constexpr AppEntry kApps[] = {
    {"wifi",           "网络配置", LaunchWifi,          wifi_lifecycle_cb,          false},
    {"digital_people", "数字人",   LaunchDigitalPeople, digital_people_lifecycle_cb, true},
#if !defined(BOARD_ESP_VOCAT)
    {"call",           "电话",     LaunchCall,          call_lifecycle_cb,          false},
#endif
    {"music",          "音乐",     LaunchMusic,         music_lifecycle_cb,         false},
    {"alarm",          "闹钟",     LaunchClock,         clock_lifecycle_cb,         false},
    {"album",          "相册",     LaunchAlbum,         album_lifecycle_cb,         false},
#if !defined(BOARD_ESP_VOCAT)
    {"gps",            "地图",     LaunchGps,           gps_lifecycle_cb,           true},
    {"spirit_level",   "水平仪",   LaunchLevel,         level_lifecycle_cb,         false},
    {"magnet",         "磁场",     LaunchMagnet,        magnet_lifecycle_cb,        false},
    {"vibrate",        "震动",     LaunchVibrate,       vibrate_lifecycle_cb,       false},
#endif
#if !defined(BOARD_ESP_SHOW)
    {"weather",        "天气",     LaunchWeather,       weather_lifecycle_cb,       true},
#endif
    {"sd",             "SD卡",     LaunchSdCard,        sd_card_lifecycle_cb,       false},
    {"badge",          "像章",     LaunchBadge,         badge_lifecycle_cb,         false},
    {"bagclip",        "背包扣",   LaunchBagclip,       bagclip_lifecycle_cb,       false},
#if !defined(BOARD_ESP_VOCAT)
    {"pin",            "引脚测试", LaunchPinTest,       pin_test_lifecycle_cb,      false},
#endif
    {"2048",           "游戏",     LaunchGame2048,      game_2048_lifecycle_cb,     false},
    {"info",           "信息",     LaunchInfo,          info_lifecycle_cb,          false},
#if !defined(BOARD_ESP_VOCAT)
    {"test",           "测试",     LaunchTest,          test_lifecycle_cb,          false},
#endif
    {"settings",       "设置",     LaunchSettings,      settings_lifecycle_cb,      false},
    // 暂时隐藏电台入口（radio_screen 源码仍保留，恢复时取消注释并加回 CMake）。
    // {"radio",          "电台",     LaunchRadio,         radio_lifecycle_cb,         true},
    {"recording",      "录音",     LaunchRecording,     recording_lifecycle_cb,     false},
};

constexpr int kTotalApps = static_cast<int>(sizeof(kApps) / sizeof(kApps[0]));

// 圆屏四叶瓣：空瓣底图 + 每页 4 App 叠图标/文字；中心固定「唤醒」。
constexpr AppEntry kWakeEntry = {
    nullptr, "唤醒", LaunchWake, nullptr, false};

constexpr int kCloverAppsPerPage = 4;

const AppEntry* FindAppBySuffix(const char* suffix) {
    if (suffix == nullptr) {
        return nullptr;
    }
    for (int i = 0; i < kTotalApps; ++i) {
        if (kApps[i].icon_suffix != nullptr &&
            std::strcmp(kApps[i].icon_suffix, suffix) == 0) {
            return &kApps[i];
        }
    }
    return nullptr;
}

void BuildCloverAppOrder(int* out_indices, int* out_count) {
    // 圆屏四叶瓣：slot 0=12点(上) 1=3点(右) 2=6点(下) 3=9点(左)，每页 4 个。
    // 第1页：数字人 / 相册 / 音乐 / 网络
    // 第2页：闹钟 / 录音 / 像章 / 背包扣
    // 第3页：游戏 / 设置 / SD卡 / 信息
    static const char* kOrder[] = {
        "digital_people",
        "album",
        "music",
        "wifi",
        "alarm",
        "recording",
        "badge",
        "bagclip",
        "2048",
        "settings",
        "sd",
        "info",
    };
    int n = 0;
    for (const char* suffix : kOrder) {
        const AppEntry* app = FindAppBySuffix(suffix);
        if (app == nullptr) {
            continue;
        }
        const int idx = static_cast<int>(app - kApps);
        if (idx < 0 || idx >= kTotalApps) {
            continue;
        }
        out_indices[n++] = idx;
    }
    *out_count = n;
}

// ????????
// app ????????LVGL ????
// HomeScreen::Create()
// ????
// EnsureIconPathsBuilt() ??????id ????????????//
// ????????
// lv_image ?????????lv_image_set_src ????
// ????????namespace ?????????????
constexpr int kIconPathBufSize = 56;
char s_icon_paths[kTotalApps][kIconPathBufSize];
char s_clover_icon_paths[kTotalApps][kIconPathBufSize];

// 主题 App 已移除，主屏图标固定使用 theme2 资源前缀。
constexpr int kHomeIconThemeId = 2;

void EnsureIconPathsBuilt() {
    static bool built = false;
    if (built) {
        return;
    }
    for (int i = 0; i < kTotalApps; ++i) {
        std::snprintf(s_icon_paths[i], kIconPathBufSize,
                      "A:ic_app_home_theme%d_%s.spng",
                      kHomeIconThemeId, kApps[i].icon_suffix);
        std::snprintf(s_clover_icon_paths[i], kIconPathBufSize,
                      "A:ic_clover_%s.spng", kApps[i].icon_suffix);
    }
    built = true;
    ESP_LOGI(TAG_HOME, "icon paths built for theme%d", kHomeIconThemeId);
}

// ---------------------------------------------------------------------------
// ????????????????????screen ??????????// ???? cell ??LV_EVENT_CLICKED??// ---------------------------------------------------------------------------

constexpr int kHomeMoveThreshold = 16;
constexpr int kHomeAxisLockThreshold = 20;
constexpr int kPageSnapThreshold = kPanelW / 5;
constexpr int kHomeFlickThreshold = 24;
constexpr uint32_t kHomeLongPressMs = 750;
constexpr uint32_t kPageSlideAnimMs = kLayoutRoundSmall ? 200 : 300;
// 圆屏四叶瓣：拖到这个距离就直接翻页，不等松手。
constexpr int kCloverSwipeTriggerPx = 44;

constexpr lv_obj_flag_t kAppCellFlag = LV_OBJ_FLAG_USER_2;

enum class HomeTouchKind {
    None,
    SwipeLeft,
    SwipeRight,
    SwipeUp,
    SwipeDown,
    Click,
    LongPress,
};

enum class HomeGestureAxis {
    None,
    Horizontal,
    Vertical,
};

struct HomeTouchSession {
    bool active = false;
    bool consumed = false;
    bool paging = false;
    bool page_flipped = false;  // 同一次触摸最多翻一页，避免长滑连跳
HomeGestureAxis axis = HomeGestureAxis::None;
    int16_t start_x = 0;
    int16_t start_y = 0;
    int16_t last_x = 0;
uint32_t press_tick = 0;
    lv_obj_t* press_cell = nullptr;
    const AppEntry* app = nullptr;
};
HomeTouchSession s_home_touch;

// ---------------------------------------------------------------------------
// ??????????idle_power_policy ??????????+ ???????// ---------------------------------------------------------------------------
void BeginSystemShutdown(const char* reason);

void ResetHomeIdleTimer() { IdlePower_NotifyActivity(); }

void StopHomeIdleTimer() { IdlePower_Detach(IdlePowerSession::Home); }

void StartHomeIdleTimer() {
    IdlePower_Attach(IdlePowerSession::Home, /*reset_activity=*/true);
}

// App ?????????????????????
constexpr uint32_t kCellPressScaleMs = 200;
const lv_style_prop_t kPressTransProps[] = {
    LV_STYLE_TRANSFORM_SCALE_X,
    LV_STYLE_TRANSFORM_SCALE_Y,
    LV_STYLE_PROP_INV,
};

lv_style_transition_dsc_t& GetPressTransition() {
    static lv_style_transition_dsc_t dsc;
    static bool inited = false;
    if (!inited) {
        lv_style_transition_dsc_init(&dsc, kPressTransProps, lv_anim_path_ease_out,
                                     kCellPressScaleMs, 0, nullptr);
        inited = true;
    }
    return dsc;
}

// ??
// false??
// WiFi ????????????????????
bool LaunchHomeApp(const AppEntry* app) {
    if (app == nullptr || app->launch == nullptr) {
        return false;
    }
    // ?? requires_wifi ?????????????????????????
if (app->requires_wifi && WifiRequired_ShouldBlock()) {
        ESP_LOGW(TAG_HOME, "block app '%s': WiFi not connected",
                 app->icon_suffix != nullptr ? app->icon_suffix : "?");
        WifiRequired_ShowDialog();
        return false;
    }
    app->launch(app->lifecycle_cb);
    return true;
}

struct CellScaleCtx {
    lv_obj_t* cell = nullptr;
    const AppEntry* launch_app = nullptr;
};

lv_timer_t* s_cell_scale_timer = nullptr;
CellScaleCtx s_cell_scale_ctx;

void CancelCellScaleTimer() {
    if (s_cell_scale_timer == nullptr) {
        return;
    }
    lv_timer_delete(s_cell_scale_timer);
    s_cell_scale_timer = nullptr;
    s_cell_scale_ctx = CellScaleCtx{};
}

void OnCellScaleTimer(lv_timer_t* timer) {
    const CellScaleCtx ctx = s_cell_scale_ctx;
    s_cell_scale_timer = nullptr;
    s_cell_scale_ctx = CellScaleCtx{};
    lv_timer_delete(timer);

    if (ctx.launch_app != nullptr) {
        if (!LaunchHomeApp(ctx.launch_app) && ctx.cell != nullptr) {
            lv_obj_remove_state(ctx.cell, LV_STATE_PRESSED);
        }
        return;
    }
    if (ctx.cell != nullptr) {
        lv_obj_remove_state(ctx.cell, LV_STATE_PRESSED);
    }
}

// launch_app??????app????kCellPressScaleMs ?? launch????
// nullptr????????
void PlayAppCellPressScale(lv_obj_t* cell, const AppEntry* launch_app) {
    if (cell == nullptr) {
        return;
    }
    CancelCellScaleTimer();
    lv_obj_add_state(cell, LV_STATE_PRESSED);
    s_cell_scale_ctx.cell = cell;
    s_cell_scale_ctx.launch_app = launch_app;
    s_cell_scale_timer =
        lv_timer_create(OnCellScaleTimer, kCellPressScaleMs, nullptr);
    lv_timer_set_repeat_count(s_cell_scale_timer, 1);
}

lv_obj_t* FindAppCellFromTarget(lv_obj_t* target, lv_obj_t* screen) {
    for (lv_obj_t* obj = target; obj != nullptr && obj != screen;
         obj = lv_obj_get_parent(obj)) {
        if (lv_obj_has_flag(obj, kAppCellFlag)) {
            return obj;
        }
    }
    return nullptr;
}

lv_obj_t* CreateAppCellSkeleton(lv_obj_t* cell) {
    // ???????????PPA ??????/???? msync ????fallback??    // ?? invalid addr?????????????????????????????
constexpr uint32_t kSkeletonBg = 0x2A2F3A;

    lv_obj_t* skeleton = lv_obj_create(cell);
    lv_obj_remove_style_all(skeleton);
    lv_obj_set_size(skeleton, kIconSize, kIconSize);
    lv_obj_set_style_bg_color(skeleton, lv_color_hex(kSkeletonBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(skeleton, LV_OPA_COVER, LV_PART_MAIN);
    // 与图标圆角大致一致，避免滑动时露出直角灰块
    lv_obj_set_style_radius(skeleton, kIconSize / 5, LV_PART_MAIN);
    lv_obj_set_style_border_width(skeleton, 0, LV_PART_MAIN);
    lv_obj_align(skeleton, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_flag(skeleton, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(skeleton, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_flag(skeleton, LV_OBJ_FLAG_HIDDEN);
    return skeleton;
}

lv_obj_t* CreateAppCell(lv_obj_t* parent, const AppEntry& entry, int idx) {
    lv_obj_t* cell = lv_obj_create(parent);
    lv_obj_remove_style_all(cell);
    lv_obj_set_size(cell, kCellWidth, kCellHeight);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(cell, 0, LV_PART_MAIN);

    // ?????????????????clip???????????
lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_radius(cell, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(cell, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(cell, 0, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(cell, false, LV_PART_MAIN);
    lv_obj_set_style_transition(cell, &GetPressTransition(), LV_PART_MAIN);

    // ?????????? screen ??
// Click / LongPress ??add_state(PRESSED)??
lv_obj_set_style_transform_pivot_x(cell, kCellWidth / 2,
                                       LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_transform_pivot_y(cell, kIconSize / 2,
                                       LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale(cell, 262, LV_PART_MAIN | LV_STATE_PRESSED);

    // 资源为 128x128 圆角 PNG，控件也用 128：不要 STRETCH。
    // S31 上对 ARGB 做 STRETCH 会出现叶片形/竖缝扭曲。
    lv_obj_t* icon = lv_image_create(cell);
    lv_image_set_src(icon, s_icon_paths[idx]);
    lv_obj_set_size(icon, kIconSize, kIconSize);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);

    // child[1]????????icon ????????
// SetPagerSkeletonMode??
CreateAppCellSkeleton(cell);

    lv_obj_t* name = lv_label_create(cell);
    lv_label_set_text(name, entry.name != nullptr ? I18n::T(entry.name) : "");
    lv_obj_set_width(name, kCellWidth);
    lv_label_set_long_mode(name, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(name, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(name, lv_color_hex(0xE5E7EB), LV_PART_MAIN);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, kIconSize + kNameGap);
    lv_obj_remove_flag(name, LV_OBJ_FLAG_CLICKABLE);

    if (entry.launch != nullptr) {
        // ??????PRESSED ????app????LV_EVENT_CLICKED?? screen ????
lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(cell, kAppCellFlag);
        lv_obj_set_user_data(cell, const_cast<AppEntry*>(&entry));
    }
    return cell;
}

// ---------------------------------------------------------------------------
// Pager + page-indicator
//
// Layout
//   pager (720 x kPagerHeight) -- horizontal scroll, snap to page center.
//     each child is a Page object (also 720 x kPagerHeight) which holds a
//     3x3 grid of cells.  We mark every page LV_OBJ_FLAG_SNAPPABLE so a
//     swipe ends with one page perfectly centered.
//
// State
//   PagerState lives on the heap and is owned by the screen via
//   LV_EVENT_DELETE.  The scroll callback uses it to map scroll position
//   -> current page and re-tint the dots.  No globals; if HomeScreen is
//   torn down and rebuilt, the new instance allocates a fresh state.
// ---------------------------------------------------------------------------

struct PagerState {
    lv_obj_t* pager;
    lv_obj_t* indicator = nullptr;
    lv_obj_t* dots[kMaxPages];
    int page_count;
    int current_page;
    bool skeleton_active = false;
    bool clover = false;  // 圆屏四叶瓣：底图固定，翻页淡入淡出
    lv_obj_t* clover_round = nullptr;
    lv_obj_t* clover_chrome = nullptr;
    lv_obj_t* clover_layer = nullptr;
    lv_obj_t* clover_icon[kCloverAppsPerPage] = {};
    lv_obj_t* clover_name[kCloverAppsPerPage] = {};
    lv_obj_t* clover_hs[kCloverAppsPerPage] = {};
    int clover_order[kTotalApps] = {};
    int clover_order_count = 0;
    int clover_pending_page = 0;
    int clover_step = 1;        // 本次翻页方向：+1 下一页，-1 上一页
    int clover_queued_step = 0; // 动画中又来手势时排队一步，避免丢手势
    bool clover_busy = false;
};

// ????????????????????????????????// ??????HomeScreen::Create() ??????????????????// ???????GoToPage?????HighlightDot ??????
// CreateIndicator ??????
// HighlightDot(state, 0) ???????
int s_last_home_page = 0;
PagerState* s_active_home_pager = nullptr;

void RememberHomePage(const PagerState* state) {
    if (state == nullptr || state->page_count <= 0) {
        return;
    }
    int page = state->current_page;
    if (state->clover && state->clover_busy) {
        page = state->clover_pending_page;
    }
    if (page < 0 || page >= state->page_count) {
        page = 0;
    }
    s_last_home_page = page;
}

int ClampHomePage(int page, int page_count) {
    if (page_count <= 0) {
        return 0;
    }
    if (page < 0 || page >= page_count) {
        return 0;
    }
    return page;
}

struct HomeStatusState {
    lv_obj_t* bar = nullptr;
    lv_obj_t* network_icon_lbl = nullptr;
    lv_obj_t* network_type_lbl = nullptr;
    lv_obj_t* sim_slot_lbl = nullptr;
lv_obj_t* battery_icon_lbl = nullptr;
lv_obj_t* battery_pct_lbl  = nullptr;
    lv_obj_t* time_lbl = nullptr;
    lv_obj_t* activation_overlay = nullptr;
    lv_obj_t* activation_title_lbl = nullptr;
    lv_obj_t* activation_code_lbl = nullptr;
    lv_timer_t* update_timer = nullptr;
    const char* last_icon = nullptr;
    const char* last_battery_icon = nullptr;
    int  last_battery_pct = -1;
    bool last_battery_low = false;
    bool last_battery_charging = false;
    bool last_battery_valid = false;
int  last_net_type = -1;
// SIM ??
    int  last_sim_slot = -1;
    std::string last_activation_text;
    bool last_activation_visible = false;
};

HomeStatusState* s_home_status = nullptr;

void SetPagerSkeletonMode(PagerState* state, bool active) {
    if (state == nullptr || state->pager == nullptr ||
        state->skeleton_active == active) {
        return;
    }
    if (state->clover) {
        return;
    }
    state->skeleton_active = active;

    // PPA ???? fill ?? msync ??????????????????    // ??cacheable ????????????????/??????
if (state->indicator != nullptr) {
        if (active) {
            lv_obj_add_flag(state->indicator, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(state->indicator, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_home_status != nullptr && s_home_status->bar != nullptr) {
        lv_obj_set_style_bg_opa(s_home_status->bar,
                                active ? LV_OPA_COVER : LV_OPA_50, LV_PART_MAIN);
    }

    const uint32_t page_child_count = lv_obj_get_child_count(state->pager);
    for (uint32_t p = 0; p < page_child_count; ++p) {
        lv_obj_t* page = lv_obj_get_child(state->pager, p);
        if (page == nullptr) {
            continue;
        }
        const uint32_t cell_count = lv_obj_get_child_count(page);
        for (uint32_t c = 0; c < cell_count; ++c) {
            lv_obj_t* cell = lv_obj_get_child(page, c);
            if (cell == nullptr || !lv_obj_has_flag(cell, kAppCellFlag)) {
                continue;
            }
            // child[0]=icon, [1]=skeleton, [2]=name
            lv_obj_t* icon = lv_obj_get_child(cell, 0);
            lv_obj_t* skeleton = lv_obj_get_child(cell, 1);
            lv_obj_t* name = lv_obj_get_child(cell, 2);
            if (icon == nullptr || skeleton == nullptr) {
                continue;
            }
            if (active) {
                lv_obj_remove_state(cell, LV_STATE_PRESSED);
                lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(skeleton, LV_OBJ_FLAG_HIDDEN);
                if (name != nullptr) {
                    lv_obj_add_flag(name, LV_OBJ_FLAG_HIDDEN);
                }
            } else {
                lv_obj_add_flag(skeleton, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(icon, LV_OBJ_FLAG_HIDDEN);
                if (name != nullptr) {
                    lv_obj_remove_flag(name, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
    }
}

void UpdateHomeStatusBar(HomeStatusState* st);

int GetSavedNetworkType() {
    // 禁止在 LVGL 线程里再开 NVS：栈可能在 PSRAM，且会与 ML307 HTTP 抢 Flash。
    // DualNetworkBoard 构造时已从 NVS 载入 network_type_。
    auto* dual = dynamic_cast<DualNetworkBoard*>(&Board::GetInstance());
    if (dual != nullptr) {
        return dual->GetNetworkType() == NetworkType::ML307 ? 1 : 0;
    }
#if defined(CONFIG_IDF_TARGET_ESP32S31)
    return 0;  // WiFi
#else
    return 1;  // 非 Dual 板默认按 4G 显示
#endif
}

// network_screen 与 boot SIM 查询共用；只在内存缓存，避免 LVGL 路径反复读 NVS。
int s_cached_sim_slot = -1;

int GetSavedSimSlot() {
    if (s_cached_sim_slot == 0 || s_cached_sim_slot == 1) {
        return s_cached_sim_slot;
    }
    // 禁止在 LVGL 路径懒加载 NVS；WarmStatusCaches() 应在 main_task 预载。
    return 0;
}

void WarmStatusCachesImpl() {
    Settings settings("network", false);
    int v = settings.GetInt("sim_slot", 0);
    s_cached_sim_slot = (v == 1) ? 1 : 0;
    IdlePower_WarmSettingsCache();
}

void SaveSimSlot(int slot) {
    const int v = (slot == 1) ? 1 : 0;
    s_cached_sim_slot = v;
    Settings settings("network", true);
    settings.SetInt("sim_slot", v);
}

Nt26Board* GetNt26Board() {
    auto& board = Board::GetInstance();
    auto* dual = dynamic_cast<DualNetworkBoard*>(&board);
    if (dual != nullptr) {
        return dynamic_cast<Nt26Board*>(&dual->GetCurrentBoard());
    }
    return dynamic_cast<Nt26Board*>(&board);
}

// ????????????SIM ??????NVS ????????
bool s_boot_sim_slot_query_done = false;

int ParseSimSlotFromEcsimcfg(const std::string& resp) {
    constexpr const char* kKey = "\"SimSlot\"";
    size_t pos = 0;
    while ((pos = resp.find(kKey, pos)) != std::string::npos) {
        size_t comma = resp.find(',', pos);
        if (comma == std::string::npos) {
            return -1;
        }
        size_t i = comma + 1;
        while (i < resp.size() && (resp[i] == ' ' || resp[i] == '\t')) {
            ++i;
        }
        if (i >= resp.size() ||
            !std::isdigit(static_cast<unsigned char>(resp[i]))) {
            pos = comma + 1;
            continue;
        }
        int slot = 0;
        while (i < resp.size() &&
               std::isdigit(static_cast<unsigned char>(resp[i]))) {
            slot = slot * 10 + (resp[i] - '0');
            ++i;
        }
        return slot;
    }
    return -1;
}

struct BootSimSlotQueryMsg {
    int slot = -1;
};

void AsyncBootSimSlotSynced(void* user_data) {
    auto* msg = static_cast<BootSimSlotQueryMsg*>(user_data);
    if (msg->slot >= 0 && GetSavedSimSlot() != msg->slot) {
        SaveSimSlot(msg->slot);
        ESP_LOGI(TAG_HOME, "sim_slot synced from modem at boot: %d", msg->slot);
    }
    if (s_home_status != nullptr) {
        s_home_status->last_sim_slot = -1;
        UpdateHomeStatusBar(s_home_status);
    }
    delete msg;
}

void BootSimSlotQueryTask(void* /*arg*/) {
    auto* msg = new BootSimSlotQueryMsg{};
    Nt26Board* nt26 = GetNt26Board();
    if (nt26 != nullptr) {
        std::string resp;
        esp_err_t err = nt26->SendAtCommand("AT+ECSIMCFG?", resp, 5000,
                                            /*bypass_init_check=*/true);
        ESP_LOGI(TAG_HOME, "boot AT+ECSIMCFG? -> err=%d resp_len=%u",
                 (int)err, (unsigned)resp.size());
        if (err == ESP_OK && resp.find("OK") != std::string::npos) {
            const int slot = ParseSimSlotFromEcsimcfg(resp);
            if (slot == 0 || slot == 1) {
                msg->slot = slot;
            } else {
                ESP_LOGW(TAG_HOME, "boot ECSIMCFG: SimSlot not parsed, resp='%s'",
                         resp.c_str());
            }
        }
    }
    screen_async_call(AsyncBootSimSlotSynced, msg);
    vTaskDelete(nullptr);
}

void ScheduleBootSimSlotQuery() {
    if (s_boot_sim_slot_query_done) {
        return;
    }
    if (GetSavedNetworkType() != 1) {
        return;
    }
    if (GetNt26Board() == nullptr) {
        return;
    }
    s_boot_sim_slot_query_done = true;
    if (xTaskCreate(BootSimSlotQueryTask, "home_sim_q", 4096, nullptr, 5,
                    nullptr) != pdPASS) {
        s_boot_sim_slot_query_done = false;
        ESP_LOGE(TAG_HOME, "xTaskCreate(home_sim_q) failed");
    }
}

void UpdateHomeStatusBar(HomeStatusState* st) {
    if (st == nullptr || st->bar == nullptr) {
        return;
    }

    const int net_type = GetSavedNetworkType();
    if (st->network_type_lbl != nullptr) {
        lv_label_set_text(st->network_type_lbl, net_type == 1 ? "4G" : "WiFi");
        lv_obj_remove_flag(st->network_type_lbl, LV_OBJ_FLAG_HIDDEN);
    }

    // SIM：仅双卡板在 4G 时显示内外置短标签；单卡板隐藏。
    if (st->sim_slot_lbl != nullptr) {
#if BOARD_HAS_DUAL_SIM
        if (net_type == 1) {
            const int slot = GetSavedSimSlot();
            if (st->last_net_type != net_type || st->last_sim_slot != slot) {
                if (kLayoutRoundSmall) {
                    lv_label_set_text(st->sim_slot_lbl, slot == 1 ? "外" : "内");
                } else {
                    lv_label_set_text(st->sim_slot_lbl,
                                      slot == 1 ? I18n::T("外置卡") : I18n::T("内置卡"));
                }
                st->last_sim_slot = slot;
            }
            lv_obj_remove_flag(st->sim_slot_lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            if (st->last_net_type != net_type) {
                lv_obj_add_flag(st->sim_slot_lbl, LV_OBJ_FLAG_HIDDEN);
            }
        }
#else
        lv_obj_add_flag(st->sim_slot_lbl, LV_OBJ_FLAG_HIDDEN);
        (void)net_type;
#endif
    }
    st->last_net_type = net_type;

    const char* icon = Board::GetInstance().GetNetworkStateIcon();
    if (icon != nullptr && st->network_icon_lbl != nullptr &&
        icon != st->last_icon) {
        st->last_icon = icon;
        lv_label_set_text(st->network_icon_lbl, icon);
    }

    // ---- ?? ----
#if HOME_STATUS_SHOW_BATTERY_ICON
    // 右上角使用紧凑的“图标 + 百分比”组合，避免电压等调试信息干扰主菜单。
    if (st->battery_icon_lbl != nullptr || st->battery_pct_lbl != nullptr) {
        int battery_level = 0;
        bool charging = false, discharging = false;
        if (Board::GetInstance().GetBatteryLevel(battery_level, charging, discharging)) {
            if (battery_level < 0)   battery_level = 0;
            if (battery_level > 100) battery_level = 100;

            const char* bat_icon = nullptr;
            if (charging) {
                bat_icon = FONT_AWESOME_BATTERY_BOLT;
            } else if (battery_level >= 80) {
                bat_icon = FONT_AWESOME_BATTERY_FULL;
            } else if (battery_level >= 60) {
                bat_icon = FONT_AWESOME_BATTERY_THREE_QUARTERS;
            } else if (battery_level >= 40) {
                bat_icon = FONT_AWESOME_BATTERY_HALF;
            } else if (battery_level >= 25) {
                bat_icon = FONT_AWESOME_BATTERY_QUARTER;
            } else {
                bat_icon = FONT_AWESOME_BATTERY_EMPTY;
            }
            if (st->battery_icon_lbl != nullptr &&
                bat_icon != st->last_battery_icon) {
                st->last_battery_icon = bat_icon;
                lv_label_set_text(st->battery_icon_lbl, bat_icon);
            }

            const bool low = !charging && battery_level < 25;
            if (st->battery_pct_lbl != nullptr &&
                st->last_battery_pct != battery_level) {
                char battery_text[8];
                std::snprintf(battery_text, sizeof(battery_text), "%d%%",
                              battery_level);
                lv_label_set_text(st->battery_pct_lbl, battery_text);
                st->last_battery_pct = battery_level;
            }

            const bool color_changed = !st->last_battery_valid ||
                                       low != st->last_battery_low ||
                                       charging != st->last_battery_charging;
            if (color_changed) {
                st->last_battery_low = low;
                st->last_battery_charging = charging;
                const uint32_t color = low ? 0xF87171
                                           : (charging ? 0x86EFAC : 0xFFFFFF);
                if (st->battery_icon_lbl != nullptr) {
                    lv_obj_set_style_text_color(st->battery_icon_lbl,
                                                lv_color_hex(color), LV_PART_MAIN);
                }
                if (st->battery_pct_lbl != nullptr) {
                    lv_obj_set_style_text_color(st->battery_pct_lbl,
                                                lv_color_hex(color), LV_PART_MAIN);
                }
            }
            st->last_battery_valid = true;
        } else {
            if (st->battery_icon_lbl != nullptr &&
                (st->last_battery_icon == nullptr || st->last_battery_valid)) {
                st->last_battery_icon = FONT_AWESOME_BATTERY_SLASH;
                lv_label_set_text(st->battery_icon_lbl,
                                  FONT_AWESOME_BATTERY_SLASH);
            }
            if (st->battery_pct_lbl != nullptr && st->last_battery_pct != -1) {
                st->last_battery_pct = -1;
                lv_label_set_text(st->battery_pct_lbl, "--%");
            }
            if (st->last_battery_valid || st->last_battery_low ||
                st->last_battery_charging) {
                st->last_battery_valid = false;
                st->last_battery_low = false;
                st->last_battery_charging = false;
                if (st->battery_icon_lbl != nullptr) {
                    lv_obj_set_style_text_color(st->battery_icon_lbl,
                                                lv_color_hex(0x9AA3B2), LV_PART_MAIN);
                }
                if (st->battery_pct_lbl != nullptr) {
                    lv_obj_set_style_text_color(st->battery_pct_lbl,
                                                lv_color_hex(0x9AA3B2), LV_PART_MAIN);
                }
            }
        }
    }
#else
    // ?????????? + ????+ ????????
    if (st->battery_pct_lbl != nullptr) {
        int battery_level = 0;
        bool charging = false, discharging = false;
        bool has_battery = Board::GetInstance().GetBatteryLevel(
            battery_level, charging, discharging);

        if (has_battery) {
            if (battery_level < 0)   battery_level = 0;
            if (battery_level > 100) battery_level = 100;

            const bool low = !charging && battery_level < 25;

            char buf[48];
            uint16_t dbg_mv = 0;
            char volt_str[16];
            if (Bq27220Gauge::GetInstance().GetVoltageMv(dbg_mv)) {
                std::snprintf(volt_str, sizeof(volt_str), "%.2fV",
                              dbg_mv / 1000.0f);
            } else {
                std::snprintf(volt_str, sizeof(volt_str), "--V");
            }
            if (charging) {
                std::snprintf(buf, sizeof(buf), I18n::T("电量 %d%% 充电中%s"), battery_level, volt_str);
            } else {
                std::snprintf(buf, sizeof(buf), I18n::T("电量 %d%% %s"), battery_level, volt_str);
            }
            lv_label_set_text(st->battery_pct_lbl, buf);
            st->last_battery_pct = battery_level;

            if (low != st->last_battery_low) {
                st->last_battery_low = low;
                uint32_t color = low ? 0xF87171 : 0xFFFFFF;
                lv_obj_set_style_text_color(st->battery_pct_lbl,
                                            lv_color_hex(color), LV_PART_MAIN);
            }
        } else {
            if (st->last_battery_pct != -1) {
                st->last_battery_pct = -1;
                // lv_label_set_text(st->battery_pct_lbl, I18n::T("?? --%"));
            }
            if (st->last_battery_low) {
                st->last_battery_low = false;
                lv_obj_set_style_text_color(st->battery_pct_lbl,
                                            lv_color_hex(0xFFFFFF), LV_PART_MAIN);
            }
        }
    }
#endif

    if (st->time_lbl != nullptr) {
        time_t now = time(nullptr);
        struct tm tm_info = {};
        if (localtime_r(&now, &tm_info) != nullptr &&
            tm_info.tm_year >= 2025 - 1900) {
            char time_str[16];
            strftime(time_str, sizeof(time_str), "%H:%M", &tm_info);
            lv_label_set_text(st->time_lbl, time_str);
        } else {
            lv_label_set_text(st->time_lbl, "--:--");
        }
    }

    if (st->activation_overlay != nullptr && st->activation_code_lbl != nullptr) {
        auto& app = Application::GetInstance();
        if (app.HasPendingActivation()) {
            const char* code = app.GetPendingActivationCode().c_str();
            if (st->last_activation_text != code) {
                st->last_activation_text = code;
                lv_label_set_text(st->activation_code_lbl, code);
            }
            if (!st->last_activation_visible) {
                st->last_activation_visible = true;
                lv_obj_remove_flag(st->activation_overlay, LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_move_foreground(st->activation_overlay);
        } else if (st->last_activation_visible) {
            st->last_activation_visible = false;
            st->last_activation_text.clear();
            lv_obj_add_flag(st->activation_overlay, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void OnHomeStatusTimer(lv_timer_t* timer) {
    UpdateHomeStatusBar(static_cast<HomeStatusState*>(lv_timer_get_user_data(timer)));
}

void OnHomeStatusDeleted(lv_event_t* e) {
    auto* st = static_cast<HomeStatusState*>(lv_event_get_user_data(e));
    if (st == nullptr) {
        return;
    }
    if (s_home_status == st) {
        s_home_status = nullptr;
    }
    if (st->update_timer != nullptr) {
        lv_timer_delete(st->update_timer);
        st->update_timer = nullptr;
    }
    delete st;
}

void CreateActivationOverlay(lv_obj_t* screen, HomeStatusState* st) {
    lv_obj_t* box = lv_obj_create(screen);
    st->activation_overlay = box;
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, kLayoutRoundSmall ? 200 : 420, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(box, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(box, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(box, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_radius(box, 16, LV_PART_MAIN);
    lv_obj_add_flag(box, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);

    st->activation_title_lbl = lv_label_create(box);
    lv_label_set_text(st->activation_title_lbl, I18n::T("请绑定设备"));
    lv_obj_set_style_text_font(st->activation_title_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->activation_title_lbl, lv_color_hex(0xFFFFFF),
                                LV_PART_MAIN);
    lv_obj_set_style_text_align(st->activation_title_lbl, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);

    st->activation_code_lbl = lv_label_create(box);
    lv_label_set_text(st->activation_code_lbl, "");
    lv_obj_set_style_text_font(st->activation_code_lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->activation_code_lbl, lv_color_hex(0xFBBF24),
                                LV_PART_MAIN);
    lv_obj_set_style_text_align(st->activation_code_lbl, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);

    UpdateHomeStatusBar(st);
}

lv_obj_t* CreateStatusBar(lv_obj_t* screen, HomeStatusState* st) {
    // 大屏：左网络 / 右电量宽区 + 居中时间。
    // 360 圆屏：左右收窄、加大水平边距；左侧保留 WiFi 图标+文字，右侧电量。
    constexpr int kStatusLeftWidth  = kLayoutRoundSmall ? 118 : 300;
    constexpr int kStatusRightWidth = kLayoutRoundSmall ? 0 : 120;
    constexpr int kStatusPadHor     = kLayoutRoundSmall ? 28 : 10;
    constexpr int kStatusPadVer     = kLayoutRoundSmall ? 4 : 8;
    constexpr int kStatusNetworkShiftX = kLayoutRoundSmall ? 8 : 0;

    lv_obj_t* bar = lv_obj_create(screen);
    st->bar = bar;
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, kPanelW, kStatusBarHeight);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kStatusBarBg), LV_PART_MAIN);
    // 四叶瓣首页：状态栏浮在图上，背景接近透明，避免挡瓣区观感
    lv_obj_set_style_bg_opa(bar, kLayoutRoundSmall ? LV_OPA_TRANSP : LV_OPA_50,
                            LV_PART_MAIN);
    if (kLayoutRoundSmall) {
        lv_obj_add_flag(bar, LV_OBJ_FLAG_FLOATING);
    }
    lv_obj_set_style_pad_hor(bar, kStatusPadHor, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(bar, kStatusPadVer, LV_PART_MAIN);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t* left = lv_obj_create(bar);
    lv_obj_remove_style_all(left);
    lv_obj_set_size(left, kStatusLeftWidth, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(left, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left, kLayoutRoundSmall ? 4 : 10, LV_PART_MAIN);
    if (kStatusNetworkShiftX != 0) {
        lv_obj_set_style_margin_left(left, kStatusNetworkShiftX, LV_PART_MAIN);
    }

    st->network_icon_lbl = lv_label_create(left);
    lv_label_set_text(st->network_icon_lbl, FONT_AWESOME_WIFI);
    lv_obj_set_style_text_font(st->network_icon_lbl, &font_awesome_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->network_icon_lbl, lv_color_hex(0xFFFFFF),
                                LV_PART_MAIN);

    st->network_type_lbl = lv_label_create(left);
    lv_label_set_long_mode(st->network_type_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(st->network_type_lbl, LV_SIZE_CONTENT);
    lv_label_set_text(st->network_type_lbl, "WiFi");
    lv_obj_set_style_text_font(st->network_type_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->network_type_lbl, lv_color_hex(0xFFFFFF),
                                LV_PART_MAIN);

    // SIM：圆屏放在 WiFi 文字旁（4G 时显示），空间不够时 Update 里再裁
    st->sim_slot_lbl = lv_label_create(left);
    lv_label_set_long_mode(st->sim_slot_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(st->sim_slot_lbl, LV_SIZE_CONTENT);
    lv_label_set_text(st->sim_slot_lbl, "");
    lv_obj_set_style_text_font(st->sim_slot_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->sim_slot_lbl, lv_color_hex(0xC9D1D9),
                                LV_PART_MAIN);
    lv_obj_add_flag(st->sim_slot_lbl, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* right = lv_obj_create(bar);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, kStatusRightWidth, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right, kLayoutRoundSmall ? 5 : 8, LV_PART_MAIN);
#if HOME_STATUS_SHOW_BATTERY_ICON
    if (!kLayoutRoundSmall) {
        st->battery_icon_lbl = lv_label_create(right);
        lv_label_set_text(st->battery_icon_lbl, FONT_AWESOME_BATTERY_FULL);
        lv_obj_set_style_text_font(st->battery_icon_lbl, &font_awesome_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(st->battery_icon_lbl, lv_color_hex(0xFFFFFF),
                                    LV_PART_MAIN);
        st->battery_pct_lbl = lv_label_create(right);
        lv_label_set_long_mode(st->battery_pct_lbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(st->battery_pct_lbl, 54);
        lv_obj_set_style_text_align(st->battery_pct_lbl, LV_TEXT_ALIGN_RIGHT,
                                    LV_PART_MAIN);
        lv_label_set_text(st->battery_pct_lbl, "--%");
        lv_obj_set_style_text_font(st->battery_pct_lbl, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(st->battery_pct_lbl, lv_color_hex(0xFFFFFF),
                                    LV_PART_MAIN);
    }
#else
    if (!kLayoutRoundSmall) {
        st->battery_pct_lbl = lv_label_create(right);
        lv_label_set_long_mode(st->battery_pct_lbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(st->battery_pct_lbl, 380);
        lv_obj_set_style_text_align(st->battery_pct_lbl, LV_TEXT_ALIGN_RIGHT,
                                    LV_PART_MAIN);
        lv_obj_set_style_text_font(st->battery_pct_lbl, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(st->battery_pct_lbl, lv_color_hex(0xFFFFFF),
                                    LV_PART_MAIN);
    }
#endif

    lv_obj_t* center = lv_obj_create(bar);
    lv_obj_add_flag(center, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_style_all(center);
    lv_obj_set_size(center, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(center, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(center, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(center, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(center, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(center, kLayoutRoundSmall ? 4 : 8, LV_PART_MAIN);
    lv_obj_align(center, LV_ALIGN_CENTER, 0, 0);

    st->time_lbl = lv_label_create(center);
    lv_label_set_long_mode(st->time_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(st->time_lbl, kLayoutRoundSmall ? 56 : 80);
    lv_label_set_text(st->time_lbl, "--:--");
    lv_obj_set_style_text_align(st->time_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(st->time_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->time_lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);

    UpdateHomeStatusBar(st);
    st->update_timer = lv_timer_create(OnHomeStatusTimer, 1000, st);
    s_home_status = st;
    ScheduleBootSimSlotQuery();

    lv_obj_add_event_cb(screen, OnHomeStatusDeleted, LV_EVENT_DELETE, st);
    return bar;
}

void HighlightDot(PagerState* state, int page) {
    if (page < 0 || page >= state->page_count) {
        return;
    }
    state->current_page = page;
    for (int i = 0; i < state->page_count; ++i) {
        // Active dot is fully opaque, idle dots are subtle.  We keep both
        // the color the same so the row reads as a connected element.
        lv_opa_t opa = (i == page) ? LV_OPA_COVER : LV_OPA_40;
        lv_obj_set_style_bg_opa(state->dots[i], opa, LV_PART_MAIN);
    }
}

// 翻页动效只动 4 个图标 + 4 条文字（image_opa / text_opa / translate_x）。
// 不能给 clover_layer 整体设 opa：容器带子对象且 opa < COVER 时 LVGL 会渲染到
// intermediate layer，而 LV_DRAW_LAYER_SIMPLE_BUF_SIZE 只有 24KB，360×360 要切
// 二十多刀重绘，一帧都刷不完，手感就是"顿"。
// 时长按 LV_DEF_REFR_PERIOD=33ms 取整数帧数，避免最后一帧被截断。
// 图标/底图已常驻 PSRAM，翻页每帧只是 blit，淡入淡出不会再一块一块冒出来。
constexpr uint32_t kCloverFadeOutMs = 165;  // 5 帧
constexpr uint32_t kCloverFadeInMs = 198;   // 6 帧
constexpr int32_t kCloverSlidePx = 36;      // 位移量，给出方向感又不至于扩太多重绘区

const char* CloverDisplayName(const AppEntry* entry);
bool PagerLoopEnabled(const PagerState* state);
void CloverGoToPage(PagerState* state, int target_page);

// 四叶瓣图标由 tools/redraw_clover_icons.py 按显示尺寸生成（圆屏 80×80，1:1 不缩放）。
// 大屏若需原生清晰度：python3 tools/redraw_clover_icons.py --size 112
constexpr int kCloverIconFrame = kLayoutRoundSmall ? 80 : 112;

// xingzhi-assets 打包用 MMAP_SPLIT_HEIGHT=16，.spng 是切片格式，esp_lv_decoder 对切片图
// 强制 no_cache：每次重绘都要按 16 行重新 inflate（80×80 图标 5 片，360×360 底图 23 片）。
// 翻页有 8 个失效区，每个区都会把压在下面的底图切片和自己的图标重解一遍，解码穿插在各块
// flush 之间，看着就是图标一个接一个冒出来。这里各解一次常驻 PSRAM，翻页只剩 blit。
// 底图按黑底压平成不透明 RGB565（屏幕底色就是黑），省一半内存，也省掉逐像素 alpha 混合。
constexpr const char* kCloverChromePath = "A:home_clover_chrome.spng";
constexpr size_t kCloverRamBudget = 1200u * 1024u;
size_t s_clover_ram_used = 0;

lv_image_dsc_t s_clover_icon_dsc[kTotalApps];
bool s_clover_icon_ready[kTotalApps];
bool s_clover_icon_tried[kTotalApps];
lv_image_dsc_t s_clover_chrome_dsc;
bool s_clover_chrome_ready = false;

// 把 A: 盘一张图整幅画到常驻 PSRAM。切片图没法一次拿到整幅缓冲，借个离屏 canvas 走正常
// 绘制流程最省事。opaque_on_black：直接铺在黑底上存成不透明 RGB565（底图用，省一半内存、
// 画的时候也不用混合）；否则存 ARGB8888（图标要透明地压在底图上）。
bool DecodeAssetToRam(const char* path, bool opaque_on_black, lv_image_dsc_t* out) {
    if (path == nullptr || out == nullptr) {
        return false;
    }
    lv_image_header_t header;
    if (lv_image_decoder_get_info(path, &header) != LV_RESULT_OK) {
        return false;
    }
    const int32_t w = static_cast<int32_t>(header.w);
    const int32_t h = static_cast<int32_t>(header.h);
    const lv_color_format_t cf =
        opaque_on_black ? LV_COLOR_FORMAT_RGB565 : LV_COLOR_FORMAT_ARGB8888;
    const size_t px = opaque_on_black ? 2u : 4u;
    const size_t bytes = static_cast<size_t>(w) * h * px;
    if (w <= 0 || h <= 0 || s_clover_ram_used + bytes > kCloverRamBudget) {
        return false;
    }
    auto* buf = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
    if (buf == nullptr) {
        return false;
    }
    // 全 0 起手：RGB565 就是黑底，ARGB8888 就是全透明。
    std::memset(buf, 0, bytes);

    lv_obj_t* holder = lv_obj_create(nullptr);
    lv_obj_t* canvas = (holder != nullptr) ? lv_canvas_create(holder) : nullptr;
    if (canvas == nullptr) {
        if (holder != nullptr) {
            lv_obj_delete(holder);
        }
        heap_caps_free(buf);
        return false;
    }
    lv_canvas_set_buffer(canvas, buf, w, h, cf);

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);
    lv_draw_image_dsc_t img;
    lv_draw_image_dsc_init(&img);
    img.src = path;
    lv_area_t area = {0, 0, w - 1, h - 1};
    lv_draw_image(&layer, &img, &area);
    lv_canvas_finish_layer(canvas, &layer);
    // canvas 只是借用这块内存，销毁时不会释放。
    lv_obj_delete(holder);

    std::memset(out, 0, sizeof(*out));
    out->header.magic = LV_IMAGE_HEADER_MAGIC;
    out->header.cf = cf;
    out->header.w = static_cast<uint32_t>(w);
    out->header.h = static_cast<uint32_t>(h);
    out->header.stride = static_cast<uint32_t>(w) * px;
    out->data_size = static_cast<uint32_t>(bytes);
    out->data = buf;
    s_clover_ram_used += bytes;
    return true;
}

// 解不出来就退回文件路径，最差也只是回到原来的表现。
const void* CloverIconSrc(int idx) {
    if (idx < 0 || idx >= kTotalApps) {
        return nullptr;
    }
    if (!s_clover_icon_tried[idx]) {
        s_clover_icon_tried[idx] = true;
        s_clover_icon_ready[idx] = DecodeAssetToRam(
            s_clover_icon_paths[idx], false, &s_clover_icon_dsc[idx]);
    }
    if (s_clover_icon_ready[idx]) {
        return &s_clover_icon_dsc[idx];
    }
    return s_clover_icon_paths[idx];
}

const void* CloverChromeSrc() {
    static bool tried = false;
    if (!tried) {
        tried = true;
        s_clover_chrome_ready =
            DecodeAssetToRam(kCloverChromePath, true, &s_clover_chrome_dsc);
        ESP_LOGI(TAG_HOME, "clover chrome ram=%d used=%uKB",
                 s_clover_chrome_ready ? 1 : 0,
                 static_cast<unsigned>(s_clover_ram_used / 1024));
    }
    if (s_clover_chrome_ready) {
        return &s_clover_chrome_dsc;
    }
    return kCloverChromePath;
}

// 后面几页的图标在空闲时预解，一次 tick 只解一张，别把 LVGL 堵住。
int s_clover_prefetch[kTotalApps];
int s_clover_prefetch_count = 0;
int s_clover_prefetch_pos = 0;
lv_timer_t* s_clover_prefetch_timer = nullptr;

void CloverPrefetchCb(lv_timer_t* timer) {
    while (s_clover_prefetch_pos < s_clover_prefetch_count) {
        const int idx = s_clover_prefetch[s_clover_prefetch_pos++];
        if (idx >= 0 && idx < kTotalApps && !s_clover_icon_tried[idx]) {
            CloverIconSrc(idx);
            return;
        }
    }
    ESP_LOGI(TAG_HOME, "clover icons cached, ram=%uKB",
             static_cast<unsigned>(s_clover_ram_used / 1024));
    lv_timer_delete(timer);
    s_clover_prefetch_timer = nullptr;
}

void StartCloverPrefetch(const int* order, int count) {
    if (s_clover_prefetch_timer != nullptr || order == nullptr || count <= 0) {
        return;
    }
    s_clover_prefetch_count = (count > kTotalApps) ? kTotalApps : count;
    for (int i = 0; i < s_clover_prefetch_count; ++i) {
        s_clover_prefetch[i] = order[i];
    }
    s_clover_prefetch_pos = 0;
    s_clover_prefetch_timer = lv_timer_create(CloverPrefetchCb, 40, nullptr);
}

void SetupCloverIcon(lv_obj_t* icon, lv_coord_t center_x, lv_coord_t center_y) {
    if (icon == nullptr) {
        return;
    }
    lv_obj_set_size(icon, kCloverIconFrame, kCloverIconFrame);
    lv_obj_set_pos(icon, center_x - kCloverIconFrame / 2,
                   center_y - kCloverIconFrame / 2);
    // 资源边长与 kCloverIconFrame 一致时按原图像素绘制。
    lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CENTER);
    lv_image_set_antialias(icon, true);
    lv_obj_add_flag(icon, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_image_opa(icon, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
}

void CloverApplyPage(PagerState* state, int page) {
    if (state == nullptr || !state->clover) {
        return;
    }
    if (page < 0 || page >= state->page_count) {
        return;
    }

    for (int s = 0; s < kCloverAppsPerPage; ++s) {
        const int oi = page * kCloverAppsPerPage + s;
        const int idx =
            (oi < state->clover_order_count) ? state->clover_order[oi] : -1;
        lv_obj_t* icon = state->clover_icon[s];
        lv_obj_t* name = state->clover_name[s];
        lv_obj_t* hs = state->clover_hs[s];
        if (idx < 0 || idx >= kTotalApps) {
            if (icon != nullptr) {
                lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
            }
            if (name != nullptr) {
                lv_label_set_text(name, "");
                lv_obj_add_flag(name, LV_OBJ_FLAG_HIDDEN);
            }
            if (hs != nullptr) {
                lv_obj_add_flag(hs, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_user_data(hs, nullptr);
            }
            continue;
        }
        const AppEntry& app = kApps[idx];
        if (icon != nullptr) {
            lv_image_set_src(icon, CloverIconSrc(idx));
            lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CENTER);
            lv_obj_remove_flag(icon, LV_OBJ_FLAG_HIDDEN);
        }
        if (name != nullptr) {
            lv_label_set_text(name, I18n::T(CloverDisplayName(&app)));
            lv_obj_remove_flag(name, LV_OBJ_FLAG_HIDDEN);
        }
        if (hs != nullptr) {
            lv_obj_set_user_data(hs, const_cast<AppEntry*>(&app));
            lv_obj_remove_flag(hs, LV_OBJ_FLAG_HIDDEN);
        }
    }
    state->current_page = page;
    s_last_home_page = page;
}

// opa/位移一起改：一次遍历只碰 8 个对象，重绘区就是图标 + 文字那几块。
void CloverSetTransition(PagerState* state, lv_opa_t opa, int32_t shift) {
    if (state == nullptr) {
        return;
    }
    for (int s = 0; s < kCloverAppsPerPage; ++s) {
        lv_obj_t* icon = state->clover_icon[s];
        if (icon != nullptr) {
            lv_obj_set_style_image_opa(icon, opa, LV_PART_MAIN);
            lv_obj_set_style_translate_x(icon, shift, LV_PART_MAIN);
        }
        lv_obj_t* name = state->clover_name[s];
        if (name != nullptr) {
            lv_obj_set_style_text_opa(name, opa, LV_PART_MAIN);
            lv_obj_set_style_translate_x(name, shift, LV_PART_MAIN);
        }
    }
}

void CloverResetTransition(PagerState* state) {
    CloverSetTransition(state, LV_OPA_COVER, 0);
}

// 动画进度统一用 0..256，避免除法丢精度。
constexpr int32_t kCloverAnimSpan = 256;

void CloverAnimOutExec(void* var, int32_t v) {
    auto* state = static_cast<PagerState*>(var);
    if (state == nullptr) {
        return;
    }
    const int32_t opa = LV_OPA_COVER - LV_OPA_COVER * v / kCloverAnimSpan;
    const int32_t shift =
        -state->clover_step * kCloverSlidePx * v / kCloverAnimSpan;
    CloverSetTransition(state, static_cast<lv_opa_t>(opa), shift);
}

void CloverAnimInExec(void* var, int32_t v) {
    auto* state = static_cast<PagerState*>(var);
    if (state == nullptr) {
        return;
    }
    const int32_t opa = LV_OPA_COVER * v / kCloverAnimSpan;
    const int32_t shift = state->clover_step * kCloverSlidePx *
                          (kCloverAnimSpan - v) / kCloverAnimSpan;
    CloverSetTransition(state, static_cast<lv_opa_t>(opa), shift);
}

void CloverFadeInReady(lv_anim_t* a) {
    auto* state = static_cast<PagerState*>(lv_anim_get_user_data(a));
    if (state == nullptr) {
        return;
    }
    CloverResetTransition(state);
    state->clover_busy = false;

    const int queued = state->clover_queued_step;
    state->clover_queued_step = 0;
    if (queued != 0) {
        CloverGoToPage(state, state->current_page + queued);
    }
}

void CloverStartFadeIn(PagerState* state) {
    if (state == nullptr) {
        return;
    }
    lv_anim_t fade_in;
    lv_anim_init(&fade_in);
    lv_anim_set_var(&fade_in, state);
    lv_anim_set_user_data(&fade_in, state);
    lv_anim_set_exec_cb(&fade_in, CloverAnimInExec);
    lv_anim_set_values(&fade_in, 0, kCloverAnimSpan);
    lv_anim_set_duration(&fade_in, kCloverFadeInMs);
    lv_anim_set_path_cb(&fade_in, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&fade_in, CloverFadeInReady);
    lv_anim_start(&fade_in);
}

void CloverFadeOutReady(lv_anim_t* a) {
    auto* state = static_cast<PagerState*>(lv_anim_get_user_data(a));
    if (state == nullptr) {
        return;
    }
    // 旧页已淡到全透明，换图后再从对侧滑入。
    CloverApplyPage(state, state->clover_pending_page);
    HighlightDot(state, state->clover_pending_page);
    CloverSetTransition(state, LV_OPA_TRANSP,
                        state->clover_step * kCloverSlidePx);
    CloverStartFadeIn(state);
}

void CloverGoToPage(PagerState* state, int target_page) {
    if (state == nullptr || !state->clover || state->page_count <= 1) {
        return;
    }
    // 调用方只会传 current_page ± 1，方向比绝对页号更好用（要处理首尾环绕）。
    const int delta = target_page - state->current_page;
    if (delta == 0) {
        return;
    }
    const int step = delta > 0 ? 1 : -1;

    if (state->clover_busy) {
        // 同一次滑动手势内不排队连翻，避免一次滑动跳多页。
        if (s_home_touch.active && s_home_touch.page_flipped) {
            return;
        }
        state->clover_queued_step = step;
        ResetHomeIdleTimer();
        return;
    }

    int next = state->current_page + step;
    if (next < 0 || next >= state->page_count) {
        if (!PagerLoopEnabled(state)) {
            return;
        }
        next = (next % state->page_count + state->page_count) %
               state->page_count;
    }

    state->clover_busy = true;
    state->clover_step = step;
    state->clover_pending_page = next;
    ResetHomeIdleTimer();

    lv_anim_t fade_out;
    lv_anim_init(&fade_out);
    lv_anim_set_var(&fade_out, state);
    lv_anim_set_user_data(&fade_out, state);
    lv_anim_set_exec_cb(&fade_out, CloverAnimOutExec);
    lv_anim_set_values(&fade_out, 0, kCloverAnimSpan);
    lv_anim_set_duration(&fade_out, kCloverFadeOutMs);
    lv_anim_set_path_cb(&fade_out, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&fade_out, CloverFadeOutReady);
    lv_anim_start(&fade_out);
}

bool PagerLoopEnabled(const PagerState* state) {
    return state != nullptr && state->page_count > 1;
}

// ???????pager ?????????
// [????][???0..N-1][????]???? i ? scroll_x = (i+1)*kPanelW?
int32_t PagerScrollXForPage(const PagerState* state, int logical_page) {
    if (state == nullptr) {
        return 0;
    }
    const int physical = PagerLoopEnabled(state) ? logical_page + 1 : logical_page;
    return static_cast<int32_t>(physical) * kPanelW;
}

void PagerMaybeWrapAfterScroll(PagerState* state) {
    if (!PagerLoopEnabled(state) || state->pager == nullptr) {
        return;
    }
    const int32_t scroll_x = lv_obj_get_scroll_x(state->pager);
    if (scroll_x == 0) {
        const int last = state->page_count - 1;
        lv_obj_scroll_to_x(state->pager, PagerScrollXForPage(state, last),
                           LV_ANIM_OFF);
        HighlightDot(state, last);
        s_last_home_page = last;
    } else if (scroll_x == static_cast<int32_t>(state->page_count + 1) * kPanelW) {
        lv_obj_scroll_to_x(state->pager, PagerScrollXForPage(state, 0),
                           LV_ANIM_OFF);
        HighlightDot(state, 0);
        s_last_home_page = 0;
    }
}

void OnPagerScrollBegin(lv_event_t* e) {
    lv_anim_t* a = lv_event_get_scroll_anim(e);
    if (a == nullptr) {
        return;
    }
    lv_anim_set_duration(a, kPageSlideAnimMs);
    lv_anim_set_path_cb(a, lv_anim_path_ease_out);
}

void OnPagerScrollEnd(lv_event_t* e) {
    auto* state = static_cast<PagerState*>(lv_event_get_user_data(e));
    if (state == nullptr || state->pager == nullptr) {
        return;
    }
    // ??????scroll_by(LV_ANIM_OFF) ??????
// SCROLL_END??    // ??????????????????????????
if (s_home_touch.active && s_home_touch.paging) {
        return;
    }
    if (!lv_obj_is_scrolling(state->pager)) {
        PagerMaybeWrapAfterScroll(state);
        SetPagerSkeletonMode(state, false);
    }
}

// ????????????????????????????????
void GoToPage(PagerState* state, int target_page) {
    if (state == nullptr) {
        return;
    }
    if (state->clover) {
        CloverGoToPage(state, target_page);
        return;
    }
    if (state->pager == nullptr) {
        return;
    }
    if (target_page < 0 || target_page >= state->page_count) {
        if (!PagerLoopEnabled(state)) {
            return;
        }
        target_page =
            (target_page % state->page_count + state->page_count) % state->page_count;
    }

    const int current = state->current_page;
    int32_t target_x = PagerScrollXForPage(state, target_page);
    if (PagerLoopEnabled(state)) {
        if (target_page == 0 && current == state->page_count - 1) {
            target_x = static_cast<int32_t>(state->page_count + 1) * kPanelW;
        } else if (target_page == state->page_count - 1 && current == 0) {
            target_x = 0;
        }
    }

    const int32_t scroll_x = lv_obj_get_scroll_x(state->pager);
    if (target_page == current && scroll_x == target_x) {
        SetPagerSkeletonMode(state, false);
        return;
    }
    SetPagerSkeletonMode(state, true);
    lv_obj_scroll_to_x(state->pager, target_x, LV_ANIM_ON);
    HighlightDot(state, target_page);
    s_last_home_page = target_page;
    ResetHomeIdleTimer();
}

void SnapPagerToNearestPage(PagerState* state, int release_dx) {
    if (state == nullptr || state->pager == nullptr) {
        return;
    }
    const int32_t scroll_x = lv_obj_get_scroll_x(state->pager);
    const int anchor_x = PagerScrollXForPage(state, state->current_page);
    const int delta = static_cast<int>(scroll_x) - anchor_x;

    int target = state->current_page;
    // ???????????? delta>0 ??????? delta<0 ??????
if (delta > kPageSnapThreshold ||
        (release_dx <= -kHomeFlickThreshold && delta > kHomeMoveThreshold)) {
        target = state->current_page + 1;
    } else if (delta < -kPageSnapThreshold ||
               (release_dx >= kHomeFlickThreshold && delta < -kHomeMoveThreshold)) {
        target = state->current_page - 1;
    }

    if (target < 0) {
        if (PagerLoopEnabled(state) && state->current_page == 0) {
            GoToPage(state, state->page_count - 1);
            return;
        }
        target = 0;
    }
    if (target >= state->page_count) {
        if (PagerLoopEnabled(state) &&
            state->current_page == state->page_count - 1) {
            GoToPage(state, 0);
            return;
        }
        target = state->page_count - 1;
    }
    GoToPage(state, target);
}

// ---------------------------------------------------------------------------
// ?????PRESSED ???? ??PRESSING/RELEASED ??????RELEASED ??????
//
//   |dx|?|dy| ??< kHomeMoveThreshold??//     ????? < kHomeLongPressMs ??Click????+ launch??//     ????? ??kHomeLongPressMs ??LongPress????????
//   |dx| >= |dy| ?????? >= kHomeAxisLockThreshold ??????PRESSING ?? / RELEASED ????//   |dy| > |dx| ?????? >= kHomeAxisLockThreshold ????????????????//   ???????????? ???????????
// ---------------------------------------------------------------------------

bool HomeTouchIsHorizontalSlide(int dx, int dy) {
    const int adx = std::abs(dx);
    const int ady = std::abs(dy);
    return adx >= kHomeAxisLockThreshold && adx * 2 > ady * 3;
}

bool HomeTouchIsVerticalSlide(int dx, int dy) {
    const int adx = std::abs(dx);
    const int ady = std::abs(dy);
    return ady >= kHomeAxisLockThreshold && ady * 2 > adx * 3;
}

void HomeTouchUpdateAxisLock(int dx, int dy) {
    if (s_home_touch.axis != HomeGestureAxis::None) {
        return;
    }
    if (HomeTouchIsHorizontalSlide(dx, dy)) {
        s_home_touch.axis = HomeGestureAxis::Horizontal;
        return;
    }
    if (HomeTouchIsVerticalSlide(dx, dy)) {
        s_home_touch.axis = HomeGestureAxis::Vertical;
        s_home_touch.consumed = true;
    }
}

bool HomeTouchIsTapLike(int dx, int dy) {
    return std::abs(dx) < kHomeMoveThreshold && std::abs(dy) < kHomeMoveThreshold;
}

HomeTouchKind HomeTouchClassifySwipe(int dx, int dy) {
    if (HomeTouchIsTapLike(dx, dy)) {
        return HomeTouchKind::None;
    }
    if (HomeTouchIsHorizontalSlide(dx, dy)) {
        return dx < 0 ? HomeTouchKind::SwipeLeft : HomeTouchKind::SwipeRight;
    }
    if (HomeTouchIsVerticalSlide(dx, dy)) {
        return dy < 0 ? HomeTouchKind::SwipeUp : HomeTouchKind::SwipeDown;
    }
    return HomeTouchKind::None;
}

void HomeTouchHandleSwipe(PagerState* state, HomeTouchKind kind) {
    if (state == nullptr) {
        return;
    }
    switch (kind) {
        case HomeTouchKind::SwipeLeft:
            GoToPage(state, state->current_page + 1);
            break;
        case HomeTouchKind::SwipeRight:
            GoToPage(state, state->current_page - 1);
            break;
        case HomeTouchKind::SwipeUp:
        case HomeTouchKind::SwipeDown:
        case HomeTouchKind::None:
        case HomeTouchKind::Click:
        case HomeTouchKind::LongPress:
            break;
    }
}

void HomeTouchDispatchTapLike(HomeTouchKind kind) {
    if (s_home_touch.press_cell == nullptr) {
        return;
    }
    if (kind == HomeTouchKind::Click) {
        if (s_home_touch.app == nullptr) {
            return;
        }
        PlayAppCellPressScale(s_home_touch.press_cell, s_home_touch.app);
        return;
    }
    if (kind == HomeTouchKind::LongPress) {
        PlayAppCellPressScale(s_home_touch.press_cell, nullptr);
    }
}

void HomeTouchTryStartPageDrag(PagerState* state, int dx, int dy, int current_x) {
    if (s_home_touch.axis != HomeGestureAxis::Horizontal) {
        return;
    }
    if (s_home_touch.consumed && !s_home_touch.paging) {
        return;
    }
    if (!HomeTouchIsHorizontalSlide(dx, dy)) {
        return;
    }
    if (!s_home_touch.paging) {
        s_home_touch.paging = true;
        s_home_touch.consumed = true;
        if (state != nullptr && state->pager != nullptr) {
            lv_obj_stop_scroll_anim(state->pager);
            SetPagerSkeletonMode(state, true);
        }
        s_home_touch.last_x = static_cast<int16_t>(current_x);
    }
}

void OnHomePressed(lv_event_t* e) {
    if (s_home_touch.active) {
        return;
    }
    lv_indev_t* indev = lv_event_get_indev(e);
    if (indev == nullptr) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    lv_obj_t* screen = lv_event_get_current_target_obj(e);
    lv_obj_t* cell = FindAppCellFromTarget(lv_event_get_target_obj(e), screen);

    auto* state = static_cast<PagerState*>(lv_event_get_user_data(e));
    if (state != nullptr && state->clover) {
        state->clover_queued_step = 0;
    }

    s_home_touch.active = true;
    s_home_touch.consumed = false;
    s_home_touch.paging = false;
    s_home_touch.page_flipped = false;
    s_home_touch.axis = HomeGestureAxis::None;
    s_home_touch.start_x = static_cast<int16_t>(p.x);
    s_home_touch.start_y = static_cast<int16_t>(p.y);
    s_home_touch.last_x = static_cast<int16_t>(p.x);
    s_home_touch.press_tick = lv_tick_get();
    s_home_touch.press_cell = cell;
    s_home_touch.app =
        cell != nullptr
            ? static_cast<const AppEntry*>(lv_obj_get_user_data(cell))
            : nullptr;
    ResetHomeIdleTimer();
}

void OnHomePressing(lv_event_t* e) {
    if (!s_home_touch.active) {
        return;
    }
    auto* state = static_cast<PagerState*>(lv_event_get_user_data(e));
    lv_indev_t* indev = lv_event_get_indev(e);
    if (indev == nullptr) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    const int dx = p.x - s_home_touch.start_x;
    const int dy = p.y - s_home_touch.start_y;
    if (!HomeTouchIsTapLike(dx, dy)) {
        ResetHomeIdleTimer();
    }

    HomeTouchUpdateAxisLock(dx, dy);

    if (s_home_touch.axis != HomeGestureAxis::Horizontal) {
        return;
    }

    // 圆屏四叶瓣不跟手拖页：横移够阈值就立刻翻一页；同一次触摸不再连翻。
    if (state != nullptr && state->clover) {
        if (!s_home_touch.page_flipped && !state->clover_busy) {
            const int anchor_dx = p.x - s_home_touch.last_x;
            if (std::abs(anchor_dx) >= kCloverSwipeTriggerPx) {
                s_home_touch.consumed = true;
                s_home_touch.paging = true;
                s_home_touch.page_flipped = true;
                s_home_touch.last_x = static_cast<int16_t>(p.x);
                HomeTouchHandleSwipe(state, anchor_dx < 0 ? HomeTouchKind::SwipeLeft
                                                         : HomeTouchKind::SwipeRight);
            }
        }
        return;
    }

    if (state != nullptr) {
        SetPagerSkeletonMode(state, true);
    }

    if (!s_home_touch.consumed || s_home_touch.paging) {
        HomeTouchTryStartPageDrag(state, dx, dy, p.x);
    }

    if (s_home_touch.paging && state != nullptr && state->pager != nullptr) {
        const int delta_x = p.x - s_home_touch.last_x;
        if (delta_x != 0) {
            lv_obj_scroll_by(state->pager, delta_x, 0, LV_ANIM_OFF);
        }
        s_home_touch.last_x = static_cast<int16_t>(p.x);
    }
}

void OnHomeReleased(lv_event_t* e) {
    if (!s_home_touch.active) {
        return;
    }

    auto* state = static_cast<PagerState*>(lv_event_get_user_data(e));

    lv_indev_t* indev = lv_event_get_indev(e);
    int dx = 0;
    int dy = 0;
    if (indev != nullptr) {
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        dx = p.x - s_home_touch.start_x;
        dy = p.y - s_home_touch.start_y;
    }

    const bool was_paging = s_home_touch.paging;

    if (s_home_touch.axis == HomeGestureAxis::Vertical) {
        // 竖滑留给状态栏/其它手势；首页翻页只处理水平拖拽。
    } else if (was_paging && state != nullptr) {
        // 松手后对齐最近一页（此前 else if 被注释吞掉，导致不 snap、灰块残留）。
        SnapPagerToNearestPage(state, dx);
    } else if (!s_home_touch.consumed) {
        const uint32_t elapsed = lv_tick_elaps(s_home_touch.press_tick);
        if (HomeTouchIsTapLike(dx, dy)) {
            const HomeTouchKind kind =
                elapsed < kHomeLongPressMs ? HomeTouchKind::Click
                                           : HomeTouchKind::LongPress;
            if (kind == HomeTouchKind::Click) {
                RememberHomePage(state);
            }
            HomeTouchDispatchTapLike(kind);
        } else {
            const HomeTouchKind kind = HomeTouchClassifySwipe(dx, dy);
            if (state != nullptr && kind != HomeTouchKind::None &&
                !s_home_touch.page_flipped) {
                s_home_touch.page_flipped = true;
                HomeTouchHandleSwipe(state, kind);
            }
        }
    }

    s_home_touch.active = false;
    s_home_touch.consumed = false;
    s_home_touch.paging = false;
    s_home_touch.page_flipped = false;
    s_home_touch.axis = HomeGestureAxis::None;
    s_home_touch.press_cell = nullptr;
    s_home_touch.app = nullptr;

    // 非翻页拖拽：对齐在当前页且无滚动动画时恢复图标。
    // 翻页路径由 Snap/GoToPage → SCROLL_END 关 skeleton。
    if (!was_paging && state != nullptr && state->pager != nullptr &&
        !lv_obj_is_scrolling(state->pager)) {
        const int32_t scroll_x = lv_obj_get_scroll_x(state->pager);
        const int32_t expected = PagerScrollXForPage(state, state->current_page);
        if (scroll_x == expected) {
            SetPagerSkeletonMode(state, false);
        }
    }
}

// ????????LV_OBJ_FLAG_EVENT_BUBBLE?? cell / icon ??
// PRESSED /
// PRESSING / RELEASED ???????????? handler ????
void EnableHomeEventBubble(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);
    const uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; ++i) {
        EnableHomeEventBubble(lv_obj_get_child(obj, i));
    }
}

void OnHomeScreenLoaded(lv_event_t* e) {
    lv_obj_t* scr = lv_event_get_current_target_obj(e);
    EnableHomeEventBubble(scr);

    auto* state = static_cast<PagerState*>(lv_event_get_user_data(e));
    if (state != nullptr && state->clover) {
        int page = ClampHomePage(s_last_home_page, state->page_count);
        lv_anim_delete(state, nullptr);
        state->clover_busy = false;
        state->clover_queued_step = 0;
        CloverApplyPage(state, page);
        if (state->clover_layer != nullptr) {
            lv_obj_set_style_opa(state->clover_layer, LV_OPA_COVER, LV_PART_MAIN);
        }
        CloverResetTransition(state);
        HighlightDot(state, page);
    } else if (state != nullptr && state->pager != nullptr) {
        lv_obj_update_layout(state->pager);
        int page = ClampHomePage(s_last_home_page, state->page_count);
        lv_obj_scroll_to_x(state->pager, PagerScrollXForPage(state, page),
                           LV_ANIM_OFF);
        HighlightDot(state, page);
    }

    PwrKey_OnScreenLifecycle("home", SCREEN_LIFECYCLE_LOAD);
    StartHomeIdleTimer();
}

void OnHomeScreenUnloaded(lv_event_t* e) {
    RememberHomePage(static_cast<PagerState*>(lv_event_get_user_data(e)));
    PwrKey_OnScreenLifecycle("home", SCREEN_LIFECYCLE_UNLOAD);
}

void OnScreenDeleted(lv_event_t* e) {
    CancelCellScaleTimer();
    StopHomeIdleTimer();
    auto* state = static_cast<PagerState*>(lv_event_get_user_data(e));
    RememberHomePage(state);
    if (s_active_home_pager == state) {
        s_active_home_pager = nullptr;
    }
    // 翻页动画的 var 是 state，先撤动画再释放，否则回调会踩野指针。
    lv_anim_delete(state, nullptr);
    delete state;
}

// ---------------------------------------------------------------------------
// ??????(??PwrKey_Init ??????lv_async_call ??)
//
// ??
// PWR_KEY ???????????????????/ ????// ??????mask ??????card ???????????//
// ??????//   - pwr_key_handler ??IOExpander monitor task ?????????
//     lv_async_call ??ShowPowerOptionsDialog ????LVGL ??????//   - ??????
// PWR_KEY_PULSE ???????
// FreeRTOS task ????//     ????LVGL ????????IOExpander ??monitor??//
// EVENT_BUBBLE ????????card / ????????????mask ??
// ???????????????OnPwrMaskClicked ??????target ????// ---------------------------------------------------------------------------
struct PowerDialogUi {
    lv_obj_t* mask = nullptr;
    lv_obj_t* card = nullptr;
};

PowerDialogUi s_pwr_dlg;

void ClosePowerDialog() {
    if (s_pwr_dlg.mask != nullptr) {
        lv_obj_delete(s_pwr_dlg.mask);
    }
    s_pwr_dlg = PowerDialogUi{};
}

void OnPwrMaskClicked(lv_event_t* e);  // forward decl

lv_obj_t* s_shutdown_screen = nullptr;

enum class ShutdownScreenMode {
    kPoweringOff,
    kUnsupported,
};

void ReturnHomeFromShutdownScreen(lv_event_t* /*e*/) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* home = HomeScreen::Create();
    lv_screen_load(home);
    if (old_scr != nullptr && old_scr != home) {
        lv_obj_delete_async(old_scr);
    }
    s_shutdown_screen = nullptr;
}

void AppendShutdownProgressContent(lv_obj_t* parent, ShutdownScreenMode mode,
                                   const char* reason) {
    lv_obj_t* box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, kPanelW - 80, LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_row(box, 24, LV_PART_MAIN);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);

    if (mode == ShutdownScreenMode::kPoweringOff) {
        lv_obj_t* spin = lv_spinner_create(box);
        lv_obj_set_size(spin, 140, 140);
        lv_spinner_set_anim_params(spin, 1000, 200);
        lv_obj_set_style_arc_color(spin, lv_color_hex(0x2A2F3A), LV_PART_MAIN);
        lv_obj_set_style_arc_color(spin, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(spin, 10, LV_PART_MAIN);
        lv_obj_set_style_arc_width(spin, 10, LV_PART_INDICATOR);
        lv_obj_remove_flag(spin, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_t* icon = lv_label_create(box);
        lv_label_set_text(icon, LV_SYMBOL_WARNING);
        lv_obj_set_style_text_color(icon, lv_color_hex(0xFBBF24), LV_PART_MAIN);
        lv_obj_set_style_text_font(icon, &font_puhui_30_4, LV_PART_MAIN);
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t* lbl = lv_label_create(box);
    lv_label_set_text(lbl, mode == ShutdownScreenMode::kPoweringOff
                               ? I18n::T("正在关机...")
                               : I18n::T("当前板级不支持软件断电"));
    lv_obj_set_width(lbl, kPanelW - 100);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* sub = lv_label_create(box);
    lv_label_set_text(sub, mode == ShutdownScreenMode::kPoweringOff
                               ? (reason != nullptr ? reason : I18n::T("请稍候"))
                               : I18n::T("S31 没有外置电源保持/关断 IO，点击屏幕返回主页"));
    lv_obj_set_width(sub, kPanelW - 120);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x9CA3AF), LV_PART_MAIN);
    lv_obj_set_style_text_font(sub, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_remove_flag(sub, LV_OBJ_FLAG_CLICKABLE);
}

lv_obj_t* CreateShutdownScreen(ShutdownScreenMode mode, const char* reason) {
    lv_obj_t* screen = lv_obj_create(NULL);
    lv_obj_set_size(screen, kPanelW, kPanelH);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    if (mode == ShutdownScreenMode::kUnsupported) {
        lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(screen, ReturnHomeFromShutdownScreen,
                            LV_EVENT_CLICKED, nullptr);
    }
    AppendShutdownProgressContent(screen, mode, reason);
    return screen;
}

// ????????????????????????????
void ShowShutdownScreen(ShutdownScreenMode mode, const char* reason) {
    ClosePowerDialog();

    if (s_shutdown_screen != nullptr) {
        lv_obj_delete(s_shutdown_screen);
    }
    s_shutdown_screen = CreateShutdownScreen(mode, reason);
    lv_screen_load(s_shutdown_screen);
}

// 关机脉冲 task：Claw4 用 TCA9555 打 PWR_KEY_PULSE；VoCat 释放 PG2；S31 无硬件断电。
void PwrShutdownPulseTask(void* /*arg*/) {
#if CONFIG_BOARD_TYPE_ESP_VOCAT
    vTaskDelay(pdMS_TO_TICKS(1200));
    board_release_power_hold_if_supported();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#elif defined(BOARD_ESP_SHOW)
    ESP_LOGW(TAG_HOME, "ESP-Show has no software power-off IO");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#elif defined(CONFIG_IDF_TARGET_ESP32S31)
    ESP_LOGW(TAG_HOME, "S31 has no IOExpander; stay on shutdown screen");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#else
    auto& io = IOExpander::getInstance();
    constexpr int kPulseHalfMs = 100;
    for (;;) {
        io.setLevel(IOExpander::Pin::PWR_KEY_PULSE, true);
        vTaskDelay(pdMS_TO_TICKS(kPulseHalfMs));
        io.setLevel(IOExpander::Pin::PWR_KEY_PULSE, false);
        vTaskDelay(pdMS_TO_TICKS(kPulseHalfMs));
    }
#endif
}

void BeginSystemShutdown(const char* reason) {
#if (defined(CONFIG_IDF_TARGET_ESP32S31) && !defined(BOARD_ESP_VOCAT)) || \
    defined(BOARD_ESP_SHOW)
    ESP_LOGW(TAG_HOME, "board has no IOExpander/software power-off");
    IdlePower_Stop();
    ShowShutdownScreen(ShutdownScreenMode::kUnsupported, reason);
    return;
#else
    static bool shutting_down = false;
    if (shutting_down) {
        return;
    }
    shutting_down = true;

    IdlePower_Stop();
    ShowShutdownScreen(ShutdownScreenMode::kPoweringOff, reason);
    xTaskCreate(PwrShutdownPulseTask, "pwr_off_pulse", 2048, nullptr, 5, nullptr);
#endif
}

void OnPwrShutdownClicked(lv_event_t* /*e*/) {
    BeginSystemShutdown(I18n::T("关机 [电源键]"));
}

void OnPwrRebootClicked(lv_event_t* /*e*/) {
    // ESP_LOGW(TAG_HOME, "???? [??]????Application::Reboot()");
    ClosePowerDialog();
    Application::GetInstance().Reboot();
}

void OnPwrMaskClicked(lv_event_t* e) {
    // ???????mask ???????card / ????????EVENT_BUBBLE
    // ?????????????????????target ?????????
    // ????EVENT_BUBBLE ?????????
if (lv_event_get_target_obj(e) != lv_event_get_current_target_obj(e)) {
        return;
    }
    ESP_LOGI(TAG_HOME, "power dialog mask clicked, close");
    ClosePowerDialog();
}

lv_obj_t* CreatePowerActionBtn(lv_obj_t* parent,
                               const char* icon_src,
                               const char* text,
                               lv_event_cb_t on_click) {
    const int kBtnSize = kLayoutRoundSmall ? 100 : 180;
    const int kBtnIconSize = kLayoutRoundSmall ? 48 : 96;

    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, kBtnSize, kBtnSize);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);

    lv_obj_set_style_bg_color(btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_10, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, kLayoutRoundSmall ? 16 : 24, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_opa(btn, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_30, LV_PART_MAIN | LV_STATE_PRESSED);

    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t* icon = lv_image_create(btn);
    lv_image_set_src(icon, icon_src);
    lv_obj_set_size(icon, kBtnIconSize, kBtnIconSize);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(
        lbl, kLayoutRoundSmall ? &font_puhui_20_4 : &font_puhui_30_4,
        LV_PART_MAIN);
    lv_obj_set_style_pad_top(lbl, kLayoutRoundSmall ? 6 : 12, LV_PART_MAIN);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, on_click, LV_EVENT_CLICKED, nullptr);
    return btn;
}

void ShowPowerDialog() {
    if (s_pwr_dlg.mask != nullptr) {
        return;
    }
    lv_obj_t* parent = lv_screen_active();
    if (parent == nullptr) {
        return;
    }

    // ---- ???? ----
    // FLOATING?? mask ??????flex / grid ????    // ?? gps_screen ????????LV_FLEX_FLOW_COLUMN??????flag
    // ??mask ??????flex ?? ???`lv_obj_set_pos(0,0)` ??????
    // ???????? 720x720 ??????????????????    // FLOATING ??mask ?????????set_pos ?????dialog ????    // ?????????????
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
    lv_obj_add_event_cb(mask, OnPwrMaskClicked, LV_EVENT_CLICKED, nullptr);
    s_pwr_dlg.mask = mask;

    // ---- card ----
    const int kCardW = kLayoutRoundSmall ? 280 : 480;
    const int kCardH = kLayoutRoundSmall ? 240 : 360;
    const int kCardPad = kLayoutRoundSmall ? 16 : 24;
    lv_obj_t* card = lv_obj_create(mask);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, kCardW, kCardH);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1B2030), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, kCardPad, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    // card is clickable so clicks don't bubble to mask
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    s_pwr_dlg.card = card;

    lv_obj_t* title = lv_label_create(card);
    lv_label_set_text(title, I18n::T("电源"));
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(
        title, kLayoutRoundSmall ? &font_puhui_20_4 : &font_puhui_30_4,
        LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE);

    // ---- actions ----
    lv_obj_t* row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, kLayoutRoundSmall ? 20 : 32, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    CreatePowerActionBtn(row, "A:ic_s_home_reboot.spng", I18n::T("重启"), OnPwrRebootClicked);
    CreatePowerActionBtn(row, "A:ic_s_home_power.spng", I18n::T("关机"), OnPwrShutdownClicked);

    // ---- hint ----
    lv_obj_t* hint = lv_label_create(card);
    lv_label_set_text(hint, I18n::T("选择电源操作"));
    lv_obj_set_style_text_color(hint, lv_color_hex(0x9CA3AF), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(hint, LV_OBJ_FLAG_CLICKABLE);
}

lv_obj_t* CreateCloverHotspot(lv_obj_t* parent, int x, int y, int w, int h,
                              const AppEntry* entry) {
    if (entry == nullptr || entry->launch == nullptr) {
        return nullptr;
    }
    lv_obj_t* hs = lv_obj_create(parent);
    lv_obj_remove_style_all(hs);
    lv_obj_set_pos(hs, x, y);
    lv_obj_set_size(hs, w, h);
    lv_obj_set_style_bg_opa(hs, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hs, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_width(hs, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(hs, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(hs, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(hs, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(hs, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(hs, kAppCellFlag);
    lv_obj_set_user_data(hs, const_cast<AppEntry*>(entry));
    lv_obj_set_style_transform_pivot_x(hs, w / 2, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_transform_pivot_y(hs, h / 2, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale(hs, 250, LV_PART_MAIN | LV_STATE_PRESSED);
    return hs;
}

const char* CloverDisplayName(const AppEntry* entry) {
    if (entry == nullptr || entry->icon_suffix == nullptr) {
        return "";
    }
    if (std::strcmp(entry->icon_suffix, "wifi") == 0) {
        return "网络";
    }
    if (std::strcmp(entry->icon_suffix, "pin") == 0) {
        return "引脚";
    }
    return entry->name != nullptr ? entry->name : "";
}

void AddCloverPetalVisual(lv_obj_t* page, const AppEntry* entry, int app_idx,
                          int slot) {
    if (page == nullptr || entry == nullptr || slot < 0 || slot >= 4) {
        return;
    }
    constexpr int kIconCx[4] = {180, 298, 180, 62};
    constexpr lv_coord_t kIconCy[4] = {60, 178, 282, 178};
    constexpr lv_coord_t kTextCy[4] = {104, 222, 326, 222};

    lv_obj_t* icon = lv_image_create(page);
    if (app_idx >= 0 && app_idx < kTotalApps) {
        lv_image_set_src(icon, CloverIconSrc(app_idx));
    }
    SetupCloverIcon(icon, kIconCx[slot], kIconCy[slot]);

    lv_obj_t* name = lv_label_create(page);
    lv_label_set_text(name, I18n::T(CloverDisplayName(entry)));
    lv_obj_set_width(name, 120);
    lv_label_set_long_mode(name, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(name, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(name, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(name, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_pos(name, kIconCx[slot] - 60, kTextCy[slot] - 10);
    lv_obj_remove_flag(name, LV_OBJ_FLAG_CLICKABLE);
}

// slots: 0上 1右 2下 3左；app_indices 可为 -1 表示空瓣
lv_obj_t* CreateCloverPage(lv_obj_t* pager, const int* app_indices) {
    lv_obj_t* page = lv_obj_create(pager);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, kPanelW, kPanelH);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    struct Slot {
        int x;
        int y;
        int w;
        int h;
    };
    constexpr Slot kSlots[kCloverAppsPerPage] = {
        {110, 6, 140, 118},    // 上
        {232, 110, 122, 140},  // 右
        {110, 236, 140, 118},  // 下
        {6, 110, 122, 140},    // 左
    };

    for (int s = 0; s < kCloverAppsPerPage; ++s) {
        if (app_indices == nullptr) {
            break;
        }
        const int idx = app_indices[s];
        if (idx < 0 || idx >= kTotalApps) {
            continue;
        }
        const AppEntry& app = kApps[idx];
        AddCloverPetalVisual(page, &app, idx, s);
        CreateCloverHotspot(page, kSlots[s].x, kSlots[s].y, kSlots[s].w,
                            kSlots[s].h, &app);
    }
    return page;
}

lv_obj_t* CreatePage(lv_obj_t* pager, int page_index, int total_apps) {
    lv_obj_t* page = lv_obj_create(pager);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, kPanelW, kPagerHeight);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(page, kPagePadHor, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(page, kPagePadVer, LV_PART_MAIN);
    lv_obj_set_style_pad_column(page, kGridColGap, LV_PART_MAIN);
    lv_obj_set_style_pad_row(page, kGridRowGap, LV_PART_MAIN);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    // Fixed 3x3 grid -- each app sits in its natural (col, row) slot so an
    // under-filled page (e.g. a single app on page 2) anchors top-left
    // instead of getting visually centered by a flex space-distribute.
    lv_obj_set_grid_dsc_array(page, s_col_dsc, s_row_dsc);
    lv_obj_set_layout(page, LV_LAYOUT_GRID);

    const int start = page_index * kAppsPerPage;
    for (int i = 0; i < kAppsPerPage; ++i) {
        const int idx = start + i;
        if (idx >= total_apps)
            break;
        const AppEntry& app = kApps[idx];
        if (app.icon_suffix == nullptr)
            continue;
        lv_obj_t* cell = CreateAppCell(page, app, idx);
        const int col = i % kPageCols;
        const int row = i / kPageCols;
        lv_obj_set_grid_cell(cell, LV_GRID_ALIGN_STRETCH, col, 1, LV_GRID_ALIGN_STRETCH, row, 1);
    }
    return page;
}

void CreateIndicator(lv_obj_t* screen, PagerState* state) {
    // The pill-shaped capsule under the dots gives the indicator enough
    // contrast against any wallpaper / page colour without competing for
    // attention.  It only shows when there are 2+ pages.
    lv_obj_t* indicator = lv_obj_create(screen);
    lv_obj_remove_style_all(indicator);
    lv_obj_set_size(indicator, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(indicator, LV_ALIGN_BOTTOM_MID, 0, -kIndicatorYOffset);
    lv_obj_set_style_bg_color(indicator, lv_color_hex(kIndicatorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(indicator, LV_OPA_40, LV_PART_MAIN);  // ~40% black
    lv_obj_set_style_radius(indicator, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(indicator, kIndicatorPadHor, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(indicator, kIndicatorPadVer, LV_PART_MAIN);
    lv_obj_set_style_pad_column(indicator, kDotGap, LV_PART_MAIN);
    lv_obj_remove_flag(indicator, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(indicator, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(indicator, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    // The indicator is purely decorative -- touch events fall through to
    // the pager underneath so the user can grab it to swipe pages.
    lv_obj_remove_flag(indicator, LV_OBJ_FLAG_CLICKABLE);
    state->indicator = indicator;

    for (int i = 0; i < state->page_count; ++i) {
        lv_obj_t* dot = lv_obj_create(indicator);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, kDotSize, kDotSize);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(dot, lv_color_hex(kDotColor), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, LV_OPA_40, LV_PART_MAIN);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        state->dots[i] = dot;
    }

    HighlightDot(state, 0);
}

void OnOpenDigitalPeopleAsync(void* /*arg*/) {
    LaunchDigitalPeople(digital_people_lifecycle_cb);
}

void CreateCenterBatteryGroup(lv_obj_t* parent, HomeStatusState* st) {
    if (parent == nullptr || st == nullptr || !kLayoutRoundSmall) {
        return;
    }

    // 中央深色圆形区域约 110x110，电池图标和百分比上下排列，
    // 保持在圆内并避开四周四个功能瓣的文字。
    lv_obj_t* group = lv_obj_create(parent);
    lv_obj_remove_style_all(group);
    lv_obj_set_size(group, 110, 82);
    lv_obj_align(group, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(group, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(group, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_row(group, 0, LV_PART_MAIN);
    lv_obj_remove_flag(group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(group, LV_OBJ_FLAG_CLICKABLE);

    st->battery_icon_lbl = lv_label_create(group);
    lv_label_set_text(st->battery_icon_lbl, FONT_AWESOME_BATTERY_FULL);
    lv_obj_set_width(st->battery_icon_lbl, 52);
    lv_obj_align(st->battery_icon_lbl, LV_ALIGN_TOP_MID, 0, 3);
    lv_obj_set_style_text_font(st->battery_icon_lbl, &font_awesome_20_4,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(st->battery_icon_lbl, lv_color_hex(0xFFFFFF),
                                LV_PART_MAIN);
    lv_obj_set_style_text_align(st->battery_icon_lbl, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);

    st->battery_pct_lbl = lv_label_create(group);
    lv_label_set_text(st->battery_pct_lbl, "--%");
    lv_obj_set_width(st->battery_pct_lbl, 80);
    lv_obj_align(st->battery_pct_lbl, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_text_align(st->battery_pct_lbl, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(st->battery_pct_lbl, &font_puhui_20_4,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(st->battery_pct_lbl, lv_color_hex(0xFFFFFF),
                                LV_PART_MAIN);
}

}  // namespace

void HomeScreen::WarmStatusCaches() { WarmStatusCachesImpl(); }

void HomeScreen::OpenDigitalPeopleAsync() {
    screen_async_call(OnOpenDigitalPeopleAsync, nullptr);
}

void HomeScreen::ShowPowerOptionsDialog() { ShowPowerDialog(); }

lv_obj_t* CreateRoundCloverHome() {
    EnsureIconPathsBuilt();

    int order[kTotalApps];
    int order_count = 0;
    BuildCloverAppOrder(order, &order_count);

    int page_count =
        order_count > 0
            ? (order_count + kCloverAppsPerPage - 1) / kCloverAppsPerPage
            : 1;
    if (page_count > kMaxPages) {
        page_count = kMaxPages;
    }

    lv_obj_t* screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    auto* state = new PagerState{};
    state->pager = nullptr;
    state->page_count = page_count;
    state->current_page = 0;
    state->clover = true;
    state->clover_order_count = order_count;
    for (int i = 0; i < order_count; ++i) {
        state->clover_order[i] = order[i];
    }
    s_active_home_pager = state;

    const int restore_page = ClampHomePage(s_last_home_page, page_count);

    lv_obj_t* chrome = lv_image_create(screen);
    lv_image_set_src(chrome, CloverChromeSrc());
    lv_obj_set_size(chrome, kPanelW, kPanelH);
    lv_obj_align(chrome, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_remove_flag(chrome, LV_OBJ_FLAG_CLICKABLE);
    state->clover_chrome = chrome;
    state->clover_round = nullptr;

    auto* status = new HomeStatusState{};
    CreateStatusBar(screen, status);

    lv_obj_t* layer = lv_obj_create(screen);
    lv_obj_remove_style_all(layer);
    lv_obj_set_size(layer, kPanelW, kPanelH);
    lv_obj_set_pos(layer, 0, 0);
    lv_obj_set_style_bg_opa(layer, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(layer, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    state->clover_layer = layer;
    CreateCenterBatteryGroup(layer, status);
    UpdateHomeStatusBar(status);

    constexpr lv_coord_t kIconCx[4] = {180, 298, 180, 62};
    constexpr lv_coord_t kIconCy[4] = {60, 178, 282, 178};
    constexpr lv_coord_t kTextCy[4] = {104, 222, 326, 222};
    struct Slot {
        int x;
        int y;
        int w;
        int h;
    };
    constexpr Slot kSlots[kCloverAppsPerPage] = {
        {125, 6, 140, 118},
        {250, 110, 122, 140},
        {125, 236, 140, 118},
        {20, 110, 122, 140},
    };

    for (int s = 0; s < kCloverAppsPerPage; ++s) {
        lv_obj_t* icon = lv_image_create(layer);
        SetupCloverIcon(icon, kIconCx[s], kIconCy[s]);
        state->clover_icon[s] = icon;

        lv_obj_t* name = lv_label_create(layer);
        lv_label_set_text(name, "");
        lv_obj_set_width(name, 128);
        lv_label_set_long_mode(name, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(name, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(name, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(name, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_pos(name, kIconCx[s] - 64, kTextCy[s] - 10);
        lv_obj_remove_flag(name, LV_OBJ_FLAG_CLICKABLE);
        state->clover_name[s] = name;

        lv_obj_t* hs = CreateCloverHotspot(screen, kSlots[s].x, kSlots[s].y,
                                          kSlots[s].w, kSlots[s].h, &kWakeEntry);
        if (hs != nullptr) {
            lv_obj_add_flag(hs, LV_OBJ_FLAG_FLOATING);
        }
        state->clover_hs[s] = hs;
    }

    CloverApplyPage(state, restore_page);
    lv_obj_set_style_opa(layer, LV_OPA_COVER, LV_PART_MAIN);
    StartCloverPrefetch(order, order_count);

    lv_obj_t* wake = CreateCloverHotspot(screen, 125, 125, 110, 110, &kWakeEntry);
    if (wake != nullptr) {
        lv_obj_add_flag(wake, LV_OBJ_FLAG_FLOATING);
        lv_obj_move_foreground(wake);
    }

    if (page_count > 1) {
        CreateIndicator(screen, state);
        if (state->indicator != nullptr) {
            lv_obj_add_flag(state->indicator, LV_OBJ_FLAG_FLOATING);
            lv_obj_set_style_bg_opa(state->indicator, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_style_pad_ver(state->indicator, 2, LV_PART_MAIN);
            lv_obj_align(state->indicator, LV_ALIGN_BOTTOM_MID, 0, -4);
        }
        HighlightDot(state, restore_page);
    }

    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(screen, OnHomePressed, LV_EVENT_PRESSED, state);
    lv_obj_add_event_cb(screen, OnHomePressing, LV_EVENT_PRESSING, state);
    lv_obj_add_event_cb(screen, OnHomeReleased, LV_EVENT_RELEASED, state);
    lv_obj_add_event_cb(screen, OnHomeScreenLoaded, LV_EVENT_SCREEN_LOADED,
                        state);
    lv_obj_add_event_cb(screen, OnHomeScreenUnloaded, LV_EVENT_SCREEN_UNLOADED,
                        state);
    lv_obj_add_event_cb(screen, OnScreenDeleted, LV_EVENT_DELETE, state);

    if (status->bar != nullptr) {
        lv_obj_move_foreground(status->bar);
    }
    CreateActivationOverlay(screen, status);
    return screen;
}

lv_obj_t* HomeScreen::Create() {
    // ????????NVS ?????? id ??kApps ??icon_suffix ????
    // ??????s_icon_paths ??????CreateAppCell ??????????
EnsureIconPathsBuilt();

    if (kLayoutRoundSmall) {
        return CreateRoundCloverHome();
    }

    lv_obj_t* screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    // ----- Figure out how many pages we need -----
    // kTotalApps ????namespace ?????????????
int page_count = (kTotalApps + kAppsPerPage - 1) / kAppsPerPage;
    if (page_count < 1)
        page_count = 1;
    if (page_count > kMaxPages)
        page_count = kMaxPages;

    // PagerState owns the dot pointers + current_page; freed on screen del.
    auto* state = new PagerState{};
    state->page_count = page_count;
    state->current_page = 0;
    s_active_home_pager = state;

    auto* status = new HomeStatusState{};
    CreateStatusBar(screen, status);

    // ----- Pager?????????? -----
    // ????LVGL ??
// scrollable???? screen ??????????    // ??????
// OnHomePressing ???? pager???????fling ??    // ??
// lv_obj_scroll_to_x(..., LV_ANIM_ON) ??????????    // ??????
// PNG ?????????????????????
lv_obj_t* pager = lv_obj_create(screen);
    state->pager = pager;
    lv_obj_remove_style_all(pager);
    lv_obj_set_size(pager, kPanelW, kPagerHeight);
    lv_obj_align(pager, LV_ALIGN_TOP_LEFT, 0, kStatusBarHeight);
    lv_obj_set_style_bg_opa(pager, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(pager, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(pager, LV_SCROLLBAR_MODE_OFF);
    // Row flex????????????pager ??scroll ???????????
lv_obj_set_flex_flow(pager, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pager, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(pager, OnPagerScrollBegin, LV_EVENT_SCROLL_BEGIN, nullptr);
    lv_obj_add_event_cb(pager, OnPagerScrollEnd, LV_EVENT_SCROLL_END, state);

    if (page_count > 1) {
        // ???????????????? | ????| ????
        CreatePage(pager, page_count - 1, kTotalApps);
        for (int p = 0; p < page_count; ++p) {
            CreatePage(pager, p, kTotalApps);
        }
        CreatePage(pager, 0, kTotalApps);
    } else {
        CreatePage(pager, 0, kTotalApps);
    }

    // ----- Page indicator -----
    // Only worth drawing when there is more than one page; otherwise it's
    // a lonely single dot which just adds noise.
    if (page_count > 1) {
        CreateIndicator(screen, state);
    }

    // ----- ???screen ??PRESSED / PRESSING / RELEASED ?????? -----
    // LV_EVENT_SCREEN_LOADED ????EVENT_BUBBLE????????????screen??
lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(screen, OnHomePressed, LV_EVENT_PRESSED, state);
    lv_obj_add_event_cb(screen, OnHomePressing, LV_EVENT_PRESSING, state);
    lv_obj_add_event_cb(screen, OnHomeReleased, LV_EVENT_RELEASED, state);
    lv_obj_add_event_cb(screen, OnHomeScreenLoaded, LV_EVENT_SCREEN_LOADED, state);
    lv_obj_add_event_cb(screen, OnHomeScreenUnloaded, LV_EVENT_SCREEN_UNLOADED,
                        state);
    lv_obj_add_event_cb(screen, OnScreenDeleted, LV_EVENT_DELETE, state);

    CreateActivationOverlay(screen, status);
    return screen;
}

void OnRefreshStatusBarAsync(void* /*user_data*/) {
    if (s_home_status != nullptr) {
        UpdateHomeStatusBar(s_home_status);
    }
}

void HomeScreen::ResetToFirstPage() {
    s_last_home_page = 0;
}

void HomeScreen::RefreshStatusBar() {
    // Application::CheckNewVersion / ShowActivationCode ??
// app_main
    // ????????
// LVGL????LVGL ??????????adapter ????    // ??
// lv_obj_invalidate ??Core0 ??????
// CPU??
screen_async_call(OnRefreshStatusBarAsync, nullptr);
}

int HomeScreen::GetIdleShutdownMinutes() {
    return IdlePower_GetShutdownMinutes();
}

void HomeScreen::SetIdleShutdownMinutes(int minutes) {
    IdlePower_SetShutdownMinutes(minutes);
    IdlePower_NotifyActivity();
}

int HomeScreen::GetIdleStandbyMinutes() {
    return IdlePower_GetStandbyMinutes();
}

void HomeScreen::SetIdleStandbyMinutes(int minutes) {
    IdlePower_SetStandbyMinutes(minutes);
    IdlePower_NotifyActivity();
}

void HomeScreen::RequestSystemShutdown(const char* reason) {
    BeginSystemShutdown(reason != nullptr ? reason : I18n::T("关机"));
}
