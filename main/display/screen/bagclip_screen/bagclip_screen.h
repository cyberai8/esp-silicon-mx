#pragma once

#include "lvgl.h"
#include "screen_util.h"

// ---------------------------------------------------------------------------
// BagclipScreen — 背包扣应用
//
// 从 SD 卡 /sdcard/bagclip/ 目录扫描 JPEG/PNG，全屏 cover 缩放铺满圆屏，
// 每 kSlideIntervalMs 自动切换到下一张（循环播放）。
//   - 轻触：立即切换下一张
//   - 长按（≥600ms）：退回首页
// ---------------------------------------------------------------------------
class BagclipScreen {
public:
    static lv_obj_t* Create();
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
