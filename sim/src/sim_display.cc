#include "sim_display.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// LVGL 自带的 lodepng，既能解 PNG 也能编码，省掉一个系统依赖。
// 这里不直接 include lodepng.h：那个头把 <string> 也裹进了 extern "C"，
// 从 C++ 里 include 会炸，只声明用到的几个函数最省事。
// 另外只用内存版编码 —— LVGL 把 lodepng 的文件读写改接到 lv_fs 上了，
// 存盘这段自己 fwrite 更直接。
extern "C" {
unsigned lodepng_encode32(unsigned char** out, size_t* outsize, const unsigned char* image,
                          unsigned w, unsigned h);
const char* lodepng_error_text(unsigned code);
void lv_free(void* ptr);
}

namespace SimDisplay {
namespace {

int s_w = 0;
int s_h = 0;
std::vector<uint8_t> s_fb;  // RGB565，LVGL 直接画在这里

void FlushCb(lv_display_t* disp, const lv_area_t* /*area*/, uint8_t* /*px_map*/) {
    // DIRECT 模式下 LVGL 已经画进 s_fb 了，这里没有真实的屏要送。
    lv_display_flush_ready(disp);
}

}  // namespace

lv_display_t* Create(int w, int h) {
    s_w = w;
    s_h = h;
    s_fb.assign(static_cast<size_t>(w) * h * 2, 0);

    lv_display_t* disp = lv_display_create(w, h);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, FlushCb);
    lv_display_set_buffers(disp, s_fb.data(), nullptr, s_fb.size(),
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    return disp;
}

int Width() {
    return s_w;
}

int Height() {
    return s_h;
}

void CaptureRgba(std::vector<uint8_t>* out, bool round) {
    out->assign(static_cast<size_t>(s_w) * s_h * 4, 0);
    if (s_fb.empty()) {
        return;
    }
    std::vector<uint8_t>& rgba = *out;
    const float cx = static_cast<float>(s_w) / 2.0f - 0.5f;
    const float cy = static_cast<float>(s_h) / 2.0f - 0.5f;
    const float radius = static_cast<float>(s_w < s_h ? s_w : s_h) / 2.0f;

    for (int y = 0; y < s_h; ++y) {
        for (int x = 0; x < s_w; ++x) {
            const size_t si = (static_cast<size_t>(y) * s_w + x) * 2;
            const uint16_t v =
                static_cast<uint16_t>(s_fb[si] | (static_cast<uint16_t>(s_fb[si + 1]) << 8));
            // RGB565 -> RGB888，高位补到低位，避免整体偏暗
            uint8_t r = static_cast<uint8_t>(((v >> 11) & 0x1F) * 255 / 31);
            uint8_t g = static_cast<uint8_t>(((v >> 5) & 0x3F) * 255 / 63);
            uint8_t b = static_cast<uint8_t>((v & 0x1F) * 255 / 31);

            if (round) {
                const float dx = static_cast<float>(x) - cx;
                const float dy = static_cast<float>(y) - cy;
                const float d = std::sqrt(dx * dx + dy * dy);
                if (d > radius) {
                    r = g = b = 16;  // 圆外是机身，真机上看不到
                } else if (d > radius - 1.5f) {
                    r = g = b = 70;  // 一圈细边，方便看清屏幕边界
                }
            }
            const size_t di = (static_cast<size_t>(y) * s_w + x) * 4;
            rgba[di + 0] = r;
            rgba[di + 1] = g;
            rgba[di + 2] = b;
            rgba[di + 3] = 255;
        }
    }
}

bool WritePng(const char* path, const uint8_t* rgba, int w, int h) {
    unsigned char* png = nullptr;
    size_t png_len = 0;
    const unsigned err = lodepng_encode32(&png, &png_len, rgba, static_cast<unsigned>(w),
                                          static_cast<unsigned>(h));
    if (err != 0) {
        fprintf(stderr, "[sim] PNG 编码失败: %s\n", lodepng_error_text(err));
        lv_free(png);
        return false;
    }
    FILE* fp = fopen(path, "wb");
    if (fp == nullptr) {
        fprintf(stderr, "[sim] 写不了 %s\n", path);
        lv_free(png);
        return false;
    }
    const size_t wrote = fwrite(png, 1, png_len, fp);
    fclose(fp);
    lv_free(png);
    return wrote == png_len;
}

bool Screenshot(const char* path, bool round) {
    if (s_fb.empty()) {
        return false;
    }
    std::vector<uint8_t> rgba;
    CaptureRgba(&rgba, round);
    return WritePng(path, rgba.data(), s_w, s_h);
}

}  // namespace SimDisplay
