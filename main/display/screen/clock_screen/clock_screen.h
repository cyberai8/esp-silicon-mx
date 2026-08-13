#pragma once

#include "lvgl.h"
#include "screen_util.h"

// 时钟应用：闹钟 / 计时 / 倒计时（圆屏三 Tab，交互对齐预览图）。
class ClockScreen {
public:
    static lv_obj_t* Create();
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
