#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_lv_adapter_display.h"

struct ImuSample {
    int16_t ax = 0;
    int16_t ay = 0;
    int16_t az = 0;
    int16_t gx = 0;
    int16_t gy = 0;
    int16_t gz = 0;
    bool ok = false;
};

// 基于板级默认方向（config.h 中 DISPLAY_*），叠加 0/90/180/270° 旋转。
void DisplayOrientationInit(esp_lcd_panel_handle_t panel, esp_lcd_touch_handle_t touch,
                            bool base_swap_xy, bool base_mirror_x, bool base_mirror_y);

esp_err_t DisplayOrientationApply(esp_lv_adapter_rotation_t rotation);

esp_lv_adapter_rotation_t DisplayOrientationGet();

// NVS display/auto_rotate；关闭后 IMU 不再自动转屏。
bool DisplayOrientationIsAutoEnabled();
void DisplayOrientationSetAutoEnabled(bool enabled);

// 竖握：加速度计重力方向；平放：陀螺仪 Z 积分（绕屏法向转 90°）。
esp_lv_adapter_rotation_t DisplayOrientationUpdate(const ImuSample& sample, int dt_ms);

// 调试：每 2s 打印一次 IMU 与判定结果（config 里可关）。
void DisplayOrientationMaybeLog(const ImuSample& sample, esp_lv_adapter_rotation_t target);
