#pragma once

#include "lvgl.h"
#include "screen_util.h"

// 相册：读取 SD 卡里的图片，缩略图网格 + 全屏看图。
class AlbumScreen {
public:
    static lv_obj_t* Create();
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
