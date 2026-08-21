#pragma once

#include "lvgl.h"

// 待机第三脸：背包扣同款全屏 cover + 自动翻页。
// 优先扫 /sdcard/bagclip，为空再扫 /sdcard/badge。
lv_obj_t* StandbyGallery_Create(lv_obj_t* parent);
void StandbyGallery_SetActive(bool active);
void StandbyGallery_Destroy();
