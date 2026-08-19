#include "wifi_board.h"

#include "application.h"
#include "assets/lang_config.h"
#include "backlight.h"
#include "display.h"
#include "settings.h"
#include "system_info.h"

#include <esp_log.h>
#include <esp_network.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <font_awesome.h>
#include <ssid_manager.h>
#include <wifi_configuration_ap.h>
#include <wifi_station.h>
#include "afsk_demod.h"

#ifdef HAVE_LVGL
#include "esp_lv_adapter.h"
#include "wifi_config_tip_screen/wifi_config_tip_screen.h"
#endif

static const char* TAG = "WifiBoard";

WifiBoard::WifiBoard() {
    Settings settings("wifi", true);
    wifi_config_mode_ = settings.GetInt("force_ap") == 1;
    if (wifi_config_mode_) {
        ESP_LOGI(TAG, "force_ap is set to 1, reset to 0");
        settings.SetInt("force_ap", 0);
    }
}

std::string WifiBoard::GetBoardType() { return "wifi"; }

void WifiBoard::EnterWifiConfigMode() {
    auto& application = Application::GetInstance();
    application.SetDeviceState(kDeviceStateWifiConfiguring);

    auto& wifi_ap = WifiConfigurationAp::GetInstance();
    wifi_ap.SetLanguage(Lang::CODE);
    wifi_ap.SetSsidPrefix("Xiaozhi");
    wifi_ap.Start();

    // Wait 1.5 seconds to display board information
    vTaskDelay(pdMS_TO_TICKS(1500));

    // Display WiFi configuration AP SSID and web server URL
    const std::string ssid = wifi_ap.GetSsid();
    const std::string url = wifi_ap.GetWebServerUrl();
    std::string hint = Lang::Strings::CONNECT_TO_HOTSPOT;
    hint += ssid;
    hint += Lang::Strings::ACCESS_VIA_BROWSER;
    hint += url;

    // LVGL 板：结束开机动画，单独全屏展示配网提示（Boot 时 Chat 屏未激活，
    // Alert→SetChatMessage 会被丢弃）。同时跳过配网提示音，避免与 WiFi AP
    // 叠载触发欠压复位（VoCat 上尤为明显）。
    bool tip_shown = false;
#ifdef HAVE_LVGL
    if (esp_lv_adapter_is_initialized()) {
        WifiConfigTipScreen::Show(Lang::Strings::WIFI_CONFIG_MODE, ssid.c_str(),
                                  url.c_str());
        tip_shown = true;
        if (auto* backlight = Board::GetInstance().GetBacklight()) {
            backlight->RestoreBrightness();
        }
    }
#endif
    if (tip_shown) {
        ESP_LOGW(TAG, "Alert [gear] %s: %s", Lang::Strings::WIFI_CONFIG_MODE,
                 hint.c_str());
    } else {
        application.Alert(Lang::Strings::WIFI_CONFIG_MODE, hint.c_str(), "gear",
                          Lang::Sounds::OGG_WIFICONFIG);
    }

#if CONFIG_USE_ACOUSTIC_WIFI_PROVISIONING
    auto display = Board::GetInstance().GetDisplay();
    auto codec = Board::GetInstance().GetAudioCodec();
    int channel = 1;
    if (codec) {
        channel = codec->input_channels();
    }
    ESP_LOGI(TAG, "Start receiving WiFi credentials from audio, input channels: %d", channel);
    audio_wifi_config::ReceiveWifiCredentialsFromAudio(&application, &wifi_ap, display, channel);
#endif

    // Wait forever until reset after configuration
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void WifiBoard::StartNetwork() {
    // User can press BOOT button while starting to enter WiFi configuration mode
    if (wifi_config_mode_) {
        EnterWifiConfigMode();
        return;
    }

    auto& ssid_manager = SsidManager::GetInstance();
    auto ssid_list = ssid_manager.GetSsidList();
#if (CONFIG_BOARD_TYPE_ESP_VOCAT || CONFIG_BOARD_TYPE_WAVESHARE_S3_TOUCH_LCD_1_85B)
    // 圆屏/VoCat：无 SSID 时不要卡在配网热点，直接进菜单，稍后在网络页配置。
    // 但仍要把 STA 驱动在 AFE 之前拉起：WiFi RX DMA 必须走内部 RAM，
    // 进网络页再 esp_wifi_init 会 malloc buffer fail。
    if (ssid_list.empty()) {
        ESP_LOGW(TAG, "No WiFi SSID configured, skip AP mode and continue offline");
        WifiStation::GetInstance().Start();
        ESP_LOGI(TAG, "WiFi station started (offline, for later scan)");
        return;
    }
#else
    if (ssid_list.empty()) {
        wifi_config_mode_ = true;
        EnterWifiConfigMode();
        return;
    }
#endif

    auto& wifi_station = WifiStation::GetInstance();
#if !(CONFIG_BOARD_TYPE_ESP_VOCAT || CONFIG_BOARD_TYPE_WAVESHARE_S3_TOUCH_LCD_1_85B)
    wifi_station.OnScanBegin([this]() {
        auto display = Board::GetInstance().GetDisplay();
        display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
    });
    wifi_station.OnConnect([this](const std::string& ssid) {
        auto display = Board::GetInstance().GetDisplay();
        std::string notification = Lang::Strings::CONNECT_TO;
        notification += ssid;
        notification += "...";
        display->ShowNotification(notification.c_str(), 30000);
    });
    wifi_station.OnConnected([this](const std::string& ssid) {
        auto display = Board::GetInstance().GetDisplay();
        std::string notification = Lang::Strings::CONNECTED_TO;
        notification += ssid;
        display->ShowNotification(notification.c_str(), 30000);
    });
#endif
    wifi_station.Start();

#if (CONFIG_BOARD_TYPE_ESP_VOCAT || CONFIG_BOARD_TYPE_WAVESHARE_S3_TOUCH_LCD_1_85B)
    // 不阻塞等待联网；失败也不进配网 AP。菜单已经（或即将）显示。
    ESP_LOGI(TAG, "WiFi station started (non-blocking)");
#else
    if (!wifi_station.WaitForConnected(60 * 1000)) {
        wifi_station.Stop();
        wifi_config_mode_ = true;
        EnterWifiConfigMode();
        return;
    }
#endif
}

NetworkInterface* WifiBoard::GetNetwork() {
    static EspNetwork network;
    return &network;
}

const char* WifiBoard::GetNetworkStateIcon() {
    if (wifi_config_mode_) {
        return FONT_AWESOME_WIFI;
    }
    auto& wifi_station = WifiStation::GetInstance();
    if (!wifi_station.IsConnected()) {
        return FONT_AWESOME_WIFI_SLASH;
    }
    int8_t rssi = wifi_station.GetRssi();
    if (rssi >= -60) {
        return FONT_AWESOME_WIFI;
    } else if (rssi >= -70) {
        return FONT_AWESOME_WIFI_FAIR;
    } else {
        return FONT_AWESOME_WIFI_WEAK;
    }
}

std::string WifiBoard::GetBoardJson() {
    // Set the board type for OTA
    auto& wifi_station = WifiStation::GetInstance();
    std::string board_json = R"({)";
    board_json += R"("type":")" + std::string(BOARD_TYPE) + R"(",)";
    board_json += R"("name":")" + std::string(BOARD_NAME) + R"(",)";
    if (!wifi_config_mode_) {
        const std::string& ssid = wifi_station.GetSsid();
        if (!ssid.empty()) {
            board_json += R"("ssid":")" + ssid + R"(",)";
        }
        if (wifi_station.IsConnected()) {
            board_json += R"("rssi":)" + std::to_string(wifi_station.GetRssi()) + R"(,)";
            board_json += R"("channel":)" + std::to_string(wifi_station.GetChannel()) + R"(,)";
            board_json += R"("ip":")" + wifi_station.GetIpAddress() + R"(",)";
        }
    }
    board_json += R"("mac":")" + SystemInfo::GetMacAddress() + R"(")";
    board_json += R"(})";
    return board_json;
}

void WifiBoard::SetPowerSaveMode(bool enabled) {
    auto& wifi_station = WifiStation::GetInstance();
    wifi_station.SetPowerSaveMode(enabled);
}

void WifiBoard::ResetWifiConfiguration() {
    // Set a flag and reboot the device to enter the network configuration mode
    {
        Settings settings("wifi", true);
        settings.SetInt("force_ap", 1);
    }
    GetDisplay()->ShowNotification(Lang::Strings::ENTERING_WIFI_CONFIG_MODE);
    vTaskDelay(pdMS_TO_TICKS(1000));
    // Reboot the device
    esp_restart();
}

std::string WifiBoard::GetDeviceStatusJson() {
    /*
     * Return device status JSON
     *
     * The returned JSON structure is as follows:
     * {
     *     "audio_speaker": {
     *         "volume": 70
     *     },
     *     "screen": {
     *         "brightness": 100,
     *         "theme": "light"
     *     },
     *     "battery": {
     *         "level": 50,
     *         "charging": true
     *     },
     *     "network": {
     *         "type": "wifi",
     *         "ssid": "Xiaozhi",
     *         "rssi": -60
     *     },
     *     "chip": {
     *         "temperature": 25
     *     }
     * }
     */
    auto& board = Board::GetInstance();
    auto root = cJSON_CreateObject();

    // Audio speaker
    auto audio_speaker = cJSON_CreateObject();
    auto audio_codec = board.GetAudioCodec();
    if (audio_codec) {
        cJSON_AddNumberToObject(audio_speaker, "volume", audio_codec->output_volume());
    }
    cJSON_AddItemToObject(root, "audio_speaker", audio_speaker);

    // Screen brightness
    auto backlight = board.GetBacklight();
    auto screen = cJSON_CreateObject();
    if (backlight) {
        cJSON_AddNumberToObject(screen, "brightness", backlight->brightness());
    }
    auto display = board.GetDisplay();
    if (display && display->height() > 64) {  // For LCD display only
        auto theme = display->GetTheme();
        if (theme != nullptr) {
            cJSON_AddStringToObject(screen, "theme", theme->name().c_str());
        }
    }
    cJSON_AddItemToObject(root, "screen", screen);

    // Battery
    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        cJSON* battery = cJSON_CreateObject();
        cJSON_AddNumberToObject(battery, "level", battery_level);
        cJSON_AddBoolToObject(battery, "charging", charging);
        cJSON_AddItemToObject(root, "battery", battery);
    }

    // Network
    auto network = cJSON_CreateObject();
    auto& wifi_station = WifiStation::GetInstance();
    cJSON_AddStringToObject(network, "type", "wifi");
    const std::string& ssid = wifi_station.GetSsid();
    if (!ssid.empty()) {
        cJSON_AddStringToObject(network, "ssid", ssid.c_str());
    }
    if (wifi_station.IsConnected()) {
        int rssi = wifi_station.GetRssi();
        if (rssi >= -60) {
            cJSON_AddStringToObject(network, "signal", "strong");
        } else if (rssi >= -70) {
            cJSON_AddStringToObject(network, "signal", "medium");
        } else {
            cJSON_AddStringToObject(network, "signal", "weak");
        }
    } else {
        cJSON_AddStringToObject(network, "signal", "disconnected");
    }
    cJSON_AddItemToObject(root, "network", network);

    // Chip
    float esp32temp = 0.0f;
    if (board.GetTemperature(esp32temp)) {
        auto chip = cJSON_CreateObject();
        cJSON_AddNumberToObject(chip, "temperature", esp32temp);
        cJSON_AddItemToObject(root, "chip", chip);
    }

    auto json_str = cJSON_PrintUnformatted(root);
    std::string json(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return json;
}
