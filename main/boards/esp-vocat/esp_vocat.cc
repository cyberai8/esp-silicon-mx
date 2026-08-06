#include "config.h"

#include "application.h"
#include "audio/codecs/box_audio_codec.h"
#include "backlight.h"
#include "bq27220_gauge.h"
#include "button.h"
#include "display/lv_adapter_display.h"
#include "dual_network_board.h"
#include "led/gpio_led.h"
#include "SdCardManager.hpp"
#include "settings.h"
#include "wifi_board.h"

#include <atomic>
#include <cstdlib>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <esp_err.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_st77916.h>
#include <esp_lcd_touch_cst816s.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "ESP-VoCat"

// Several shared sensor/test screens still use the Claw4-era symbol name.
static i2c_master_bus_handle_t s_board_i2c_bus = nullptr;

extern "C" i2c_master_bus_handle_t metalio_claw_4_get_i2c_bus() {
    return s_board_i2c_bus;
}

static const st77916_lcd_init_cmd_t vendor_specific_init_yysj[] = {
    {0xF0, (uint8_t []){0x28}, 1, 0},
    {0xF2, (uint8_t []){0x28}, 1, 0},
    {0x73, (uint8_t []){0xF0}, 1, 0},
    {0x7C, (uint8_t []){0xD1}, 1, 0},
    {0x83, (uint8_t []){0xE0}, 1, 0},
    {0x84, (uint8_t []){0x61}, 1, 0},
    {0xF2, (uint8_t []){0x82}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x01}, 1, 0},
    {0xF1, (uint8_t []){0x01}, 1, 0},
    {0xB0, (uint8_t []){0x56}, 1, 0},
    {0xB1, (uint8_t []){0x4D}, 1, 0},
    {0xB2, (uint8_t []){0x24}, 1, 0},
    {0xB4, (uint8_t []){0x87}, 1, 0},
    {0xB5, (uint8_t []){0x44}, 1, 0},
    {0xB6, (uint8_t []){0x8B}, 1, 0},
    {0xB7, (uint8_t []){0x40}, 1, 0},
    {0xB8, (uint8_t []){0x86}, 1, 0},
    {0xBA, (uint8_t []){0x00}, 1, 0},
    {0xBB, (uint8_t []){0x08}, 1, 0},
    {0xBC, (uint8_t []){0x08}, 1, 0},
    {0xBD, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x80}, 1, 0},
    {0xC1, (uint8_t []){0x10}, 1, 0},
    {0xC2, (uint8_t []){0x37}, 1, 0},
    {0xC3, (uint8_t []){0x80}, 1, 0},
    {0xC4, (uint8_t []){0x10}, 1, 0},
    {0xC5, (uint8_t []){0x37}, 1, 0},
    {0xC6, (uint8_t []){0xA9}, 1, 0},
    {0xC7, (uint8_t []){0x41}, 1, 0},
    {0xC8, (uint8_t []){0x01}, 1, 0},
    {0xC9, (uint8_t []){0xA9}, 1, 0},
    {0xCA, (uint8_t []){0x41}, 1, 0},
    {0xCB, (uint8_t []){0x01}, 1, 0},
    {0xD0, (uint8_t []){0x91}, 1, 0},
    {0xD1, (uint8_t []){0x68}, 1, 0},
    {0xD2, (uint8_t []){0x68}, 1, 0},
    {0xF5, (uint8_t []){0x00, 0xA5}, 2, 0},
    {0xDD, (uint8_t []){0x4F}, 1, 0},
    {0xDE, (uint8_t []){0x4F}, 1, 0},
    {0xF1, (uint8_t []){0x10}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x02}, 1, 0},
    {0xE0, (uint8_t []){0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
    {0xE1, (uint8_t []){0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
    {0xF0, (uint8_t []){0x10}, 1, 0},
    {0xF3, (uint8_t []){0x10}, 1, 0},
    {0xE0, (uint8_t []){0x07}, 1, 0},
    {0xE1, (uint8_t []){0x00}, 1, 0},
    {0xE2, (uint8_t []){0x00}, 1, 0},
    {0xE3, (uint8_t []){0x00}, 1, 0},
    {0xE4, (uint8_t []){0xE0}, 1, 0},
    {0xE5, (uint8_t []){0x06}, 1, 0},
    {0xE6, (uint8_t []){0x21}, 1, 0},
    {0xE7, (uint8_t []){0x01}, 1, 0},
    {0xE8, (uint8_t []){0x05}, 1, 0},
    {0xE9, (uint8_t []){0x02}, 1, 0},
    {0xEA, (uint8_t []){0xDA}, 1, 0},
    {0xEB, (uint8_t []){0x00}, 1, 0},
    {0xEC, (uint8_t []){0x00}, 1, 0},
    {0xED, (uint8_t []){0x0F}, 1, 0},
    {0xEE, (uint8_t []){0x00}, 1, 0},
    {0xEF, (uint8_t []){0x00}, 1, 0},
    {0xF8, (uint8_t []){0x00}, 1, 0},
    {0xF9, (uint8_t []){0x00}, 1, 0},
    {0xFA, (uint8_t []){0x00}, 1, 0},
    {0xFB, (uint8_t []){0x00}, 1, 0},
    {0xFC, (uint8_t []){0x00}, 1, 0},
    {0xFD, (uint8_t []){0x00}, 1, 0},
    {0xFE, (uint8_t []){0x00}, 1, 0},
    {0xFF, (uint8_t []){0x00}, 1, 0},
    {0x60, (uint8_t []){0x40}, 1, 0},
    {0x61, (uint8_t []){0x04}, 1, 0},
    {0x62, (uint8_t []){0x00}, 1, 0},
    {0x63, (uint8_t []){0x42}, 1, 0},
    {0x64, (uint8_t []){0xD9}, 1, 0},
    {0x65, (uint8_t []){0x00}, 1, 0},
    {0x66, (uint8_t []){0x00}, 1, 0},
    {0x67, (uint8_t []){0x00}, 1, 0},
    {0x68, (uint8_t []){0x00}, 1, 0},
    {0x69, (uint8_t []){0x00}, 1, 0},
    {0x6A, (uint8_t []){0x00}, 1, 0},
    {0x6B, (uint8_t []){0x00}, 1, 0},
    {0x70, (uint8_t []){0x40}, 1, 0},
    {0x71, (uint8_t []){0x03}, 1, 0},
    {0x72, (uint8_t []){0x00}, 1, 0},
    {0x73, (uint8_t []){0x42}, 1, 0},
    {0x74, (uint8_t []){0xD8}, 1, 0},
    {0x75, (uint8_t []){0x00}, 1, 0},
    {0x76, (uint8_t []){0x00}, 1, 0},
    {0x77, (uint8_t []){0x00}, 1, 0},
    {0x78, (uint8_t []){0x00}, 1, 0},
    {0x79, (uint8_t []){0x00}, 1, 0},
    {0x7A, (uint8_t []){0x00}, 1, 0},
    {0x7B, (uint8_t []){0x00}, 1, 0},
    {0x80, (uint8_t []){0x48}, 1, 0},
    {0x81, (uint8_t []){0x00}, 1, 0},
    {0x82, (uint8_t []){0x06}, 1, 0},
    {0x83, (uint8_t []){0x02}, 1, 0},
    {0x84, (uint8_t []){0xD6}, 1, 0},
    {0x85, (uint8_t []){0x04}, 1, 0},
    {0x86, (uint8_t []){0x00}, 1, 0},
    {0x87, (uint8_t []){0x00}, 1, 0},
    {0x88, (uint8_t []){0x48}, 1, 0},
    {0x89, (uint8_t []){0x00}, 1, 0},
    {0x8A, (uint8_t []){0x08}, 1, 0},
    {0x8B, (uint8_t []){0x02}, 1, 0},
    {0x8C, (uint8_t []){0xD8}, 1, 0},
    {0x8D, (uint8_t []){0x04}, 1, 0},
    {0x8E, (uint8_t []){0x00}, 1, 0},
    {0x8F, (uint8_t []){0x00}, 1, 0},
    {0x90, (uint8_t []){0x48}, 1, 0},
    {0x91, (uint8_t []){0x00}, 1, 0},
    {0x92, (uint8_t []){0x0A}, 1, 0},
    {0x93, (uint8_t []){0x02}, 1, 0},
    {0x94, (uint8_t []){0xDA}, 1, 0},
    {0x95, (uint8_t []){0x04}, 1, 0},
    {0x96, (uint8_t []){0x00}, 1, 0},
    {0x97, (uint8_t []){0x00}, 1, 0},
    {0x98, (uint8_t []){0x48}, 1, 0},
    {0x99, (uint8_t []){0x00}, 1, 0},
    {0x9A, (uint8_t []){0x0C}, 1, 0},
    {0x9B, (uint8_t []){0x02}, 1, 0},
    {0x9C, (uint8_t []){0xDC}, 1, 0},
    {0x9D, (uint8_t []){0x04}, 1, 0},
    {0x9E, (uint8_t []){0x00}, 1, 0},
    {0x9F, (uint8_t []){0x00}, 1, 0},
    {0xA0, (uint8_t []){0x48}, 1, 0},
    {0xA1, (uint8_t []){0x00}, 1, 0},
    {0xA2, (uint8_t []){0x05}, 1, 0},
    {0xA3, (uint8_t []){0x02}, 1, 0},
    {0xA4, (uint8_t []){0xD5}, 1, 0},
    {0xA5, (uint8_t []){0x04}, 1, 0},
    {0xA6, (uint8_t []){0x00}, 1, 0},
    {0xA7, (uint8_t []){0x00}, 1, 0},
    {0xA8, (uint8_t []){0x48}, 1, 0},
    {0xA9, (uint8_t []){0x00}, 1, 0},
    {0xAA, (uint8_t []){0x07}, 1, 0},
    {0xAB, (uint8_t []){0x02}, 1, 0},
    {0xAC, (uint8_t []){0xD7}, 1, 0},
    {0xAD, (uint8_t []){0x04}, 1, 0},
    {0xAE, (uint8_t []){0x00}, 1, 0},
    {0xAF, (uint8_t []){0x00}, 1, 0},
    {0xB0, (uint8_t []){0x48}, 1, 0},
    {0xB1, (uint8_t []){0x00}, 1, 0},
    {0xB2, (uint8_t []){0x09}, 1, 0},
    {0xB3, (uint8_t []){0x02}, 1, 0},
    {0xB4, (uint8_t []){0xD9}, 1, 0},
    {0xB5, (uint8_t []){0x04}, 1, 0},
    {0xB6, (uint8_t []){0x00}, 1, 0},
    {0xB7, (uint8_t []){0x00}, 1, 0},
    {0xB8, (uint8_t []){0x48}, 1, 0},
    {0xB9, (uint8_t []){0x00}, 1, 0},
    {0xBA, (uint8_t []){0x0B}, 1, 0},
    {0xBB, (uint8_t []){0x02}, 1, 0},
    {0xBC, (uint8_t []){0xDB}, 1, 0},
    {0xBD, (uint8_t []){0x04}, 1, 0},
    {0xBE, (uint8_t []){0x00}, 1, 0},
    {0xBF, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x10}, 1, 0},
    {0xC1, (uint8_t []){0x47}, 1, 0},
    {0xC2, (uint8_t []){0x56}, 1, 0},
    {0xC3, (uint8_t []){0x65}, 1, 0},
    {0xC4, (uint8_t []){0x74}, 1, 0},
    {0xC5, (uint8_t []){0x88}, 1, 0},
    {0xC6, (uint8_t []){0x99}, 1, 0},
    {0xC7, (uint8_t []){0x01}, 1, 0},
    {0xC8, (uint8_t []){0xBB}, 1, 0},
    {0xC9, (uint8_t []){0xAA}, 1, 0},
    {0xD0, (uint8_t []){0x10}, 1, 0},
    {0xD1, (uint8_t []){0x47}, 1, 0},
    {0xD2, (uint8_t []){0x56}, 1, 0},
    {0xD3, (uint8_t []){0x65}, 1, 0},
    {0xD4, (uint8_t []){0x74}, 1, 0},
    {0xD5, (uint8_t []){0x88}, 1, 0},
    {0xD6, (uint8_t []){0x99}, 1, 0},
    {0xD7, (uint8_t []){0x01}, 1, 0},
    {0xD8, (uint8_t []){0xBB}, 1, 0},
    {0xD9, (uint8_t []){0xAA}, 1, 0},
    {0xF3, (uint8_t []){0x01}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0x21, (uint8_t []){}, 0, 0},
    {0x35, (uint8_t []){0x00}, 1, 0},
    {0x11, (uint8_t []){}, 0, 0},
    {0x00, (uint8_t []){}, 0, 120},
};

class VocatPwmBacklight : public PwmBacklight {
public:
    using PwmBacklight::PwmBacklight;

    void RestoreBrightnessImmediately() {
        Settings settings("display");
        int saved_brightness = settings.GetInt("brightness", kBacklightDefaultPercent);
        if (saved_brightness < static_cast<int>(kBacklightMinPercent)) {
            saved_brightness = kBacklightMinPercent;
        } else if (saved_brightness > 100) {
            saved_brightness = 100;
        }

        if (transition_timer_ != nullptr) {
            esp_timer_stop(transition_timer_);
        }
        brightness_ = static_cast<uint8_t>(saved_brightness);
        target_brightness_ = brightness_;
        SetBrightnessImpl(brightness_);
        ESP_LOGI(TAG, "Backlight restored immediately to %d", saved_brightness);
    }
};

class EspVocat : public DualNetworkBoard {
public:
    // default_net_type=1 → 4G 优先，与 network_screen / metalio-claw-4 一致。
    EspVocat()
        : DualNetworkBoard(ML307_TX_PIN, ML307_RX_PIN, GPIO_NUM_NC, 1),
          boot_button_(BOOT_BUTTON_GPIO),
          head_touch_button_(HEAD_TOUCH_GPIO, HEAD_TOUCH_ACTIVE_LEVEL != 0) {
        ESP_LOGI(TAG, "Boot ESP-VoCat");
        InitializeMl307Enable();
        InitializeBacklightOff();
        InitializeButtons();
        InitializeI2c();
        InitializeBatteryGauge();
        StartupBatteryGate();
        InitializeSpi();
        InitializeSt77916Panel();
        InitializeCst816sTouch();
        // 上电尽快显示开机动画；4G/OTA/MQTT 在动画播放期间并行。
        EnsureUiInitialized();
    }

    void EnsureUiInitialized() override {
        if (display_ != nullptr) {
            return;
        }
        InitializeLvAdapterDisplay();
        // 等 LVGL 把 BootScreen 刷进 framebuffer，再开 panel/背光，避免闪边角。
        vTaskDelay(pdMS_TO_TICKS(80));
        if (panel_handle_ != nullptr) {
            esp_lcd_panel_disp_on_off(panel_handle_, true);
            ESP_LOGI(TAG, "LCD panel on after BootScreen");
        }
        if (auto* backlight = static_cast<VocatPwmBacklight*>(GetBacklight())) {
            backlight->RestoreBrightnessImmediately();
        }
        ESP_LOGI(TAG, "BootScreen visible");
    }

    void AppendDisplayJsonFallback(std::string& json) override {
        json += R"("display":{"monochrome":false,"width":)" +
                std::to_string(DISPLAY_WIDTH) + R"(,"height":)" +
                std::to_string(DISPLAY_HEIGHT) + R"(},)";
    }

    std::string GetBoardType() override { return "esp-vocat"; }

    AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR,
            AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    Display* GetDisplay() override { return display_; }

    Backlight* GetBacklight() override {
        static VocatPwmBacklight backlight(DISPLAY_BACKLIGHT_PIN,
                                           DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    Led* GetLed() override {
        // Must not share LEDC_CHANNEL_0 with PwmBacklight.
        static GpioLed led(SPEAKING_LED_GPIO, SPEAKING_LED_ACTIVE_LEVEL == 0,
                           LEDC_TIMER_1, LEDC_CHANNEL_1);
        return &led;
    }

    bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        return Bq27220Gauge::GetInstance().GetBatteryLevel(level, charging, discharging);
    }

    // Called from HomeScreen shutdown path: release PG2 after UI shows.
    void ReleasePowerHold() {
        ESP_LOGW(TAG, "Releasing PG2 hold GPIO%d", PG2_HOLD_GPIO);
        gpio_set_level(PG2_HOLD_GPIO, PG2_HOLD_ACTIVE_LEVEL ? 0 : 1);
    }

    void PrepareForNetworkOta() override {
        // 保持开机动画继续播放；仅略降背光，减轻与 4G HTTPS 叠载。
        if (auto* backlight = GetBacklight()) {
            backlight->SetBrightness(40, false);
            ESP_LOGI(TAG, "Backlight dimmed for OTA/network (boot anim keeps playing)");
        }
    }

    void RestoreAfterNetworkOta() override {
        FinishDeferredBootInit();
        if (auto* backlight = static_cast<VocatPwmBacklight*>(GetBacklight())) {
            backlight->RestoreBrightnessImmediately();
        }
        ESP_LOGI(TAG, "Deferred boot init done, backlight restored");
    }

private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    Button boot_button_;
    Button head_touch_button_;
    Display* display_ = nullptr;
    esp_lcd_panel_handle_t panel_handle_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_handle_ = nullptr;
    esp_lcd_touch_handle_t touch_handle_ = nullptr;
    esp_timer_handle_t backlight_restore_timer_ = nullptr;
    std::atomic_bool power_off_started_{false};
    i2c_master_dev_handle_t qmi_dev_ = nullptr;
    bool deferred_boot_init_done_ = false;

    void FinishDeferredBootInit() {
        if (deferred_boot_init_done_) {
            return;
        }
        deferred_boot_init_done_ = true;
        InitializeSdCard();
        InitializeMotion();
    }

    static constexpr uint32_t kLcdPixelClockHz = 40 * 1000 * 1000;
    static constexpr uint32_t kBacklightUiSettleDelayMs = 2500;
    static constexpr uint16_t kLowBatteryMv = 3300;

    void InitializeMl307Enable() {
        if (ML307_EN_PIN == GPIO_NUM_NC) {
            return;
        }
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = 1ULL << ML307_EN_PIN;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        ESP_ERROR_CHECK(gpio_config(&io_conf));
        // Keep modem powered so DualNetwork can switch to 4G without reboot GPIO race.
        gpio_set_level(ML307_EN_PIN, 1);
        ESP_LOGI(TAG, "ML307 EN GPIO%d=1", ML307_EN_PIN);
    }

    void InitializeBacklightOff() {
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = 1ULL << DISPLAY_BACKLIGHT_PIN;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        ESP_ERROR_CHECK(gpio_config(&io_conf));
        gpio_set_level(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT ? 1 : 0);
        ESP_LOGI(TAG, "Backlight held off on GPIO%d", DISPLAY_BACKLIGHT_PIN);
    }

    void ScheduleBacklightRestoreAfterUiSettle() {
        if (backlight_restore_timer_ != nullptr) {
            return;
        }
        const esp_timer_create_args_t timer_args = {
            .callback =
                [](void* arg) {
                    auto* self = static_cast<EspVocat*>(arg);
                    auto* backlight =
                        static_cast<VocatPwmBacklight*>(self->GetBacklight());
                    backlight->RestoreBrightnessImmediately();
                    ESP_LOGI(TAG, "Backlight restored after LCD UI settle");
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "vocat_bl_on",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &backlight_restore_timer_));
        ESP_ERROR_CHECK(esp_timer_start_once(backlight_restore_timer_,
                                             kBacklightUiSettleDelayMs * 1000));
        ESP_LOGI(TAG, "Backlight restore scheduled (%u ms)",
                 static_cast<unsigned>(kBacklightUiSettleDelayMs));
    }

    void InitializeButtons() {
        boot_button_.OnClick([]() { Application::GetInstance().ToggleChatState(); });
        boot_button_.OnDoubleClick([this]() {
            ESP_LOGW(TAG, "BOOT double click: reset Wi-Fi configuration");
            if (GetNetworkType() == NetworkType::WIFI) {
                static_cast<WifiBoard&>(GetCurrentBoard()).ResetWifiConfiguration();
            }
        });

        if (HEAD_TOUCH_GPIO != GPIO_NUM_NC) {
            head_touch_button_.OnClick([]() {
                Application::GetInstance().ToggleChatState();
            });
            ESP_LOGI(TAG, "Head touch GPIO%d ready", HEAD_TOUCH_GPIO);
        }
    }

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_config = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {.enable_internal_pullup = 1},
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &i2c_bus_));
        s_board_i2c_bus = i2c_bus_;
        ESP_LOGI(TAG, "I2C ready");
    }

    void InitializeBatteryGauge() {
        if (Bq27220Gauge::GetInstance().Begin(i2c_bus_)) {
            ESP_LOGI(TAG, "BQ27220 gauge ready");
        } else {
            ESP_LOGW(TAG, "BQ27220 not found (optional)");
        }
    }

    void StartupBatteryGate() {
        uint16_t mv = 0;
        if (!Bq27220Gauge::GetInstance().GetVoltageMv(mv)) {
            return;
        }
        int16_t ma = 0;
        Bq27220Gauge::GetInstance().ReadCurrentMa(ma);
        ESP_LOGI(TAG, "Startup battery %umV %dmA", mv, ma);
        if (mv < kLowBatteryMv && ma <= 5) {
            ESP_LOGW(TAG, "Low battery at boot → soft shutdown");
            // Delay so log flushes; UI may not be up yet.
            vTaskDelay(pdMS_TO_TICKS(200));
            ReleasePowerHold();
            for (;;) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
    }

    void InitializeSpi() {
        const spi_bus_config_t bus_config = {
            .data0_io_num = QSPI_PIN_NUM_LCD_DATA0,
            .data1_io_num = QSPI_PIN_NUM_LCD_DATA1,
            .sclk_io_num = QSPI_PIN_NUM_LCD_PCLK,
            .data2_io_num = QSPI_PIN_NUM_LCD_DATA2,
            .data3_io_num = QSPI_PIN_NUM_LCD_DATA3,
            .data4_io_num = -1,
            .data5_io_num = -1,
            .data6_io_num = -1,
            .data7_io_num = -1,
            .data_io_default_level = false,
            .max_transfer_sz = QSPI_LCD_H_RES * QSPI_LCD_V_RES * sizeof(uint16_t),
            .flags = 0,
            .isr_cpu_id = ESP_INTR_CPU_AFFINITY_AUTO,
            .intr_flags = 0,
        };
        ESP_ERROR_CHECK(spi_bus_initialize(QSPI_LCD_HOST, &bus_config, SPI_DMA_CH_AUTO));
        ESP_LOGI(TAG, "QSPI SPI bus ready");
    }

    void InitializeSt77916Panel() {
        const esp_lcd_panel_io_spi_config_t io_config = {
            .cs_gpio_num = QSPI_PIN_NUM_LCD_CS,
            .dc_gpio_num = -1,
            .spi_mode = 0,
            .pclk_hz = kLcdPixelClockHz,
            .trans_queue_depth = 16,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .lcd_cmd_bits = 32,
            .lcd_param_bits = 8,
            .flags = {.quad_mode = true},
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
            static_cast<esp_lcd_spi_bus_handle_t>(QSPI_LCD_HOST), &io_config,
            &panel_io_handle_));

        st77916_vendor_config_t vendor_config = {
            .init_cmds = vendor_specific_init_yysj,
            .init_cmds_size =
                sizeof(vendor_specific_init_yysj) / sizeof(st77916_lcd_init_cmd_t),
            .flags = {.use_qspi_interface = 1},
        };
        const esp_lcd_panel_dev_config_t panel_config = {
            .reset_gpio_num = QSPI_PIN_NUM_LCD_RST,
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
            .bits_per_pixel = QSPI_LCD_BIT_PER_PIXEL,
            .flags = {.reset_active_high = false},
            .vendor_config = &vendor_config,
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_st77916(panel_io_handle_, &panel_config,
                                                   &panel_handle_));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle_));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle_));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle_, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle_, DISPLAY_MIRROR_X,
                                             DISPLAY_MIRROR_Y));
        ESP_LOGI(TAG, "ST77916 panel ready %dx%d pclk=%uMHz TE=GPIO%d",
                 DISPLAY_WIDTH, DISPLAY_HEIGHT,
                 static_cast<unsigned>(kLcdPixelClockHz / 1000000),
                 QSPI_PIN_NUM_LCD_TE);
    }

    void InitializeCst816sTouch() {
        esp_lcd_panel_io_handle_t tp_io_handle = nullptr;
        const esp_lcd_panel_io_i2c_config_t tp_io_config = {
            .dev_addr = ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .control_phase_bytes = 1,
            .dc_bit_offset = 0,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 0,
            .flags = {.dc_low_on_data = 0, .disable_control_phase = 1},
            .scl_speed_hz = 100000,
        };
        esp_err_t err =
            esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "CST816S panel_io failed: %s", esp_err_to_name(err));
            return;
        }

        const esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH,
            .y_max = DISPLAY_HEIGHT,
            .rst_gpio_num = TP_PIN_NUM_RST,
            .int_gpio_num = TP_PIN_NUM_INT,
            .levels = {.reset = 0, .interrupt = 0},
            .flags = {.swap_xy = DISPLAY_SWAP_XY,
                      .mirror_x = DISPLAY_MIRROR_X,
                      .mirror_y = DISPLAY_MIRROR_Y},
        };
        err = esp_lcd_touch_new_i2c_cst816s(tp_io_handle, &tp_cfg, &touch_handle_);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "CST816S init failed: %s", esp_err_to_name(err));
            touch_handle_ = nullptr;
            return;
        }
        ESP_LOGI(TAG, "CST816S touch ready");
    }

    void InitializeLvAdapterDisplay() {
        // TE_SYNC 在本板 QSPI 上会 spi transmit (queue) color failed → 黑屏。
        // 先用 NONE + strip 刷新把 UI 跑通；撕裂可后续再调。
        display_ = new LVAdapterDisplay(panel_handle_, panel_io_handle_, touch_handle_,
                                        DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                        ESP_LV_ADAPTER_PANEL_IF_OTHER, nullptr);
        ESP_LOGI(TAG, "LVAdapterDisplay Home UI ready (360 round, te=off)");
    }

    void InitializeSdCard() {
        if (!SdCardManager::GetInstance().Mount()) {
            ESP_LOGW(TAG, "SD card mount failed (optional)");
        } else {
            ESP_LOGI(TAG, "SD card mounted");
        }
    }

    bool ProbeQmi8658(uint8_t addr) {
        if (i2c_master_probe(i2c_bus_, addr, 50) != ESP_OK) {
            return false;
        }
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addr,
            .scl_speed_hz = 400000,
        };
        if (i2c_master_bus_add_device(i2c_bus_, &dev_cfg, &qmi_dev_) != ESP_OK) {
            return false;
        }
        uint8_t reg = 0x00;  // WHO_AM_I
        uint8_t who = 0;
        if (i2c_master_transmit_receive(qmi_dev_, &reg, 1, &who, 1, 50) != ESP_OK) {
            i2c_master_bus_rm_device(qmi_dev_);
            qmi_dev_ = nullptr;
            return false;
        }
        // QMI8658A WHO_AM_I is typically 0x05
        if (who != 0x05) {
            ESP_LOGW(TAG, "QMI8658 WHO_AM_I=0x%02X at 0x%02X (unexpected)", who, addr);
        }
        ESP_LOGI(TAG, "QMI8658A detected at 0x%02X who=0x%02X", addr, who);
        return true;
    }

    void InitializeMotion() {
        // Light probe only; shake UX can be expanded later.
        const uint8_t addrs[] = {0x6A, 0x6B};
        for (uint8_t addr : addrs) {
            if (ProbeQmi8658(addr)) {
                xTaskCreate(
                    [](void* arg) {
                        auto* self = static_cast<EspVocat*>(arg);
                        uint8_t reg = 0x35;  // AX_L
                        int16_t prev_ax = 0;
                        for (;;) {
                            uint8_t raw[6] = {};
                            if (self->qmi_dev_ != nullptr &&
                                i2c_master_transmit_receive(self->qmi_dev_, &reg, 1, raw,
                                                            6, 50) == ESP_OK) {
                                const int16_t ax =
                                    static_cast<int16_t>((raw[1] << 8) | raw[0]);
                                if (prev_ax != 0) {
                                    const int delta = abs(static_cast<int>(ax) - prev_ax);
                                    if (delta > 12000) {
                                        ESP_LOGI(TAG, "QMI8658 shake score=%d", delta);
                                        Application::GetInstance().ToggleChatState();
                                        vTaskDelay(pdMS_TO_TICKS(1500));
                                    }
                                }
                                prev_ax = ax;
                            }
                            vTaskDelay(pdMS_TO_TICKS(80));
                        }
                    },
                    "vocat_qmi", 3072, this, 3, nullptr);
                return;
            }
        }
        ESP_LOGW(TAG, "QMI8658A not found");
    }
};

// HomeScreen soft-shutdown hook for PG2 latch boards.
extern "C" void board_release_power_hold_if_supported() {
    auto* board = dynamic_cast<EspVocat*>(&Board::GetInstance());
    if (board != nullptr) {
        board->ReleasePowerHold();
    }
}

DECLARE_BOARD(EspVocat);
