// 验证 home_screen 里"把切片图整幅画进常驻缓冲"那套 LVGL 用法出来的像素对不对。
// 左边是直接用文件路径当图源，右边是常驻缓冲当图源；底图是常驻 RGB565 版本。
// 两边看起来一样就说明这套用法没问题。
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <esp_heap_caps.h>

#include "lvgl.h"
#include "sim_ramimg.h"

namespace {

bool DecodeAssetToRam(const char* path, bool opaque_on_black,
                      lv_image_dsc_t* out) {
    if (path == nullptr || out == nullptr) {
        return false;
    }
    lv_image_header_t header;
    if (lv_image_decoder_get_info(path, &header) != LV_RESULT_OK) {
        return false;
    }
    const int32_t w = static_cast<int32_t>(header.w);
    const int32_t h = static_cast<int32_t>(header.h);
    const lv_color_format_t cf =
        opaque_on_black ? LV_COLOR_FORMAT_RGB565 : LV_COLOR_FORMAT_ARGB8888;
    const size_t px = opaque_on_black ? 2u : 4u;
    const size_t bytes = static_cast<size_t>(w) * h * px;
    if (w <= 0 || h <= 0) {
        return false;
    }
    auto* buf = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
    if (buf == nullptr) {
        return false;
    }
    std::memset(buf, 0, bytes);

    lv_obj_t* holder = lv_obj_create(nullptr);
    lv_obj_t* canvas = (holder != nullptr) ? lv_canvas_create(holder) : nullptr;
    if (canvas == nullptr) {
        if (holder != nullptr) {
            lv_obj_delete(holder);
        }
        heap_caps_free(buf);
        return false;
    }
    lv_canvas_set_buffer(canvas, buf, w, h, cf);

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);
    lv_draw_image_dsc_t img;
    lv_draw_image_dsc_init(&img);
    img.src = path;
    lv_area_t area = {0, 0, w - 1, h - 1};
    lv_draw_image(&layer, &img, &area);
    lv_canvas_finish_layer(canvas, &layer);
    lv_obj_delete(holder);

    std::memset(out, 0, sizeof(*out));
    out->header.magic = LV_IMAGE_HEADER_MAGIC;
    out->header.cf = cf;
    out->header.w = static_cast<uint32_t>(w);
    out->header.h = static_cast<uint32_t>(h);
    out->header.stride = static_cast<uint32_t>(w) * px;
    out->data_size = static_cast<uint32_t>(bytes);
    out->data = buf;
    return true;
}

lv_image_dsc_t s_chrome;
lv_image_dsc_t s_icon;

}  // namespace

lv_obj_t* SimRamImgCreate() {
    lv_obj_t* screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);

    // SIM_RAMIMG_CHROME=file 时底图退回文件图源，用来和常驻 RGB565 版本做 diff。
    const char* chrome_mode = getenv("SIM_RAMIMG_CHROME");
    const bool chrome_ok =
        (chrome_mode == nullptr || strcmp(chrome_mode, "file") != 0) &&
        DecodeAssetToRam("A:home_clover_chrome.spng", true, &s_chrome);
    lv_obj_t* chrome = lv_image_create(screen);
    if (chrome_ok) {
        lv_image_set_src(chrome, &s_chrome);
    } else {
        lv_image_set_src(chrome, "A:home_clover_chrome.spng");
    }
    lv_obj_align(chrome, LV_ALIGN_TOP_LEFT, 0, 0);

    const bool icon_ok = DecodeAssetToRam("A:ic_clover_alarm.spng", false, &s_icon);
    // SIM_RAMIMG_ONLY=file|ram：只画一个图标、都放同一位置，两次截图可以直接做像素 diff。
    const char* only = getenv("SIM_RAMIMG_ONLY");
    const bool want_file = (only == nullptr) || strcmp(only, "file") == 0;
    const bool want_ram = (only == nullptr) || strcmp(only, "ram") == 0;

    if (want_file) {
        lv_obj_t* from_file = lv_image_create(screen);
        lv_image_set_src(from_file, "A:ic_clover_alarm.spng");
        lv_obj_set_pos(from_file, (only == nullptr ? 62 : 180) - 40, 178 - 40);
    }
    if (want_ram) {
        lv_obj_t* from_ram = lv_image_create(screen);
        lv_image_set_src(from_ram, icon_ok ? static_cast<const void*>(&s_icon)
                                           : static_cast<const void*>(
                                                 "A:ic_clover_alarm.spng"));
        lv_obj_set_pos(from_ram, (only == nullptr ? 298 : 180) - 40, 178 - 40);
    }

    if (only == nullptr) {
        lv_obj_t* tag = lv_label_create(screen);
        lv_label_set_text_fmt(tag, "chrome=%d icon=%d", chrome_ok ? 1 : 0,
                              icon_ok ? 1 : 0);
        lv_obj_set_style_text_color(tag, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_align(tag, LV_ALIGN_CENTER, 0, 0);
    }

    return screen;
}
