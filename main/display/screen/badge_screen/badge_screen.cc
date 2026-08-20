// BadgeScreen — 像章应用
//
// 从 SD 卡 /sdcard/badge/ 目录扫描 JPEG/PNG，全屏 cover 缩放铺满 360×360 圆屏。
//   - 轻触：切换下一张
//   - 长按：退回首页
//   - 右滑：退回首页（与其他应用一致）
//
// 图片解码在后台 FreeRTOS task 中完成，decode 完成后通过 lv_async_call 上屏。
// 内存全部走 heap_caps_malloc(MALLOC_CAP_SPIRAM) 以减少内部 SRAM 压力。

#include "badge_screen.h"

#include <atomic>
#include <cctype>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "config.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_common.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "home_screen/home_screen.h"
#include "i18n.h"
#include "screen_util.h"
#include "SdCardManager.hpp"

#if defined(ESP_PLATFORM)
#include "freertos/idf_additions.h"
#include <jpeglib.h>
#include <png.h>
#include <setjmp.h>
#endif

LV_FONT_DECLARE(font_puhui_20_4);

namespace {

constexpr const char* TAG = "BadgeScreen";
constexpr const char* kBadgeDir = "/sdcard/badge";
constexpr int32_t kScreenSize = DISPLAY_WIDTH;  // 360
constexpr size_t kMaxImages = 200;
constexpr size_t kMaxFileBytes = 8u * 1024u * 1024u;

// ---------------------------------------------------------------------------
// 辅助：内存
// ---------------------------------------------------------------------------

void* AllocBig(size_t bytes) {
    void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == nullptr) {
        p = heap_caps_malloc(bytes, MALLOC_CAP_DEFAULT);
    }
    return p;
}

// ---------------------------------------------------------------------------
// 文件扫描
// ---------------------------------------------------------------------------

bool ExtEquals(const char* name, const char* ext) {
    const char* dot = strrchr(name, '.');
    if (dot == nullptr) return false;
    ++dot;
    while (*dot && *ext) {
        if (tolower(static_cast<unsigned char>(*dot)) != *ext) return false;
        ++dot; ++ext;
    }
    return *dot == '\0' && *ext == '\0';
}

bool IsJpeg(const char* name) {
    return ExtEquals(name, "jpg") || ExtEquals(name, "jpeg");
}

bool IsPng(const char* name) {
    return ExtEquals(name, "png");
}

bool IsImage(const char* name) {
    return IsJpeg(name) || IsPng(name);
}

struct ImageEntry {
    std::string path;
    bool jpeg = false;
};

void ScanDir(const std::string& dir, int depth, std::vector<ImageEntry>* out) {
    if (depth > 3 || out->size() >= kMaxImages) return;
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr && out->size() < kMaxImages) {
        if (ent->d_name[0] == '.') continue;
        std::string path = dir + "/" + ent->d_name;
        if (ent->d_type == DT_DIR) {
            ScanDir(path, depth + 1, out);
        } else if (IsImage(ent->d_name)) {
            out->push_back({path, IsJpeg(ent->d_name)});
        }
    }
    closedir(d);
}

// ---------------------------------------------------------------------------
// 图片解码（全屏 cover 缩放到 kScreenSize × kScreenSize）
// ---------------------------------------------------------------------------

bool LoadFile(const std::string& path, uint8_t** out, size_t* out_len) {
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    const long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (len <= 0 || static_cast<size_t>(len) > kMaxFileBytes) {
        fclose(fp);
        return false;
    }
    auto* buf = static_cast<uint8_t*>(AllocBig(static_cast<size_t>(len)));
    if (!buf) { fclose(fp); return false; }
    if (fread(buf, 1, static_cast<size_t>(len), fp) != static_cast<size_t>(len)) {
        heap_caps_free(buf);
        fclose(fp);
        return false;
    }
    fclose(fp);
    *out = buf;
    *out_len = static_cast<size_t>(len);
    return true;
}

