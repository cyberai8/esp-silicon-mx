#include "application.h"
#include "bsp_adc_calibration.h"
#include "button.h"
#include "config.h"
#include "display/lv_adapter_display.h"
#include "led/single_led.h"
#include "SdCardManager.hpp"
#include "wifi_board.h"

#include "audio/codecs/es8389_audio_codec.h"

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_lcd_touch_gt1151.h>
#include <esp_log.h>
#include <esp_lv_adapter.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "Esp32S31Korvo1"

// UI / sensor screens still call the Claw4-era symbol name.
static i2c_master_bus_handle_t s_board_i2c_bus = nullptr;

extern "C" i2c_master_bus_handle_t metalio_claw_4_get_i2c_bus() {
    return s_board_i2c_bus;
}

// GPIO42 电阻分压网络上的 4 个 ADC 按键（官方小智板级算法）
typedef enum {
    BSP_ADC_BUTTON_SET,
    BSP_ADC_BUTTON_MODE,
    BSP_ADC_BUTTON_VOL_DOWN,
    BSP_ADC_BUTTON_VOL_UP,
    BSP_ADC_BUTTON_NUM
} bsp_adc_button_t;

typedef struct {
    button_driver_t base;
    uint16_t min_mv;
    uint16_t max_mv;
} korvo_adc_button_driver_t;

typedef struct {
    bsp_adc_button_t button;
    uint16_t center_mv;
} korvo_adc_button_ladder_t;

static adc_oneshot_unit_handle_t s_button_adc_handle = nullptr;
static korvo_adc_button_driver_t s_button_drivers[BSP_ADC_BUTTON_NUM];
static const korvo_adc_button_ladder_t s_button_ladder[BSP_ADC_BUTTON_NUM] = {
    {BSP_ADC_BUTTON_VOL_UP, ADC_BUTTON_VOL_UP_MV},
    {BSP_ADC_BUTTON_VOL_DOWN, ADC_BUTTON_VOL_DOWN_MV},
    {BSP_ADC_BUTTON_MODE, ADC_BUTTON_MODE_MV},
    {BSP_ADC_BUTTON_SET, ADC_BUTTON_SET_MV},
};

static uint16_t ButtonMvMidpoint(uint16_t a, uint16_t b) {
    return (uint16_t)(((uint32_t)a + (uint32_t)b) / 2U);
}

static uint8_t KorvoAdcButtonGetKeyLevel(button_driver_t* driver) {
    auto* btn = reinterpret_cast<korvo_adc_button_driver_t*>(driver);
    uint32_t raw_sum = 0;
    int raw = 0;
    for (int i = 0; i < 4; i++) {
        if (adc_oneshot_read(s_button_adc_handle, ADC_CHANNEL_0, &raw) != ESP_OK) {
            return BUTTON_INACTIVE;
        }
        raw_sum += raw;
    }
    int mv = 0;
    if (bsp_s31_adc_calibration_raw_to_mv(ADC_UNIT_1, (int)(raw_sum / 4), &mv) != ESP_OK) {
        return BUTTON_INACTIVE;
    }
    return (mv >= btn->min_mv && mv <= btn->max_mv) ? BUTTON_ACTIVE : BUTTON_INACTIVE;
}

