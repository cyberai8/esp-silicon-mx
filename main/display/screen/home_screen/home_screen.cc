#include "home_screen.h"
#include "i18n.h"

#include <cstdlib>
#include <cstdio>
#include <cctype>
#include <ctime>
#include <string>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <font_awesome.h>

#include "application.h"
#include "board.h"
#include "config.h"
#include "dual_network_board.h"
#include "nt26_board.h"
#include "IOExpander.hpp"
#include "bq27220_gauge.h"
#include "settings.h"
#include "settings_screen/settings_screen.h"
#include "calculator_screen/calculator_screen.h"
#include "calendar_screen/calendar_screen.h"
#include "call_screen/call_screen.h"
#include "camera_screen/camera_screen.h"
#include "chat_screen/chat_screen.h"
#include "digital_people_screen/digital_people_screen.h"
#include "game_2048_screen/game_2048_screen.h"
#include "gps_screen/gps_screen.h"
#include "level_screen/level_screen.h"
#include "magnet_screen/magnet_screen.h"
#include "music_screen/music_screen.h"
#include "radio_screen/radio_screen.h"
#include "recording_screen/recording_screen.h"
#include "openclaw_screen/openclaw_screen.h"
#include "ai_image_gen_screen/ai_image_gen_screen.h"
#include "translate_screen/translate_screen.h"
#include "pwr_key_handler.h"
#include "screen_util.h"
#include "idle_power_policy.h"
#include "vibrate_screen/vibrate_screen.h"
#include "weather_screen/weather_screen.h"
#include "network_screen/network_screen.h"
#include "pin_test_screen/pin_test_screen.h"
#include "test_screen/test_screen.h"
#include "sd_card_screen/sd_card_screen.h"
#include "theme_screen/theme_screen.h"
#include "info_screen/info_screen.h"
#include "theme_manager.h"
#include "wifi_required_dialog.h"

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

void calculator_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("calculator", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: calculator");
    } else {
        ESP_LOGI(TAG_HOME, "unload: calculator");
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

void calendar_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("calendar", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: calendar_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: calendar_screen");
    }
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

void radio_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("radio", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: radio_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: radio_screen");
    }
    RadioScreen::LifecycleCallback(event);
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

void weather_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("weather", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: weather_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: weather_screen");
    }
}

// GPS ????GPS_POWER ???????GpsScreen::LifecycleCallback????// ??????+ ???? camera / vibrate / bluetooth ??????????
void gps_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("gps", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: gps_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: gps_screen");
    }
    GpsScreen::LifecycleCallback(event);
}

// ????????????
// CameraScreen::LifecycleCallback??// ??????CAM_PWDN?TCA9555 IO2?????????? / ??????// esp_video / V4L2 ????????????????App ?????????
void camera_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("camera", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: camera_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: camera_screen");
    }
    CameraScreen::LifecycleCallback(event);
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

void chat_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("chat", event);
    ChatScreen::LifecycleCallback(event);
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

// ????????????????UNLOAD ????????/ ???? timer??// ??????????? log??
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

// OpenClaw ????????
// OpenClawScreen::LifecycleCallback????
// ?????? / ?????????? wake word ????
void openclaw_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("openclaw", event);
    OpenClawScreen::LifecycleCallback(event);
}

void ai_image_gen_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("ai_image_gen", event);
    AiImageGenScreen::LifecycleCallback(event);
}

void translate_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("translate", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: translate_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: translate_screen");
    }
    TranslateScreen::LifecycleCallback(event);
}