// RGB565 盒式均值缩放，把 src (stride_bytes/行) 的 (sx,sy,cw,ch) 裁剪区压到 dw×dh。
void BoxScale(const uint8_t* src, int stride_px, int sx, int sy, int cw, int ch,
              uint8_t* dst, int dw, int dh) {
    for (int y = 0; y < dh; ++y) {
        int y0 = sy + y * ch / dh;
        int y1 = sy + (y + 1) * ch / dh;
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; ++x) {
            int x0 = sx + x * cw / dw;
            int x1 = sx + (x + 1) * cw / dw;
            if (x1 <= x0) x1 = x0 + 1;
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1; ++yy) {
                const uint8_t* row = src + static_cast<size_t>(yy) * stride_px * 2;
                for (int xx = x0; xx < x1; ++xx) {
                    const uint16_t v = static_cast<uint16_t>(row[xx * 2] | (row[xx * 2 + 1] << 8));
                    r += (v >> 11) & 0x1F;
                    g += (v >> 5)  & 0x3F;
                    b +=  v        & 0x1F;
                    ++n;
                }
            }
            const uint16_t o = static_cast<uint16_t>(((r/n) << 11) | ((g/n) << 5) | (b/n));
            uint8_t* d = dst + (static_cast<size_t>(y) * dw + x) * 2;
            d[0] = static_cast<uint8_t>(o & 0xFF);
            d[1] = static_cast<uint8_t>(o >> 8);
        }
    }
}

// Cover 裁剪：把 raw_w×raw_h 按"短边贴合 target"缩放后居中裁掉多余部分，
// 输出 target×target 的 RGB565 缓冲，调用者负责 free。
uint8_t* CoverScale(const uint8_t* src, int raw_w, int raw_h, int stride_px, int target) {
    if (raw_w <= 0 || raw_h <= 0) return nullptr;

    // 按短边对齐 target 的缩放比
    const float fx = static_cast<float>(target) / static_cast<float>(raw_w);
    const float fy = static_cast<float>(target) / static_cast<float>(raw_h);
    const float f  = (fx > fy) ? fx : fy;  // 取大的那个（cover）

    int scaled_w = static_cast<int>(static_cast<float>(raw_w) * f + 0.5f);
    int scaled_h = static_cast<int>(static_cast<float>(raw_h) * f + 0.5f);
    if (scaled_w < target) scaled_w = target;
    if (scaled_h < target) scaled_h = target;

    // 居中裁剪起点
    const int cx = (scaled_w - target) / 2;
    const int cy = (scaled_h - target) / 2;

    // 直接从 src 的等比区域盒式降采样到 target×target
    const int src_crop_w = static_cast<int>(static_cast<float>(target) / f + 0.5f);
    const int src_crop_h = static_cast<int>(static_cast<float>(target) / f + 0.5f);
    const int src_cx = (raw_w - src_crop_w) / 2;
    const int src_cy = (raw_h - src_crop_h) / 2;

    const int real_cx = (src_cx < 0) ? 0 : src_cx;
    const int real_cy = (src_cy < 0) ? 0 : src_cy;
    const int real_cw = (real_cx + src_crop_w > raw_w) ? (raw_w - real_cx) : src_crop_w;
    const int real_ch = (real_cy + src_crop_h > raw_h) ? (raw_h - real_cy) : src_crop_h;

    (void)cx; (void)cy; (void)scaled_w; (void)scaled_h;

    auto* out = static_cast<uint8_t*>(AllocBig(static_cast<size_t>(target) * target * 2));
    if (!out) return nullptr;

    BoxScale(src, stride_px, real_cx, real_cy, real_cw, real_ch, out, target, target);
    return out;
}

#if defined(ESP_PLATFORM)

