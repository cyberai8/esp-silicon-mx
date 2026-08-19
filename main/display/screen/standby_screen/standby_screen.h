#pragma once

#include "lvgl.h"
#include "screen_util.h"

#include <cstdint>

enum class StandbyFace : uint8_t {
    Weather = 0,
    Clock = 1,
};

class StandbyScreen {
public:
    // 全屏待机页：天气（默认）/ 翻页时钟，左右滑动切换；点击或侧面键短按返回首页。
    static lv_obj_t* Create();
    static void LifecycleCallback(screen_lifecycle_event_t event);

    // 切到待机页（须在 LVGL 线程调用）。
    static void Show();
    // 从待机回到首页（须在 LVGL 线程调用）。
    static void ReturnHome();
    static bool IsActive();

    // 记住上次选择的待机界面；缺省为天气。
    static StandbyFace GetPreferredFace();
    static void SetPreferredFace(StandbyFace face);
};