class Esp32S31Korvo1 : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_ = nullptr;
    adc_oneshot_unit_handle_t adc_handle_ = nullptr;
    Button boot_button_;
    Button* adc_button_[BSP_ADC_BUTTON_NUM] = {};
    Display* display_ = nullptr;
    esp_lcd_touch_handle_t touch_handle_ = nullptr;
    esp_lcd_panel_handle_t panel_handle_ = nullptr;

    void InitializeCodecI2c() {
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
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &codec_i2c_bus_));
        s_board_i2c_bus = codec_i2c_bus_;
    }

    void ChangeVol(int delta) {
        auto codec = GetAudioCodec();
        if (codec == nullptr) {
            return;
        }
        int volume = codec->output_volume() + delta;
        if (volume > 100) {
            volume = 100;
        }
        if (volume < 0) {
            volume = 0;
        }
        codec->SetOutputVolume(volume);
        ESP_LOGI(TAG, "Volume: %d", volume);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        const adc_oneshot_unit_init_cfg_t adc_unit_config = {
            .unit_id = ADC_UNIT_1,
        };
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&adc_unit_config, &adc_handle_));
        const adc_oneshot_chan_cfg_t adc_chan_config = {
            .atten = ADC_ATTEN_DB_0,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle_, ADC_CHANNEL_0, &adc_chan_config));
        s_button_adc_handle = adc_handle_;
        ESP_ERROR_CHECK(bsp_s31_adc_calibration_init(adc_handle_, ADC_UNIT_1, ADC_CHANNEL_0));

        const button_config_t button_config = {};
        for (int i = 0; i < BSP_ADC_BUTTON_NUM; i++) {
            const bsp_adc_button_t button = s_button_ladder[i].button;
            const uint16_t center = s_button_ladder[i].center_mv;
            if (i == 0) {
                uint16_t upper =
                    ButtonMvMidpoint(s_button_ladder[0].center_mv, s_button_ladder[1].center_mv);
                uint16_t margin = upper - center;
                s_button_drivers[i].min_mv = center > margin ? center - margin : 0;
                s_button_drivers[i].max_mv = upper;
            } else if (i == BSP_ADC_BUTTON_NUM - 1) {
                s_button_drivers[i].min_mv =
                    ButtonMvMidpoint(s_button_ladder[i - 1].center_mv, center);
                s_button_drivers[i].max_mv = ButtonMvMidpoint(center, ADC_BUTTON_IDLE_MV);
            } else {
                s_button_drivers[i].min_mv =
                    ButtonMvMidpoint(s_button_ladder[i - 1].center_mv, center);
                s_button_drivers[i].max_mv =
                    ButtonMvMidpoint(center, s_button_ladder[i + 1].center_mv);
            }
            s_button_drivers[i].base.get_key_level = KorvoAdcButtonGetKeyLevel;
            ESP_LOGI(TAG, "ADC button %d: center=%umV range=%u-%umV", button, center,
                     s_button_drivers[i].min_mv, s_button_drivers[i].max_mv);

            button_handle_t handle = nullptr;
            ESP_ERROR_CHECK(iot_button_create(&button_config, &s_button_drivers[i].base, &handle));
            adc_button_[button] = new Button(handle);
        }

        adc_button_[BSP_ADC_BUTTON_VOL_UP]->OnClick([this]() { ChangeVol(10); });
        adc_button_[BSP_ADC_BUTTON_VOL_UP]->OnLongPress(
            [this]() { GetAudioCodec()->SetOutputVolume(100); });
        adc_button_[BSP_ADC_BUTTON_VOL_DOWN]->OnClick([this]() { ChangeVol(-10); });
        adc_button_[BSP_ADC_BUTTON_VOL_DOWN]->OnLongPress(
            [this]() { GetAudioCodec()->SetOutputVolume(0); });
        adc_button_[BSP_ADC_BUTTON_SET]->OnClick([this]() { EnterWifiConfigMode(); });
        adc_button_[BSP_ADC_BUTTON_MODE]->OnClick(
            []() { Application::GetInstance().ToggleChatState(); });
    }

    void InitializeSdCard() {
#if defined(SDMMC_PWR_EN_PIN) && (SDMMC_PWR_EN_PIN != GPIO_NUM_NC)
        gpio_config_t io_conf = {
            .pin_bit_mask = 1ULL << SDMMC_PWR_EN_PIN,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
        gpio_set_level(SDMMC_PWR_EN_PIN, SDMMC_PWR_EN_ACTIVE_LEVEL);
        vTaskDelay(pdMS_TO_TICKS(50));
#endif
        if (!SdCardManager::GetInstance().Mount()) {
            ESP_LOGW(TAG, "SD card mount failed (optional)");
        }
    }

    void InitializeRgbDisplay() {
        // 时序/bounce 与官方小智板级一致（bounce=8 行，实测更稳）
        esp_lcd_rgb_panel_config_t rgb_config = {
            .clk_src = LCD_CLK_SRC_DEFAULT,
            .timings =
                {
                    .pclk_hz = 16 * 1000 * 1000,
                    .h_res = DISPLAY_WIDTH,
                    .v_res = DISPLAY_HEIGHT,
                    .hsync_pulse_width = 4,
                    .hsync_back_porch = 8,
                    .hsync_front_porch = 8,
                    .vsync_pulse_width = 4,
                    .vsync_back_porch = 8,
                    .vsync_front_porch = 8,
                    .flags = {.pclk_active_neg = true},
                },
            .data_width = 16,
            .in_color_format = LCD_COLOR_FMT_RGB565,
            .out_color_format = LCD_COLOR_FMT_RGB565,
            // 与 LVAdapterDisplay RGB 路径 DOUBLE_DIRECT 对齐（要 2 个 FB）
            .num_fbs = 2,
            .bounce_buffer_size_px = DISPLAY_WIDTH * 8,
            .hsync_gpio_num = LCD_RGB_HSYNC,
            .vsync_gpio_num = LCD_RGB_VSYNC,
            .de_gpio_num = LCD_RGB_DE,
            .pclk_gpio_num = LCD_RGB_PCLK,
            .disp_gpio_num = LCD_RGB_DISP,
            .data_gpio_nums =
                {
                    LCD_RGB_DATA0,  LCD_RGB_DATA1,  LCD_RGB_DATA2,  LCD_RGB_DATA3,
                    LCD_RGB_DATA4,  LCD_RGB_DATA5,  LCD_RGB_DATA6,  LCD_RGB_DATA7,
                    LCD_RGB_DATA8,  LCD_RGB_DATA9,  LCD_RGB_DATA10, LCD_RGB_DATA11,
                    LCD_RGB_DATA12, LCD_RGB_DATA13, LCD_RGB_DATA14, LCD_RGB_DATA15,
                },
            .flags = {.fb_in_psram = 1},
        };

        ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&rgb_config, &panel_handle_));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle_));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle_));
        ESP_LOGI(TAG, "RGB LCD %dx%d ready", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    }

    void InitializeTouch() {
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH - 1,
            .y_max = DISPLAY_HEIGHT - 1,
            .rst_gpio_num = GPIO_NUM_NC,
            .int_gpio_num = GPIO_NUM_NC,
            .levels = {.reset = 0, .interrupt = 0},
            .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
        };

        esp_lcd_panel_io_handle_t tp_io_handle = nullptr;
        esp_lcd_panel_io_i2c_config_t tp_io_config = {};
        tp_io_config.scl_speed_hz = 400 * 1000;
        tp_io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT1151_ADDRESS;
        tp_io_config.control_phase_bytes = 1;
        tp_io_config.dc_bit_offset = 0;
        tp_io_config.lcd_cmd_bits = 16;
        tp_io_config.flags.disable_control_phase = 1;

        esp_err_t err = esp_lcd_new_panel_io_i2c(codec_i2c_bus_, &tp_io_config, &tp_io_handle);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "GT1151 panel_io failed: %s", esp_err_to_name(err));
            return;
        }
        err = esp_lcd_touch_new_i2c_gt1151(tp_io_handle, &tp_cfg, &touch_handle_);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "GT1151 init failed: %s", esp_err_to_name(err));
            touch_handle_ = nullptr;
            return;
        }
        ESP_LOGI(TAG, "GT1151 touch ready");
    }

    void InitializeDisplay() {
        // MetalioClaw4 UI 走 LVAdapterDisplay（RGB 接口）
        display_ = new LVAdapterDisplay(panel_handle_, nullptr, touch_handle_, DISPLAY_WIDTH,
                                        DISPLAY_HEIGHT, ESP_LV_ADAPTER_PANEL_IF_RGB);
    }

public:
    Esp32S31Korvo1() : boot_button_(BOOT_BUTTON_GPIO) {
        ESP_LOGI(TAG, "Boot ESP32-S31-Korvo-1 (MetalioClaw4)");
        InitializeCodecI2c();
        InitializeButtons();
        InitializeSdCard();
        InitializeRgbDisplay();
        vTaskDelay(pdMS_TO_TICKS(50));
        InitializeTouch();
        InitializeDisplay();
    }

    AudioCodec* GetAudioCodec() override {
        static Es8389AudioCodec audio_codec(
            codec_i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8389_ADDR, AUDIO_CODEC_USE_MCLK);
        return &audio_codec;
    }

    Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    Display* GetDisplay() override { return display_; }

    Backlight* GetBacklight() override { return nullptr; }
};

DECLARE_BOARD(Esp32S31Korvo1);
