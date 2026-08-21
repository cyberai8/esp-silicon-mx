#pragma once

// 板级 config.h 的仿真版：只保留 UI 代码会读的显示参数。
// 想仿真别的屏，改这里的宽高（同时改 sim/src/main.cc 的默认值即可）。
#define DISPLAY_WIDTH 360
#define DISPLAY_HEIGHT 360
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY false
#define DISPLAY_OFFSET_X 0
#define DISPLAY_OFFSET_Y 0

// 圆屏 + 这块板的功能开关，和 sdkconfig.esp-vocat 对齐。
#define BOARD_ESP_VOCAT 1
#define BOARD_ESP_SHOW 1
#define BOARD_HAS_EXTERNAL_BT 0
#define BOARD_HAS_NATIVE_BT 0
#define BOARD_HAS_DUAL_SIM 0
