#pragma once

// Application 的仿真替身：真实的 application.h 会把协议、OTA、音频服务整条链
// 拉进来，UI 层其实只用到少数几个入口。这里只留 UI 会调的，声音改成打日志
// （屏上看不到声音，但能确认「该响的时候确实调了」）。
#include <cstdio>
#include <string>
#include <string_view>

class Application {
public:
    static Application& GetInstance() {
        static Application inst;
        return inst;
    }

    void PlaySound(const std::string_view& sound) {
        fprintf(stderr, "[sim] PlaySound(%.*s)\n", static_cast<int>(sound.size()), sound.data());
    }

private:
    Application() = default;
};