// JPEG 解码（libjpeg-turbo）
bool DecodeJpeg(const uint8_t* data, size_t len, uint8_t** out, int* out_w, int* out_h,
                int* out_stride) {
    jpeg_decompress_struct cinfo{};
    struct {
        struct jpeg_error_mgr pub;
        jmp_buf jb;
    } jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = [](j_common_ptr p) {
        auto* e = reinterpret_cast<decltype(jerr)*>(p->err);
        longjmp(e->jb, 1);
    };
    if (setjmp(jerr.jb)) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, data, static_cast<unsigned long>(len));
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB565;
    cinfo.dct_method = JDCT_IFAST;
    jpeg_start_decompress(&cinfo);
    const int w = static_cast<int>(cinfo.output_width);
    const int h = static_cast<int>(cinfo.output_height);
    const int stride = w;  // RGB565: 2 bytes/px
    auto* buf = static_cast<uint8_t*>(AllocBig(static_cast<size_t>(w) * h * 2));
    if (!buf) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    JSAMPROW row_ptr[1];
    int row = 0;
    while (cinfo.output_scanline < cinfo.output_height) {
        row_ptr[0] = buf + static_cast<size_t>(row) * w * 2;
        jpeg_read_scanlines(&cinfo, row_ptr, 1);
        ++row;
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    *out = buf; *out_w = w; *out_h = h; *out_stride = stride;
    return true;
}

// PNG 解码（libpng）
struct PngReadCtx { const uint8_t* data; size_t pos; size_t len; };

static void PngReadFn(png_structp ps, png_bytep buf, png_size_t cnt) {
    auto* ctx = static_cast<PngReadCtx*>(png_get_io_ptr(ps));
    if (ctx->pos + cnt > ctx->len) { png_error(ps, "eof"); return; }
    memcpy(buf, ctx->data + ctx->pos, cnt);
    ctx->pos += cnt;
}

bool DecodePng(const uint8_t* data, size_t len, uint8_t** out, int* out_w, int* out_h,
               int* out_stride) {
    png_structp ps = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!ps) return false;
    png_infop pi = png_create_info_struct(ps);
    if (!pi) { png_destroy_read_struct(&ps, nullptr, nullptr); return false; }
    if (setjmp(png_jmpbuf(ps))) {
        png_destroy_read_struct(&ps, &pi, nullptr);
        return false;
    }
    PngReadCtx ctx{data, 0, len};
    png_set_read_fn(ps, &ctx, PngReadFn);
    png_read_info(ps, pi);
    int w = static_cast<int>(png_get_image_width(ps, pi));
    int h = static_cast<int>(png_get_image_height(ps, pi));
    // 转 RGB565
    png_set_expand(ps);
    png_set_strip_16(ps);
    png_set_gray_to_rgb(ps);
    png_set_filler(ps, 0xFF, PNG_FILLER_AFTER);
    png_set_bgr(ps);
    png_read_update_info(ps, pi);

    // 先解出 RGBA8888，再转 RGB565
    auto* rgba = static_cast<uint8_t*>(AllocBig(static_cast<size_t>(w) * h * 4));
    if (!rgba) { png_destroy_read_struct(&ps, &pi, nullptr); return false; }
    std::vector<png_bytep> rows(static_cast<size_t>(h));
    for (int y = 0; y < h; ++y) {
        rows[static_cast<size_t>(y)] = rgba + static_cast<size_t>(y) * w * 4;
    }
    png_read_image(ps, rows.data());
    png_destroy_read_struct(&ps, &pi, nullptr);

    auto* rgb565 = static_cast<uint8_t*>(AllocBig(static_cast<size_t>(w) * h * 2));
    if (!rgb565) { heap_caps_free(rgba); return false; }
    for (int i = 0; i < w * h; ++i) {
        const uint8_t b = rgba[i * 4 + 0];
        const uint8_t g = rgba[i * 4 + 1];
        const uint8_t r = rgba[i * 4 + 2];
        const uint16_t v = static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        rgb565[i * 2]     = static_cast<uint8_t>(v & 0xFF);
        rgb565[i * 2 + 1] = static_cast<uint8_t>(v >> 8);
    }
    heap_caps_free(rgba);
    *out = rgb565; *out_w = w; *out_h = h; *out_stride = w;
    return true;
}

#endif  // ESP_PLATFORM

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------

struct BadgeUi {
    lv_obj_t* scr       = nullptr;
    lv_obj_t* img       = nullptr;
    lv_obj_t* hint      = nullptr;  // "badge/ 目录为空" 提示
};

BadgeUi s_ui;
bool s_screen_active = false;

std::vector<ImageEntry> s_images;
int s_current_index = 0;

