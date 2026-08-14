#pragma once

#include "lvgl.h"
#include "screen_util.h"

// 可以在仿真里打开的屏幕清单。加新屏就在 sim_screens.cc 里加一行。
struct SimScreenEntry {
    const char* name;
    lv_obj_t* (*create)();
    screen_lifecycle_cb_t lifecycle;  // 可为空
};

const SimScreenEntry* SimScreens();
int SimScreenCount();
const SimScreenEntry* SimFindScreen(const char* name);
