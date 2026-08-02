#ifndef NT26_BOARD_H
#define NT26_BOARD_H

#include <memory>
#include <string>
#include <esp_err.h>
#include "board.h"

struct Nt26CeregState {
    int stat = 0;
    std::string tac;
    std::string ci;
    int AcT = -1;

    std::string ToString() const {
        std::string json = "{";
        json += "\"stat\":" + std::to_string(stat);
        if (!tac.empty()) json += ",\"tac\":\"" + tac + "\"";
        if (!ci.empty()) json += ",\"ci\":\"" + ci + "\"";
        if (AcT >= 0) json += ",\"AcT\":" + std::to_string(AcT);
        json += "}";
        return json;
    }
};

#if defined(CONFIG_IDF_TARGET_ESP32S31)

// S31 Korvo-1 has no NT26/4G modem. Keep a polymorphic stub so UI code that
// dynamic_casts Board → Nt26Board still compiles; GetNt26Board() returns null.
class Nt26Board : public Board {
public:
    Nt26Board() = default;
    ~Nt26Board() override = default;
    std::string GetBoardType() override { return "nt26-stub"; }
    void StartNetwork() override {}
    NetworkInterface* GetNetwork() override { return nullptr; }
    void SetPowerSaveMode(bool) override {}
    AudioCodec* GetAudioCodec() override { return nullptr; }
    std::string GetDeviceStatusJson() override { return "{}"; }
    std::string GetBoardJson() override { return "{}"; }
    Nt26CeregState GetRegistrationState() { return {}; }
    esp_err_t SendAtCommand(const std::string&, std::string&,
                            uint32_t = 5000, bool = false) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_err_t SendAtCommandCollectUntil(const std::string&, std::string&,
                                        uint32_t, const char*, bool = false) {
        return ESP_ERR_NOT_SUPPORTED;
    }
};

#else

#include <uart_eth_modem.h>
#include <esp_network.h>
#include <esp_pm.h>
#include <esp_timer.h>
#include "freertos/event_groups.h"

class Nt26Board : public Board {
protected:
    std::unique_ptr<UartEthModem> modem_;
    gpio_num_t tx_pin_;
    gpio_num_t rx_pin_;
    gpio_num_t dtr_pin_;
    gpio_num_t ri_pin_;
    gpio_num_t reset_pin_;

    NetworkEventCallback network_event_callback_;
    esp_pm_lock_handle_t pm_lock_cpu_max_ = nullptr;
    PowerSaveLevel current_power_level_ = PowerSaveLevel::LOW_POWER;
    esp_timer_handle_t network_ready_timer_ = nullptr;
    EventGroupHandle_t network_wait_event_ = nullptr;

    virtual std::string GetBoardJson() override;

    void OnNetworkEvent(NetworkEvent event, const std::string& data = "");
    static void OnNetworkReadyTimeout(void* arg);
    void ScheduleAsyncStop();

public:
    Nt26Board(gpio_num_t tx_pin, gpio_num_t rx_pin, gpio_num_t dtr_pin, gpio_num_t ri_pin, gpio_num_t reset_pin = GPIO_NUM_NC);
    virtual ~Nt26Board();
    virtual std::string GetBoardType() override;
    virtual void StartNetwork() override;
    virtual void SetNetworkEventCallback(NetworkEventCallback callback) override;
    virtual NetworkInterface* GetNetwork() override;
    virtual void SetPowerSaveMode(bool enabled) override;
    virtual const char* GetNetworkStateIcon() override;
    virtual AudioCodec* GetAudioCodec() override { return nullptr; }
    virtual std::string GetDeviceStatusJson() override;
    Nt26CeregState GetRegistrationState();

    esp_err_t SendAtCommand(const std::string& cmd, std::string& response,
                            uint32_t timeout_ms = 5000,
                            bool bypass_init_check = false);

    esp_err_t SendAtCommandCollectUntil(const std::string& cmd,
                                        std::string& response,
                                        uint32_t timeout_ms,
                                        const char* done_marker,
                                        bool bypass_init_check = false);
};

#endif // CONFIG_IDF_TARGET_ESP32S31

#endif // NT26_BOARD_H
