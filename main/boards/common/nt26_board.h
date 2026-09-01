#ifndef NT26_BOARD_H
#define NT26_BOARD_H

#include <esp_err.h>
#include <string>
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

// ESP-Show 无 4G 模组；保留 stub 供 UI 编译，GetNt26Board() 恒为 null。
class Nt26Board : public Board {
public:
    Nt26Board() = default;
    Nt26Board(gpio_num_t, gpio_num_t, gpio_num_t, gpio_num_t, gpio_num_t = GPIO_NUM_NC) {}
    ~Nt26Board() override = default;
    std::string GetBoardType() override { return "nt26-stub"; }
    void StartNetwork() override {}
    NetworkInterface* GetNetwork() override { return nullptr; }
    void SetPowerSaveMode(bool) override {}
    AudioCodec* GetAudioCodec() override { return nullptr; }
    const char* GetNetworkStateIcon() override { return ""; }
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

#endif  // NT26_BOARD_H