constexpr int kPanelW = DISPLAY_WIDTH;
constexpr int kPanelH = DISPLAY_HEIGHT;
// Claw4 方形 720；S31 Korvo-1 横屏 800x480。按分辨率选布局档。
constexpr int kPanelSize = (kPanelW < kPanelH) ? kPanelW : kPanelH;
constexpr bool kLayoutTall = (kPanelH >= 700);                 // 720x720
constexpr bool kLayoutWide = (kPanelW >= 750 && kPanelH < 700);  // 800x480
// 资源图标固定 128x128 圆角；横屏也用原生尺寸，避免 STRETCH 变换把圆角拉成叶片形。
constexpr int kIconAssetSize = 128;
constexpr int kStatusBarHeight = kLayoutTall ? 48 : (kLayoutWide ? 28 : 36);
// 横屏：指示器浮在 pager 上方，不额外占纵向空间，才能放下 128 图标+完整文字
constexpr int kIndicatorAreaHeight = kLayoutTall ? 40 : (kLayoutWide ? 0 : 28);
constexpr int kPagerHeight =
    kPanelH - kStatusBarHeight - kIndicatorAreaHeight;
constexpr int kAppsPerPage = 9;           // 3x3
constexpr int kPageCols = 3;
constexpr int kPageRows = 3;
constexpr int kIconSize = kIconAssetSize;
constexpr int kCellWidth = kLayoutTall ? 160 : (kLayoutWide ? 168 : 100);
constexpr int kNameGap = kLayoutTall ? 6 : (kLayoutWide ? 2 : 4);
constexpr int kNameAreaH = kLayoutTall ? 24 : (kLayoutWide ? 20 : 18);
constexpr int kCellHeight = kIconSize + kNameGap + kNameAreaH;
constexpr int kGridColGap = kLayoutTall ? 60 : (kLayoutWide ? 40 : 24);
constexpr int kGridRowGap = kLayoutTall ? 36 : (kLayoutWide ? 0 : 12);
constexpr int kPagePadHor =
    (kPanelW - kPageCols * kCellWidth - (kPageCols - 1) * kGridColGap) / 2;
constexpr int kPageContentH =
    kPageRows * kCellHeight + (kPageRows - 1) * kGridRowGap;
constexpr int kPagePadVer =
    (kPagerHeight > kPageContentH) ? (kPagerHeight - kPageContentH) / 2 : 0;

constexpr uint32_t kStatusBarBg = 0x000000;
constexpr int kMaxPages = 6;  // hard cap; bump if app list grows

// Grid descriptors -- static so the array pointers passed to LVGL outlive
// the call.  Initialized at namespace scope; LVGL reads them lazily during
// each page's relayout, so we never have to refresh them.
int32_t s_col_dsc[kPageCols + 1] = {
    kCellWidth,
    kCellWidth,
    kCellWidth,
    LV_GRID_TEMPLATE_LAST,
};
int32_t s_row_dsc[kPageRows + 1] = {
    kCellHeight,
    kCellHeight,
    kCellHeight,
    LV_GRID_TEMPLATE_LAST,
};

// Indicator dot geometry
constexpr int kDotSize = 8;
constexpr int kDotGap = 12;
constexpr int kIndicatorPadHor = 14;
constexpr int kIndicatorPadVer = 8;
constexpr int kIndicatorYOffset = 12;  // distance from panel bottom
constexpr uint32_t kIndicatorBg = 0x000000;
constexpr uint32_t kDotColor = 0xFFFFFF;

// Launchers take the lifecycle callback as an argument so we can attach it
// to the screen object before lv_screen_load() fires LV_EVENT_SCREEN_LOADED
// -- otherwise the first LOAD event would be missed.  Passing nullptr is
// supported (screen_attach_lifecycle no-ops in that case).
typedef void (*LaunchFn)(screen_lifecycle_cb_t lifecycle_cb);