std::atomic<bool> s_decode_busy{false};
std::atomic<bool> s_screen_alive{false};

// 当前显示的缓冲（UI 线程只读，worker 写完后 swap）
uint8_t* s_show_buf = nullptr;
lv_image_dsc_t s_show_dsc{};

// 解码完成后，worker 把结果放到 pending，然后 lv_async_call 上屏
struct PendingFrame {
    uint8_t* buf  = nullptr;
    int       w   = 0;
    int       h   = 0;
};

std::atomic<PendingFrame*> s_pending{nullptr};

// ---------------------------------------------------------------------------
// 辅助
// ---------------------------------------------------------------------------

inline bool ScreenAlive() { return s_screen_alive.load(std::memory_order_relaxed); }

void FillDsc(lv_image_dsc_t* dsc, const uint8_t* data, int w, int h) {
    memset(dsc, 0, sizeof(*dsc));
    dsc->header.magic  = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf     = LV_COLOR_FORMAT_RGB565;
    dsc->header.w      = static_cast<uint32_t>(w);
    dsc->header.h      = static_cast<uint32_t>(h);
    dsc->header.stride = static_cast<uint32_t>(w * 2);
    dsc->data_size     = static_cast<uint32_t>(w) * h * 2;
    dsc->data          = data;
}

// ---------------------------------------------------------------------------
// 解码 worker
// ---------------------------------------------------------------------------

void ApplyPendingFrame(void* /*arg*/) {
    if (!ScreenAlive()) return;
    PendingFrame* pf = s_pending.exchange(nullptr, std::memory_order_acq_rel);
    if (!pf) return;

    // 释放旧 buf
    if (s_show_buf) {
        heap_caps_free(s_show_buf);
    }
    s_show_buf = pf->buf;
    FillDsc(&s_show_dsc, s_show_buf, pf->w, pf->h);
    delete pf;

    if (s_ui.img) {
        lv_image_set_src(s_ui.img, nullptr);
        lv_obj_set_size(s_ui.img, kScreenSize, kScreenSize);
        lv_obj_center(s_ui.img);
        lv_image_set_src(s_ui.img, &s_show_dsc);
    }
    if (s_ui.hint) {
        lv_obj_add_flag(s_ui.hint, LV_OBJ_FLAG_HIDDEN);
    }
}

void DecodeTask(void* arg) {
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    if (index < 0 || index >= static_cast<int>(s_images.size())) {
        s_decode_busy.store(false, std::memory_order_release);
        vTaskDelete(nullptr);
        return;
    }
    const ImageEntry& entry = s_images[static_cast<size_t>(index)];

    uint8_t* file_data = nullptr;
    size_t file_len = 0;
    if (!LoadFile(entry.path, &file_data, &file_len)) {
        ESP_LOGW(TAG, "Cannot load: %s", entry.path.c_str());
        s_decode_busy.store(false, std::memory_order_release);
        vTaskDelete(nullptr);
        return;
    }

    uint8_t* raw = nullptr;
    int raw_w = 0, raw_h = 0, raw_stride = 0;
    bool ok = false;

#if defined(ESP_PLATFORM)
    if (entry.jpeg) {
        ok = DecodeJpeg(file_data, file_len, &raw, &raw_w, &raw_h, &raw_stride);
    } else {
        ok = DecodePng(file_data, file_len, &raw, &raw_w, &raw_h, &raw_stride);
    }
#endif
    heap_caps_free(file_data);

    if (!ok || !raw) {
        ESP_LOGW(TAG, "Decode failed: %s", entry.path.c_str());
        if (raw) heap_caps_free(raw);
        s_decode_busy.store(false, std::memory_order_release);
        vTaskDelete(nullptr);
        return;
    }

    uint8_t* cover = CoverScale(raw, raw_w, raw_h, raw_stride, kScreenSize);
    heap_caps_free(raw);

    if (!cover) {
        ESP_LOGW(TAG, "CoverScale OOM: %s", entry.path.c_str());
        s_decode_busy.store(false, std::memory_order_release);
        vTaskDelete(nullptr);
        return;
    }

    auto* pf = new PendingFrame{cover, kScreenSize, kScreenSize};
    // 丢掉之前未消费的帧（如果有）
    PendingFrame* old = s_pending.exchange(pf, std::memory_order_acq_rel);
    if (old) {
        heap_caps_free(old->buf);
        delete old;
    }

    if (ScreenAlive()) {
        lv_async_call(ApplyPendingFrame, nullptr);
    } else {
        heap_caps_free(cover);
        PendingFrame* stale = s_pending.exchange(nullptr, std::memory_order_acq_rel);
        if (stale) { heap_caps_free(stale->buf); delete stale; }
    }

    s_decode_busy.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

void StartDecode(int index) {
    if (s_decode_busy.exchange(true, std::memory_order_acq_rel)) {
        return;  // 已有解码任务在跑
    }
    xTaskCreate(DecodeTask, "badge_dec", 1024 * 8, reinterpret_cast<void*>(index),
                5, nullptr);
}

// ---------------------------------------------------------------------------
// 导航 / 手势
// ---------------------------------------------------------------------------

void GoHome() {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* home = HomeScreen::Create();
    lv_screen_load(home);
    if (old_scr != nullptr && old_scr != home) {
        lv_obj_delete_async(old_scr);
    }
}

void OnSwipeBack() { GoHome(); }

void OnLongPressed(lv_event_t* /*e*/) {
    if (!ScreenAlive()) return;
    GoHome();
}

void OnClicked(lv_event_t* /*e*/) {
    if (!ScreenAlive() || s_images.empty()) return;
    s_current_index = (s_current_index + 1) % static_cast<int>(s_images.size());
    StartDecode(s_current_index);
}

// ---------------------------------------------------------------------------
// 创建屏幕
// ---------------------------------------------------------------------------

}  // namespace

