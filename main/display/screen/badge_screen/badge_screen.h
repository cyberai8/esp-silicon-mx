#pragma once

#include "lvgl.h"
#include "screen_util.h"

// ---------------------------------------------------------------------------
// BadgeScreen — 像章应用
//
// 从 SD 卡 /sdcard/badge/ 目录扫描 JPEG/PNG，全屏 cover 缩放铺满圆屏。
//   - 轻触：切换下一张
//   - 长按（≥600ms）：退回首页
//   - 屏幕常亮：进入应用时关闭 LVGL 自动熄屏
// ---------------------------------------------------------------------------
class BadgeScreen {
public:
    static lv_obj_t* Create();
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
