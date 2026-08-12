#pragma once

#include "lvgl.h"

class BootScreen {
public:
    // Create a fullscreen boot page (black background + EAF animation).
    static lv_obj_t* Create();
    // 阻塞直到开机动画播完（或已播完），须在 LVGL 任务外调用。
    static void WaitUntilAnimationFinished();
};
