#pragma once

#include "lvgl.h"

// 合成触摸：脚本里写坐标，这里当成一个真实的指针设备喂给 LVGL，
// 于是点击、滑动手势、长按都走和真机一样的输入路径。
namespace SimInput {

void Init();
void MoveTo(int32_t x, int32_t y);
void SetPressed(bool pressed);

}  // namespace SimInput
