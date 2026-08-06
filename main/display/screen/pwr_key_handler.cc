#include "pwr_key_handler.h"

#include <cstring>

#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esp_log.h"
#include "lvgl.h"

#include "IOExpander.hpp"
#include "application.h"
#include "home_screen/home_screen.h"
#include "i18n/i18n.h"
#include "standby_screen/standby_screen.h"

namespace {

constexpr const char* TAG = "PwrKey";
// 栈空 / 未知前台：短按不进待机（绝不能回落成 "home"，否则二级页
// 父页 UNLOAD 时会被误判为首页）。
constexpr const char* kNoneScreen = "none";
constexpr const char* kHomeScreen = "home";
constexpr uint32_t kLongPressMs = 1500;
constexpr int kMaxStack = 8;

const char* s_stack[kMaxStack] = {};
int s_depth = 0;
bool s_inited = false;
#if defined(BOARD_ESP_VOCAT)
bool s_ui_ready = false;
gpio_num_t s_vocat_gpio = GPIO_NUM_NC;
bool s_vocat_active_high = false;
constexpr int kVocatPollMs = 50;
constexpr int kVocatShortMinMs = 20;
constexpr int kVocatShortMaxMs = 2000;
#endif

bool IsChatToggleScreen(const char* name) {
    return std::strcmp(name, "chat") == 0 ||
           std::strcmp(name, "digital_people") == 0;
}

void StackPush(const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return;
    }
    if (s_depth < kMaxStack) {
        s_stack[s_depth++] = name;
    } else {
        s_stack[kMaxStack - 1] = name;
        ESP_LOGW(TAG, "screen stack full, replace top with %s", name);
    }
}

void StackRemoveTopmost(const char* name) {
    if (name == nullptr || s_depth <= 0) {
        return;
    }
    for (int i = s_depth - 1; i >= 0; --i) {
        if (std::strcmp(s_stack[i], name) == 0) {
            for (int j = i; j < s_depth - 1; ++j) {
                s_stack[j] = s_stack[j + 1];
            }
            --s_depth;
            s_stack[s_depth] = nullptr;
            return;
        }
    }
}

const char* StackTop() {
    return s_depth > 0 ? s_stack[s_depth - 1] : kNoneScreen;
}

void OnEnterStandbyAsync(void* /*arg*/) { StandbyScreen::Show(); }

void OnLeaveStandbyAsync(void* /*arg*/) { StandbyScreen::ReturnHome(); }

void OnShutdownAsync(void* /*arg*/) {
    HomeScreen::RequestSystemShutdown(I18n::T("电源键关机"));
}

void OnShortPress() {
    const char* screen = PwrKey_ActiveScreen();
    ESP_LOGI(TAG, "short-press on screen=%s (depth=%d)", screen, s_depth);

#if defined(BOARD_ESP_VOCAT)
    if (!s_ui_ready || std::strcmp(screen, kNoneScreen) == 0) {
        ESP_LOGI(TAG, "dispatch: ignored (boot/UI not ready)");
        return;
    }
    if (std::strcmp(screen, "standby") == 0) {
        ESP_LOGI(TAG, "dispatch: leave standby -> home (power on)");
        lv_async_call(OnLeaveStandbyAsync, nullptr);
        return;
    }
    ESP_LOGI(TAG, "dispatch: soft shutdown (power off)");
    lv_async_call(OnShutdownAsync, nullptr);
    return;
#endif

    if (std::strcmp(screen, kHomeScreen) == 0) {
        ESP_LOGI(TAG, "dispatch: enter standby_screen");
        lv_async_call(OnEnterStandbyAsync, nullptr);
        return;
    }

    if (std::strcmp(screen, "standby") == 0) {
        ESP_LOGI(TAG, "dispatch: leave standby -> home");
        lv_async_call(OnLeaveStandbyAsync, nullptr);
        return;
    }

    if (IsChatToggleScreen(screen)) {
        ESP_LOGI(TAG, "dispatch: ToggleChatState()");
        Application::GetInstance().ToggleChatState();
        return;
    }

    ESP_LOGI(TAG, "dispatch: no-op (screen has no short-press action)");
}

void OnLongPressAsync(void* /*arg*/) {
    HomeScreen::ShowPowerOptionsDialog();
}

void OnLongPress() {
    const char* screen = PwrKey_ActiveScreen();
    ESP_LOGI(TAG, "long-press %ums on screen=%s -> power dialog",
             static_cast<unsigned>(kLongPressMs), screen);
    lv_async_call(OnLongPressAsync, nullptr);
}

}  // namespace

