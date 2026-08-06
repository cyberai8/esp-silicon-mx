#pragma once

#include "lvgl.h"
#include "screen_util.h"

// VoCat：SD 卡本地音乐（扫描 /sdcard 最多 5 级目录），不使用蓝牙。
class MusicScreenSd {
public:
    static lv_obj_t* Create();
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