struct AppEntry {
    // ??????????"chat"??2048"????LVGL ????ThemeManager
    // ??????id ???A:ic_app_home_theme{N}_{suffix}.spng??    // ????????????????????????AppEntry??
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

void LaunchCalculator(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = Calculator::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
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

void LaunchCalendar(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = CalendarScreen::Create();
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

void LaunchRadio(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = RadioScreen::Create();
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

void LaunchWeather(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = WeatherScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchGps(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = GpsScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchCamera(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = CameraScreen::Create();
    if (app == nullptr) {
        ESP_LOGE(TAG_HOME, "CameraScreen::Create() failed");
        return;
    }
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

void LaunchChat(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = ChatScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchDigitalPeople(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = DigitalPeopleScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
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

void LaunchOpenClaw(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    OpenClawScreen::SetLifecycleCallback(lifecycle_cb);
    lv_obj_t* app = OpenClawScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchAiImageGen(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = AiImageGenScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchTranslate(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = TranslateScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
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
        lv_async_call(EspClawSwitchFailAsync,
                      const_cast<char*>(I18n::T("未找到 ESPClaw\n请确认是否已安装到分区")));
        vTaskDelete(nullptr);
        return;
    }

    esp_err_t err = esp_ota_set_boot_partition(ota1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_HOME, "ESPClaw: set boot to %s failed: %s", ota1->label,
                 esp_err_to_name(err));
        lv_async_call(EspClawSwitchFailAsync,
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

void LaunchTheme(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = ThemeScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
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

void theme_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("theme", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: theme_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: theme_screen");
    }
    ThemeScreen::LifecycleCallback(event);
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

// ??
// app ??icon_suffix ?? ic_app_home_theme{N}_{suffix}.spng ??// ?? suffix ???name ????????????????????// ?? "??" ??suffix ??"gps"??????????"map"????" ??
// ????????ic_app_home_themeN_magnet.spng ????????????// name ??zh-CN msgid??????????
// I18n::T(entry.name)??
constexpr AppEntry kApps[] = {
    {"chat",           "聊天",     LaunchChat,          chat_lifecycle_cb,          true},
    {"wifi",           "网络配置", LaunchWifi,          wifi_lifecycle_cb,          false},
    {"digital_people", "数字人",   LaunchDigitalPeople, digital_people_lifecycle_cb, true},
    {"call",           "电话",     LaunchCall,          call_lifecycle_cb,          false},
    {"music",          "音乐",     LaunchMusic,         music_lifecycle_cb,         false},
    {"calendar",       "日历",     LaunchCalendar,      calendar_lifecycle_cb,      false},
    {"openclaw",       "OpenClaw", LaunchOpenClaw,      openclaw_lifecycle_cb,      true},
    {"espclaw",        "ESPClaw",  LaunchEspClaw,       nullptr,                    false},
    {"camera",         "相机",     LaunchCamera,        camera_lifecycle_cb,        false},
    {"gps",            "地图",     LaunchGps,           gps_lifecycle_cb,           true},
    {"spirit_level",   "水平仪",   LaunchLevel,         level_lifecycle_cb,         false},
    {"magnet",         "磁场",     LaunchMagnet,        magnet_lifecycle_cb,        false},
    {"vibrate",        "震动",     LaunchVibrate,       vibrate_lifecycle_cb,       false},
    {"calculator",     "计算器",   LaunchCalculator,    calculator_lifecycle_cb,    false},
    {"weather",        "天气",     LaunchWeather,       weather_lifecycle_cb,       true},
    {"sd",             "SD卡",     LaunchSdCard,        sd_card_lifecycle_cb,       false},
    {"pin",            "引脚测试", LaunchPinTest,       pin_test_lifecycle_cb,      false},
    {"2048",           "2048",     LaunchGame2048,      game_2048_lifecycle_cb,     false},
    {"info",           "信息",     LaunchInfo,          info_lifecycle_cb,          false},
    {"theme",          "主题",     LaunchTheme,         theme_lifecycle_cb,         false},
    {"test",           "测试",     LaunchTest,          test_lifecycle_cb,          false},
    {"settings",       "设置",     LaunchSettings,      settings_lifecycle_cb,      false},
    {"radio",          "电台",     LaunchRadio,         radio_lifecycle_cb,         true},
    {"recording",      "录音",     LaunchRecording,     recording_lifecycle_cb,     false},
    {"ai_image_gen",   "AI生图",   LaunchAiImageGen,    ai_image_gen_lifecycle_cb,  true},
    {"translate",      "翻译",     LaunchTranslate,     translate_lifecycle_cb,     true},
};

constexpr int kTotalApps = static_cast<int>(sizeof(kApps) / sizeof(kApps[0]));

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

void EnsureIconPathsBuilt() {
    static bool built = false;
    static int s_built_theme_id = 0;
    const int tid = ThemeManager::GetCurrentThemeId();
    // ??????/ ?? id ????????????
// NVS ???????????
if (built && s_built_theme_id == tid) {
        return;
    }
    for (int i = 0; i < kTotalApps; ++i) {
        std::snprintf(s_icon_paths[i], kIconPathBufSize,
                      "A:ic_app_home_theme%d_%s.spng",
                      tid, kApps[i].icon_suffix);
    }
    s_built_theme_id = tid;
    built = true;
    ESP_LOGI(TAG_HOME, "icon paths built for theme%d", tid);
}

// ---------------------------------------------------------------------------
// ????????????????????screen ??????????// ???? cell ??LV_EVENT_CLICKED??// ---------------------------------------------------------------------------

constexpr int kHomeMoveThreshold = 5;
constexpr int kHomeAxisLockThreshold = 12;
constexpr int kPageSnapThreshold = kPanelW / 5;
constexpr int kHomeFlickThreshold = 24;
constexpr uint32_t kHomeLongPressMs = 750;
constexpr uint32_t kPageSlideAnimMs = 300;

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
};

// ????????????????????????????????// ??????HomeScreen::Create() ??????????????????// ???????GoToPage?????HighlightDot ??????
// CreateIndicator ??????
// HighlightDot(state, 0) ???????
int s_last_home_page = 0;

struct HomeStatusState {
    lv_obj_t* bar = nullptr;
    lv_obj_t* network_icon_lbl = nullptr;
    lv_obj_t* network_type_lbl = nullptr;
    lv_obj_t* sim_slot_lbl = nullptr;
lv_obj_t* battery_icon_lbl = nullptr;
lv_obj_t* battery_pct_lbl  = nullptr;
    lv_obj_t* time_lbl = nullptr;
    lv_obj_t* activation_code_lbl = nullptr;
    lv_timer_t* update_timer = nullptr;
    const char* last_icon = nullptr;
    const char* last_battery_icon = nullptr;
    int  last_battery_pct = -1;
    bool last_battery_low = false;
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
    // DualNetworkBoard / network_screen 共用 NVS。
    // S31 Korvo-1 无 4G，默认 WiFi；P4 Claw4 默认 4G。
#if defined(CONFIG_IDF_TARGET_ESP32S31)
    constexpr int kDefaultNetType = 0;  // WiFi
#else
    constexpr int kDefaultNetType = 1;  // 4G
#endif
    const NetworkType type =
        DualNetworkBoard::LoadNetworkTypeFromSettings(kDefaultNetType);
    return type == NetworkType::ML307 ? 1 : 0;
}

// ??network_screen ?? "network/sim_slot" ??
// NVS key??// ?? 0 = ???????? 1 = ?????
int GetSavedSimSlot() {
    Settings settings("network", true);
    int v = settings.GetInt("sim_slot", 0);
    return (v == 1) ? 1 : 0;
}

void SaveSimSlot(int slot) {
    Settings settings("network", true);
    settings.SetInt("sim_slot", slot);
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
    lv_async_call(AsyncBootSimSlotSynced, msg);
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
    }

    // SIM ????4G ????????? / ?????WiFi ???????????    // ????????????????invalidate??
if (st->sim_slot_lbl != nullptr) {
        if (net_type == 1) {
            const int slot = GetSavedSimSlot();
            if (st->last_net_type != net_type || st->last_sim_slot != slot) {
                lv_label_set_text(st->sim_slot_lbl,
                                  slot == 1 ? I18n::T("外置卡") : I18n::T("内置卡"));
                st->last_sim_slot = slot;
            }
            lv_obj_remove_flag(st->sim_slot_lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            if (st->last_net_type != net_type) {
                lv_obj_add_flag(st->sim_slot_lbl, LV_OBJ_FLAG_HIDDEN);
            }
        }
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
    // ??????
// Font Awesome ?????????
    if (st->battery_icon_lbl != nullptr) {
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
            } else if (battery_level >= 20) {
                bat_icon = FONT_AWESOME_BATTERY_QUARTER;
            } else {
                bat_icon = FONT_AWESOME_BATTERY_EMPTY;
            }
            if (bat_icon != st->last_battery_icon) {
                st->last_battery_icon = bat_icon;
                lv_label_set_text(st->battery_icon_lbl, bat_icon);
            }

            const bool low = !charging && battery_level < 20;
            if (low != st->last_battery_low) {
                st->last_battery_low = low;
                uint32_t color = low ? 0xF87171 : 0xFFFFFF;
                lv_obj_set_style_text_color(st->battery_icon_lbl,
                                            lv_color_hex(color), LV_PART_MAIN);
            }
        } else {
            // ??????????????
st->last_battery_icon = FONT_AWESOME_BATTERY_SLASH;
            lv_label_set_text(st->battery_icon_lbl, FONT_AWESOME_BATTERY_SLASH);
            if (st->last_battery_low) {
                st->last_battery_low = false;
                lv_obj_set_style_text_color(st->battery_icon_lbl,
                                            lv_color_hex(0xFFFFFF), LV_PART_MAIN);
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

            const bool low = !charging && battery_level < 20;

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

    if (st->activation_code_lbl != nullptr) {
        auto& app = Application::GetInstance();
        if (app.HasPendingActivation()) {
            char buf[48];
            std::snprintf(buf, sizeof(buf), I18n::T("激活码 %s"), app.GetPendingActivationCode().c_str());
            if (st->last_activation_text != buf) {
                st->last_activation_text = buf;
                lv_label_set_text(st->activation_code_lbl, buf);
            }
            if (!st->last_activation_visible) {
                st->last_activation_visible = true;
                lv_obj_remove_flag(st->activation_code_lbl, LV_OBJ_FLAG_HIDDEN);
            }
        } else if (st->last_activation_visible) {
            st->last_activation_visible = false;
            st->last_activation_text.clear();
            lv_obj_add_flag(st->activation_code_lbl, LV_OBJ_FLAG_HIDDEN);
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

lv_obj_t* CreateStatusBar(lv_obj_t* screen, HomeStatusState* st) {
    // ????????????????LVGL flex ??SIZE_CONTENT + SPACE_BETWEEN
    // ????????????????????100% ?????????????    //   - ?? 300px?????????+ 4G + ?????    //   - ?? 400px?????100% ????HH:MM?? ~18 ????????    //   - ?????? flex SPACE_BETWEEN ????
    constexpr int kStatusLeftWidth  = 300;
    constexpr int kStatusRightWidth = 400;

    lv_obj_t* bar = lv_obj_create(screen);
    st->bar = bar;
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, kPanelW, kStatusBarHeight);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kStatusBarBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(bar, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(bar, 8, LV_PART_MAIN);
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
    lv_obj_set_style_pad_column(left, 10, LV_PART_MAIN);

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

    // SIM ????????/ ????????????????G????????    // ?????UpdateHomeStatusBar ???????????????
st->sim_slot_lbl = lv_label_create(left);
    lv_label_set_long_mode(st->sim_slot_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(st->sim_slot_lbl, LV_SIZE_CONTENT);
    lv_label_set_text(st->sim_slot_lbl, "");
    lv_obj_set_style_text_font(st->sim_slot_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->sim_slot_lbl, lv_color_hex(0xC9D1D9),
                                LV_PART_MAIN);
    lv_obj_add_flag(st->sim_slot_lbl, LV_OBJ_FLAG_HIDDEN);

    // ---- ????????????????????????????----
    // ??????font_awesome ??????????????/ ??????    // ????????400px??????????????????    // ???? 100% ??????????"?? --%"????????flex
    // ?????????
lv_obj_t* right = lv_obj_create(bar);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, kStatusRightWidth, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
#if HOME_STATUS_SHOW_BATTERY_ICON
    // ??????????Font Awesome ?????????
    st->battery_icon_lbl = lv_label_create(right);
    lv_label_set_text(st->battery_icon_lbl, FONT_AWESOME_BATTERY_FULL);
    lv_obj_set_style_text_font(st->battery_icon_lbl, &font_awesome_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->battery_icon_lbl, lv_color_hex(0xFFFFFF),
                                LV_PART_MAIN);
#else
    // ?????????? + ????+ ????????
    lv_obj_set_style_pad_column(right, 14, LV_PART_MAIN);

    st->battery_pct_lbl = lv_label_create(right);
    lv_label_set_long_mode(st->battery_pct_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(st->battery_pct_lbl, 380);
    // lv_label_set_text(st->battery_pct_lbl, I18n::T("?? --%"));
    lv_obj_set_style_text_align(st->battery_pct_lbl, LV_TEXT_ALIGN_RIGHT,
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(st->battery_pct_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->battery_pct_lbl, lv_color_hex(0xFFFFFF),
                                LV_PART_MAIN);
#endif

    // ?? + ??????FLOATING ???? bar ??flex ????????    // ???????????????????????????????
lv_obj_t* center = lv_obj_create(bar);
    lv_obj_add_flag(center, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_style_all(center);
    lv_obj_set_size(center, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(center, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(center, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(center, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(center, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(center, 8, LV_PART_MAIN);
    lv_obj_align(center, LV_ALIGN_CENTER, 0, 0);

    st->time_lbl = lv_label_create(center);
    lv_label_set_long_mode(st->time_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(st->time_lbl, 80);
    lv_label_set_text(st->time_lbl, "--:--");
    lv_obj_set_style_text_align(st->time_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(st->time_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->time_lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);

    st->activation_code_lbl = lv_label_create(center);
    lv_label_set_long_mode(st->activation_code_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(st->activation_code_lbl, LV_SIZE_CONTENT);
    lv_label_set_text(st->activation_code_lbl, "");
    lv_obj_set_style_text_font(st->activation_code_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->activation_code_lbl, lv_color_hex(0xFBBF24),
                                LV_PART_MAIN);
    lv_obj_add_flag(st->activation_code_lbl, LV_OBJ_FLAG_HIDDEN);

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
    if (state == nullptr || state->pager == nullptr) {
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

    s_home_touch.active = true;
    s_home_touch.consumed = false;
    s_home_touch.paging = false;
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

    // ?????????????????????
if (s_home_touch.axis != HomeGestureAxis::Horizontal) {
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
            HomeTouchDispatchTapLike(kind);
        } else {
            const HomeTouchKind kind = HomeTouchClassifySwipe(dx, dy);
            if (state != nullptr && kind != HomeTouchKind::None) {
                HomeTouchHandleSwipe(state, kind);
            }
        }
    }

    s_home_touch.active = false;
    s_home_touch.consumed = false;
    s_home_touch.paging = false;
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

    // ???????layout ??????????pager ????????????    // ????Create() ??
// scroll_to_x ???? layout ??????????
auto* state = static_cast<PagerState*>(lv_event_get_user_data(e));
    if (state != nullptr && state->pager != nullptr) {
        lv_obj_update_layout(state->pager);
        int page = s_last_home_page;
        if (page < 0 || page >= state->page_count) {
            page = 0;
        }
        lv_obj_scroll_to_x(state->pager, PagerScrollXForPage(state, page),
                           LV_ANIM_OFF);
        HighlightDot(state, page);
    }

    PwrKey_OnScreenLifecycle("home", SCREEN_LIFECYCLE_LOAD);
    StartHomeIdleTimer();
}

void OnHomeScreenUnloaded(lv_event_t* /*e*/) {
    // ????????????Test????????UNLOAD ?????? home??    // ???????????????????????
PwrKey_OnScreenLifecycle("home", SCREEN_LIFECYCLE_UNLOAD);
}

void OnScreenDeleted(lv_event_t* e) {
    CancelCellScaleTimer();
    StopHomeIdleTimer();
    delete static_cast<PagerState*>(lv_event_get_user_data(e));
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

void AppendShutdownProgressContent(lv_obj_t* parent) {
    lv_obj_t* box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_row(box, 24, LV_PART_MAIN);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* spin = lv_spinner_create(box);
    lv_obj_set_size(spin, 140, 140);
    lv_spinner_set_anim_params(spin, 1000, 200);
    lv_obj_set_style_arc_color(spin, lv_color_hex(0x2A2F3A), LV_PART_MAIN);
    lv_obj_set_style_arc_color(spin, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spin, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spin, 10, LV_PART_INDICATOR);
    lv_obj_remove_flag(spin, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* lbl = lv_label_create(box);
    // lv_label_set_text(lbl, I18n::T("????..."));
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
}

lv_obj_t* CreateShutdownScreen() {
    lv_obj_t* screen = lv_obj_create(NULL);
    lv_obj_set_size(screen, kPanelW, kPanelH);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    AppendShutdownProgressContent(screen);
    return screen;
}

// ????????????????????????????
void ShowShutdownScreen() {
    ClosePowerDialog();

    if (s_shutdown_screen == nullptr) {
        s_shutdown_screen = CreateShutdownScreen();
    }
    lv_screen_load(s_shutdown_screen);
}

// 关机脉冲 task：Claw4 用 TCA9555 打 PWR_KEY_PULSE；S31 无该芯片，避免死循环刷日志。
void PwrShutdownPulseTask(void* /*arg*/) {
#if defined(CONFIG_IDF_TARGET_ESP32S31)
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
    static bool shutting_down = false;
    if (shutting_down) {
        return;
    }
    shutting_down = true;

    // ESP_LOGW(TAG_HOME, "%s????PWR_KEY_PULSE ????", reason);
    IdlePower_Stop();
    ShowShutdownScreen();
    xTaskCreate(PwrShutdownPulseTask, "pwr_off_pulse", 2048, nullptr, 5, nullptr);
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
    constexpr int kBtnSize     = 180;
    constexpr int kBtnIconSize = 96;

    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, kBtnSize, kBtnSize);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);

    lv_obj_set_style_bg_color(btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_10, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 24, LV_PART_MAIN);
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
    lv_obj_set_style_text_font(lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_pad_top(lbl, 12, LV_PART_MAIN);
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

    // ---- ???? ----
    constexpr int kCardW = 480;
    constexpr int kCardH = 360;
    lv_obj_t* card = lv_obj_create(mask);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, kCardW, kCardH);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1B2030), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 24, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    // card ?? clickable ???????
// card ????????card ??????    // ???? mask ????????
lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    s_pwr_dlg.card = card;

    lv_obj_t* title = lv_label_create(card);
    // lv_label_set_text(title, I18n::T("????"));
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE);

    // ---- ????----
    lv_obj_t* row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 32, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    CreatePowerActionBtn(row, "A:ic_s_home_reboot.spng", I18n::T("重启"), OnPwrRebootClicked);
    CreatePowerActionBtn(row, "A:ic_s_home_power.spng", I18n::T("关机"), OnPwrShutdownClicked);

    // ---- ??????????????----
    lv_obj_t* hint = lv_label_create(card);
    // lv_label_set_text(hint, I18n::T("??????5 ??????"));
    lv_obj_set_style_text_color(hint, lv_color_hex(0x9CA3AF), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(hint, LV_OBJ_FLAG_CLICKABLE);
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

}  // namespace

void HomeScreen::ShowPowerOptionsDialog() { ShowPowerDialog(); }

lv_obj_t* HomeScreen::Create() {
    // ????????NVS ?????? id ??kApps ??icon_suffix ????
    // ??????s_icon_paths ??????CreateAppCell ??????????
EnsureIconPathsBuilt();

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
                        nullptr);
    lv_obj_add_event_cb(screen, OnScreenDeleted, LV_EVENT_DELETE, state);

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
lv_async_call(OnRefreshStatusBarAsync, nullptr);
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
