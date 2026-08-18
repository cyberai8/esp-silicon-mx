#pragma once

// 全局电量告警：任意界面弹出低电量提示，极低电量自动关机。
// 须在 LVGL 初始化完成后调用 BatteryAlert_Start() 一次。

void BatteryAlert_Start();
