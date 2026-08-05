#ifndef DUAL_NETWORK_BOARD_H
#define DUAL_NETWORK_BOARD_H

#include "board.h"
#include <memory>

enum class NetworkType {
    WIFI,
    ML307
};

#if defined(CONFIG_IDF_TARGET_ESP32S31) || defined(CONFIG_BOARD_TYPE_ESP_VOCAT)

// WiFi-only boards (no DualNetworkBoard instances). Stub keeps dynamic_cast OK.
class DualNetworkBoard : public Board {
public:
    ~DualNetworkBoard() override = default;
    static NetworkType LoadNetworkTypeFromSettings(int32_t /*default_net_type*/) {
        return NetworkType::WIFI;
    }
    void SwitchNetworkType() {
        // WiFi-only board; keep stub so network_screen compiles.
    }
    NetworkType GetNetworkType() const { return NetworkType::WIFI; }
    Board& GetCurrentBoard() const { return *const_cast<DualNetworkBoard*>(this); }
    std::string GetBoardType() override { return "dual-stub"; }
    void StartNetwork() override {}
    NetworkInterface* GetNetwork() override { return nullptr; }
    void SetPowerSaveMode(bool) override {}
    AudioCodec* GetAudioCodec() override { return nullptr; }
    std::string GetBoardJson() override { return "{}"; }
    std::string GetDeviceStatusJson() override { return "{}"; }
    const char* GetNetworkStateIcon() override { return ""; }
};

#else

#include "wifi_board.h"
#include "ml307_board.h"
#include "nt26_board.h"

class DualNetworkBoard : public Board {
private:
    std::unique_ptr<Board> current_board_;
    NetworkType network_type_ = NetworkType::ML307;

    gpio_num_t ml307_tx_pin_;
    gpio_num_t ml307_rx_pin_;
    gpio_num_t ml307_dtr_pin_;

    gpio_num_t cellular_tx_pin_;
    gpio_num_t cellular_rx_pin_;
    gpio_num_t cellular_dtr_pin_;
    gpio_num_t cellular_ri_pin_;

    void SaveNetworkTypeToSettings(NetworkType type);
    void InitializeCurrentBoard();

public:
    DualNetworkBoard(gpio_num_t ml307_tx_pin, gpio_num_t ml307_rx_pin, gpio_num_t ml307_dtr_pin = GPIO_NUM_NC, int32_t default_net_type = 1);

    DualNetworkBoard(gpio_num_t cellular_tx_pin,
        gpio_num_t cellular_rx_pin,
        gpio_num_t cellular_dtr_pin,
        gpio_num_t cellular_ri_pin,
        int32_t default_net_type);

    virtual ~DualNetworkBoard() = default;

    static NetworkType LoadNetworkTypeFromSettings(int32_t default_net_type);
    void SwitchNetworkType();
    NetworkType GetNetworkType() const { return network_type_; }
    Board& GetCurrentBoard() const { return *current_board_; }

    virtual std::string GetBoardType() override;
    virtual void StartNetwork() override;
    virtual NetworkInterface* GetNetwork() override;
    virtual const char* GetNetworkStateIcon() override;
    virtual void SetPowerSaveMode(bool enabled) override;
    virtual std::string GetBoardJson() override;
    virtual std::string GetDeviceStatusJson() override;
};

#endif // CONFIG_IDF_TARGET_ESP32S31 || CONFIG_BOARD_TYPE_ESP_VOCAT

#endif // DUAL_NETWORK_BOARD_H