lv_obj_t* BadgeScreen::Create() {
    s_screen_alive.store(true, std::memory_order_release);
    s_screen_active = true;
    s_decode_busy.store(false, std::memory_order_release);

    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_size(scr, kScreenSize, kScreenSize);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    s_ui.scr = scr;

    // 图像对象
    lv_obj_t* img = lv_image_create(scr);
    lv_obj_set_size(img, kScreenSize, kScreenSize);
    lv_obj_center(img);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_STRETCH);
    screen_make_input_passive(img);
    s_ui.img = img;

    // 空目录提示
    lv_obj_t* hint = lv_label_create(scr);
    lv_label_set_text(hint, "在 SD 卡\n/badge/ 目录\n放入图片");
    lv_obj_set_style_text_color(hint, lv_color_hex(0xAAAAAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_center(hint);
    screen_make_input_passive(hint);
    s_ui.hint = hint;

    // 手势挂在 scr 上（子控件已 passive），右滑/长按退出，轻触切图。
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    screen_attach_swipe_back(scr, OnSwipeBack);
    lv_obj_add_event_cb(scr, OnLongPressed, LV_EVENT_LONG_PRESSED, nullptr);
    lv_obj_add_event_cb(scr, OnClicked, LV_EVENT_CLICKED, nullptr);

    // 扫描图片目录
    s_images.clear();
    s_current_index = 0;
    ScanDir(kBadgeDir, 0, &s_images);
    ESP_LOGI(TAG, "Found %d images in %s", static_cast<int>(s_images.size()), kBadgeDir);

    if (!s_images.empty()) {
        lv_obj_add_flag(hint, LV_OBJ_FLAG_HIDDEN);
        StartDecode(0);
    }

    return scr;
}

void BadgeScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_UNLOAD) {
        s_screen_alive.store(false, std::memory_order_release);
        s_screen_active = false;

        // 清理 pending 帧（worker 还没上屏的）
        PendingFrame* pf = s_pending.exchange(nullptr, std::memory_order_acq_rel);
        if (pf) { heap_caps_free(pf->buf); delete pf; }

        // 释放显示缓冲
        if (s_show_buf) {
            lv_image_set_src(s_ui.img, nullptr);
            heap_caps_free(s_show_buf);
            s_show_buf = nullptr;
        }

        s_ui = {};
        s_images.clear();
    }
}
