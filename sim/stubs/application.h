#pragma once

#include "device_state.h"

#include <cstdio>
#include <functional>
#include <string>
#include <string_view>

class AudioService {
public:
    bool ReleaseWakeWordDetection() { return false; }
    void EnableWakeWordDetection(bool /*enable*/) {}
};

class Application {
public:
    static Application& GetInstance() {
        static Application inst;
        return inst;
    }

    void PlaySound(const std::string_view& sound) {
        fprintf(stderr, "[sim] PlaySound(%.*s)\n",
                static_cast<int>(sound.size()), sound.data());
    }

    // 真机异步投递到主循环；仿真里直接跑，方便闹钟铃声相关路径能编过。
    void Schedule(std::function<void()> callback) {
        if (callback) {
            callback();
        }
    }

    bool HasPendingActivation() const { return false; }
    const std::string& GetPendingActivationCode() const { return code_; }
    bool IsBackgroundNetworkReady() const { return true; }
    DeviceState GetDeviceState() const { return kDeviceStateIdle; }
    AudioService& GetAudioService() { return audio_; }

private:
    Application() = default;
    std::string code_;
    AudioService audio_;
};
