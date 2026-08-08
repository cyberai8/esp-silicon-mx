#pragma once

// 配网提示页：结束开机动画后单独展示热点名与浏览器地址。
class WifiConfigTipScreen {
public:
    // title: 如「配网模式」；ssid / url 分行高亮显示。
    static void Show(const char* title, const char* ssid, const char* url);
    static bool IsActive();
};
