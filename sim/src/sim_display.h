#pragma once

#include <cstdint>
#include <vector>

#include "lvgl.h"

// 无窗口（headless）显示驱动：LVGL 直接画进一块内存，随时可以存成 PNG。
// 不依赖 SDL / X11，所以在 SSH、CI 里都能跑。
namespace SimDisplay {

// 建一个 w×h 的 RGB565 显示器，和设备上的颜色深度一致。
lv_display_t* Create(int w, int h);

// 把当前画面存成 PNG。round=true 时按内切圆遮罩，圆外画成机身黑，
// 这样看到的就是圆屏上真实可见的范围。
bool Screenshot(const char* path, bool round);

// 取当前画面的 RGBA（给「多帧拼一张」用），大小是 Width()*Height()*4。
void CaptureRgba(std::vector<uint8_t>* out, bool round);

bool WritePng(const char* path, const uint8_t* rgba, int w, int h);

int Width();
int Height();

}  // namespace SimDisplay
