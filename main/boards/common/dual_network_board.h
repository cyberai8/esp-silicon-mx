#ifndef DUAL_NETWORK_BOARD_H
#define DUAL_NETWORK_BOARD_H

#include "board.h"
#include <memory>

enum class NetworkType {
    WIFI,
    ML307
};

// ESP-Show 仅 WiFi；保留 stub 供 network_screen 等 dynamic_cast 编译通过。
class DualNetworkBoard : public Board {
public:
    ~DualNetworkBoard() override = default;
    static NetworkType LoadNetworkTypeFromSettings(int32_t /*default_net_type*/) {
        return NetworkType::WIFI;
    }
    void SwitchNetworkType() {}
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

#endif  // DUAL_NETWORK_BOARD_H