void PwrKey_Init() {
    if (s_inited) {
        return;
    }

    auto& io = IOExpander::getInstance();
    const esp_err_t click_err = io.onClick(IOExpander::Pin::PWR_KEY, OnShortPress);
    if (click_err != ESP_OK) {
        ESP_LOGE(TAG, "onClick register failed: 0x%x",
                 static_cast<unsigned>(click_err));
        return;
    }

    const esp_err_t long_err =
        io.onLongPress(IOExpander::Pin::PWR_KEY, kLongPressMs, OnLongPress);
    if (long_err != ESP_OK) {
        ESP_LOGE(TAG, "onLongPress register failed: 0x%x",
                 static_cast<unsigned>(long_err));
        return;
    }

    s_inited = true;
    s_depth = 0;  // 等 HomeScreen LOAD 再入栈，避免残留假 "home"
    ESP_LOGI(TAG,
             "armed: short-press + long-press %ums (active_screen=%s)",
             static_cast<unsigned>(kLongPressMs), PwrKey_ActiveScreen());
}

#if defined(BOARD_ESP_VOCAT)
namespace {

bool GpioPowerKeyPressed(gpio_num_t gpio, bool active_high) {
    const int level = gpio_get_level(gpio);
    return active_high ? (level != 0) : (level == 0);
}

void ConfigurePowerKeyGpio(gpio_num_t gpio, bool active_high) {
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = 1ULL << gpio;
    io_conf.pull_down_en = active_high ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = active_high ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);
}

void VocatPowerKeyTask(void* /*arg*/) {
    const gpio_num_t gpio = s_vocat_gpio;
    const bool active_high = s_vocat_active_high;

    ESP_LOGI(TAG, "waiting PG1 release before arming...");
    constexpr int kMaxWaitReleaseMs = 800;
    int wait_log_ms = 0;
    while (GpioPowerKeyPressed(gpio, active_high) && wait_log_ms < kMaxWaitReleaseMs) {
        vTaskDelay(pdMS_TO_TICKS(kVocatPollMs));
        wait_log_ms += kVocatPollMs;
    }
    if (GpioPowerKeyPressed(gpio, active_high)) {
        ESP_LOGW(TAG, "PG1 still low after %dms, arm anyway (ignore boot press)",
                 kMaxWaitReleaseMs);
    } else {
        ESP_LOGI(TAG, "PG1 released, click detection armed");
    }

    // 若上电时仍读到按下，先同步为 down，避免松手/粘住沿误触发关机。
    bool down_last = GpioPowerKeyPressed(gpio, active_high);
    bool skip_release_click = down_last;
    int pressed_ms = 0;
    for (;;) {
        const bool down = GpioPowerKeyPressed(gpio, active_high);
        if (down && !down_last) {
            pressed_ms = 0;
            // 待机：按下即亮屏，不必等松手（更像「单击开机」）。
            if (s_ui_ready &&
                std::strcmp(PwrKey_ActiveScreen(), "standby") == 0) {
                ESP_LOGI(TAG, "standby wake on press");
                OnShortPress();
                skip_release_click = true;
            }
        }
        if (down) {
            pressed_ms += kVocatPollMs;
        } else if (down_last) {
            if (!skip_release_click && pressed_ms >= kVocatShortMinMs &&
                pressed_ms <= kVocatShortMaxMs) {
                OnShortPress();
            }
            skip_release_click = false;
        }
        down_last = down;
        vTaskDelay(pdMS_TO_TICKS(kVocatPollMs));
    }
}

}  // namespace

void PwrKey_InitGpio(gpio_num_t gpio, bool active_high) {
    if (s_inited || gpio == GPIO_NUM_NC) {
        return;
    }

    s_vocat_gpio = gpio;
    s_vocat_active_high = active_high;
    ConfigurePowerKeyGpio(gpio, active_high);

    if (xTaskCreate(VocatPowerKeyTask, "vocat_pwr", 3072, nullptr, 5, nullptr) !=
        pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(vocat_pwr) failed");
        return;
    }

    s_inited = true;
    s_depth = 0;
    ESP_LOGI(TAG, "GPIO power key poll on GPIO%d (active_high=%d)",
             static_cast<int>(gpio), active_high ? 1 : 0);
}
#else
void PwrKey_InitGpio(gpio_num_t gpio, bool active_high) {
    (void)gpio;
    (void)active_high;
}
#endif

void PwrKey_OnScreenLifecycle(const char* name,
                              screen_lifecycle_event_t event) {
    if (name == nullptr || name[0] == '\0') {
        name = kNoneScreen;
    }

    if (event == SCREEN_LIFECYCLE_LOAD) {
        StackPush(name);
#if defined(BOARD_ESP_VOCAT)
        if (std::strcmp(name, kHomeScreen) == 0 ||
            std::strcmp(name, "standby") == 0) {
            s_ui_ready = true;
            ESP_LOGI(TAG, "UI ready, power key clicks enabled");
        }
#endif
        ESP_LOGD(TAG, "active_screen -> %s (load %s, depth=%d)", StackTop(),
                 name, s_depth);
        return;
    }

    // UNLOAD：只移除栈里最靠上的同名页，绝不能回落成 "home"。
    // 否则 Test→AutoTest 这类「父页销毁、子页未登记」会误进待机。
    StackRemoveTopmost(name);
    ESP_LOGD(TAG, "active_screen -> %s (unload %s, depth=%d)", StackTop(), name,
             s_depth);
}

const char* PwrKey_ActiveScreen() {
    return StackTop();
}
