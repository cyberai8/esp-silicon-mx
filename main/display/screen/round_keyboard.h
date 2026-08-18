#pragma once

#include "lvgl.h"

// 360 圆屏键盘：每一行按所在位置的圆弦宽排键，贴着左右/底部圆弧放大按键。
// height 保留兼容。textarea 可为 nullptr。
lv_obj_t* RoundKeyboard_Create(lv_obj_t* parent, lv_obj_t* textarea, int height);

void RoundKeyboard_SetTextarea(lv_obj_t* kb, lv_obj_t* textarea);
lv_obj_t* RoundKeyboard_GetTextarea(lv_obj_t* kb);
