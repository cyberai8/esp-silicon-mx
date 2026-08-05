#pragma once

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include "display.h"
#include "esp_lv_adapter.h"
#include "esp_lv_adapter_display.h"
#include "lvgl_font.h"

class LVAdapterDisplay : public Display {
public:
    // panel_if:
    //   MIPI_DSI — Claw4
    //   RGB      — ESP32-S31 Korvo-1
    //   OTHER    — SPI/QSPI（ESP-VoCat ST77916 等）；可选 TE GPIO
    struct SpiTeConfig {
        int te_gpio = -1;          // <0: no TE sync
        uint32_t bus_freq_hz = 40 * 1000 * 1000;
        uint8_t data_lines = 4;    // QSPI=4, SPI=1
        uint8_t bits_per_pixel = 16;
    };

    LVAdapterDisplay(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io,
                     const esp_lcd_touch_handle_t touch_handle, int width, int height,
                     esp_lv_adapter_panel_interface_t panel_if = ESP_LV_ADAPTER_PANEL_IF_MIPI_DSI,
                     const SpiTeConfig* spi_te = nullptr);
    virtual ~LVAdapterDisplay();

    virtual void SetEmotion(const char* emotion) override;
    virtual void SetStatus(const char* status) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void SetTheme(Theme* theme) override;
    virtual void ShowNotification(const char* notification, int duration_ms = 3000) override;
    virtual void UpdateStatusBar(bool update_all = false) override;
    virtual void SetPowerSaveMode(bool on) override;
    virtual void SetPreviewImage(const void* image);

private:
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;
    void SetupUI();
};
