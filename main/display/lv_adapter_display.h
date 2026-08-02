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
    // panel_if: ESP_LV_ADAPTER_PANEL_IF_MIPI_DSI（默认，Claw4）或
    // ESP_LV_ADAPTER_PANEL_IF_RGB（ESP32-S31 Korvo-1 等 RGB 屏）
    LVAdapterDisplay(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io,
                     const esp_lcd_touch_handle_t touch_handle, int width, int height,
                     esp_lv_adapter_panel_interface_t panel_if = ESP_LV_ADAPTER_PANEL_IF_MIPI_DSI);
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
