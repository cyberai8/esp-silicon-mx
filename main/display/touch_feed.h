#pragma once

#include "esp_lcd_touch.h"
#include "lvgl.h"

// 芯片坐标 trace 日志：1 开启，0 关闭（默认）。
#ifndef TOUCH_FEED_DEBUG
#define TOUCH_FEED_DEBUG 0
#endif

// 单一读源：独立任务读触摸芯片，LVGL indev read_cb 只读 mutex 保护的快照。
// CST816S：INT 为短脉冲，ISR 边沿锁存 + 任务通知，避免电平轮询漏点。
void touch_feed_init(esp_lcd_touch_handle_t handle, uint32_t period_ms);
void touch_feed_attach_indev(lv_indev_t* indev);
void touch_feed_stop();
