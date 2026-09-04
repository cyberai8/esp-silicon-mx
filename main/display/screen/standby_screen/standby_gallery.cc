// 待机相册脸：逻辑对齐背包扣（cover 360 + 3s 自动翻页）。
#include "standby_gallery.h"

#include <atomic>
#include <cctype>
#include <cstring>
#include <dirent.h>
#include <string>
#include <vector>

#include "config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "i18n.h"
#include "screen_util.h"

#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include <jpeglib.h>
#include <png.h>
#include <setjmp.h>
#endif

LV_FONT_DECLARE(font_puhui_20_4);

namespace {

constexpr const char* TAG = "StandbyGallery";
constexpr const char* kBagclipDir = "/sdcard/bagclip";
constexpr const char* kBadgeDir = "/sdcard/badge";
constexpr int32_t kScreenSize = DISPLAY_WIDTH;
constexpr uint32_t kSlideIntervalMs = 3000;
constexpr size_t kMaxImages = 200;
constexpr size_t kMaxFileBytes = 8u * 1024u * 1024u;

void* AllocBig(size_t bytes) {
    void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = heap_caps_malloc(bytes, MALLOC_CAP_DEFAULT);
    }
    return p;
}

bool ExtEquals(const char* name, const char* ext) {
    const char* dot = strrchr(name, '.');
    if (!dot) {
        return false;
    }
    ++dot;
    while (*dot && *ext) {
        if (tolower(static_cast<unsigned char>(*dot)) != *ext) {
            return false;
        }
        ++dot;
        ++ext;
    }
    return *dot == '\0' && *ext == '\0';
}

bool IsJpeg(const char* n) {
    return ExtEquals(n, "jpg") || ExtEquals(n, "jpeg");
}
bool IsPng(const char* n) {
    return ExtEquals(n, "png");
}
bool IsImage(const char* n) {
    return IsJpeg(n) || IsPng(n);
}

struct ImageEntry {
    std::string path;
    bool jpeg = false;
};

void ScanDir(const std::string& dir, int depth, std::vector<ImageEntry>* out) {
    if (depth > 3 || out->size() >= kMaxImages) {
        return;
    }
    DIR* d = opendir(dir.c_str());
    if (!d) {
        return;
    }
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr && out->size() < kMaxImages) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        std::string path = dir + "/" + ent->d_name;
        if (ent->d_type == DT_DIR) {
            ScanDir(path, depth + 1, out);
        } else if (IsImage(ent->d_name)) {
            out->push_back({path, IsJpeg(ent->d_name)});
        }
    }
    closedir(d);
}

#if defined(ESP_PLATFORM)

bool LoadFile(const std::string& path, uint8_t** out, size_t* out_len) {
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) {
        return false;
    }
    fseek(fp, 0, SEEK_END);
    const long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (len <= 0 || static_cast<size_t>(len) > kMaxFileBytes) {
        fclose(fp);
        return false;
    }
    auto* buf = static_cast<uint8_t*>(AllocBig(static_cast<size_t>(len)));
    if (!buf) {
        fclose(fp);
        return false;
    }
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

void BoxScale(const uint8_t* src, int stride_px, int sx, int sy, int cw, int ch,
              uint8_t* dst, int dw, int dh) {
    for (int y = 0; y < dh; ++y) {
        int y0 = sy + y * ch / dh;
        int y1 = sy + (y + 1) * ch / dh;
        if (y1 <= y0) {
            y1 = y0 + 1;
        }
        for (int x = 0; x < dw; ++x) {
            int x0 = sx + x * cw / dw;
            int x1 = sx + (x + 1) * cw / dw;
            if (x1 <= x0) {
                x1 = x0 + 1;
            }
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1; ++yy) {
                const uint8_t* row = src + static_cast<size_t>(yy) * stride_px * 2;
                for (int xx = x0; xx < x1; ++xx) {
                    const uint16_t v =
                        static_cast<uint16_t>(row[xx * 2] | (row[xx * 2 + 1] << 8));
                    r += (v >> 11) & 0x1F;
                    g += (v >> 5) & 0x3F;
                    b += v & 0x1F;
                    ++n;
                }
            }
            const uint16_t o =
                static_cast<uint16_t>(((r / n) << 11) | ((g / n) << 5) | (b / n));
            uint8_t* d = dst + (static_cast<size_t>(y) * dw + x) * 2;
            d[0] = static_cast<uint8_t>(o & 0xFF);
            d[1] = static_cast<uint8_t>(o >> 8);
        }
    }
}

uint8_t* CoverScale(const uint8_t* src, int raw_w, int raw_h, int stride_px, int target) {
    if (raw_w <= 0 || raw_h <= 0) {
        return nullptr;
    }
    const float fx = static_cast<float>(target) / static_cast<float>(raw_w);
    const float fy = static_cast<float>(target) / static_cast<float>(raw_h);
    const float f = (fx > fy) ? fx : fy;
    const int src_crop_w = static_cast<int>(static_cast<float>(target) / f + 0.5f);
    const int src_crop_h = static_cast<int>(static_cast<float>(target) / f + 0.5f);
    int real_cx = (raw_w - src_crop_w) / 2;
    if (real_cx < 0) {
        real_cx = 0;
    }
    int real_cy = (raw_h - src_crop_h) / 2;
    if (real_cy < 0) {
        real_cy = 0;
    }
    int real_cw = src_crop_w;
    if (real_cx + real_cw > raw_w) {
        real_cw = raw_w - real_cx;
    }
    int real_ch = src_crop_h;
    if (real_cy + real_ch > raw_h) {
        real_ch = raw_h - real_cy;
    }
    auto* out = static_cast<uint8_t*>(AllocBig(static_cast<size_t>(target) * target * 2));
    if (!out) {
        return nullptr;
    }
    BoxScale(src, stride_px, real_cx, real_cy, real_cw, real_ch, out, target, target);
    return out;
}

bool DecodeJpeg(const uint8_t* data, size_t len, uint8_t** out, int* ow, int* oh, int* os) {
    jpeg_decompress_struct cinfo{};
    struct {
        struct jpeg_error_mgr pub;
        jmp_buf jb;
    } jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = [](j_common_ptr p) {
        longjmp(reinterpret_cast<decltype(jerr)*>(p->err)->jb, 1);
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
    auto* buf = static_cast<uint8_t*>(AllocBig(static_cast<size_t>(w) * h * 2));
    if (!buf) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    JSAMPROW row[1];
    int r = 0;
    while (cinfo.output_scanline < cinfo.output_height) {
        row[0] = buf + static_cast<size_t>(r++) * w * 2;
        jpeg_read_scanlines(&cinfo, row, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    *out = buf;
    *ow = w;
    *oh = h;
    *os = w;
    return true;
}

struct PngCtx {
    const uint8_t* data;
    size_t pos, len;
};
static void PngRead(png_structp ps, png_bytep buf, png_size_t cnt) {
    auto* c = static_cast<PngCtx*>(png_get_io_ptr(ps));
    if (c->pos + cnt > c->len) {
        png_error(ps, "eof");
        return;
    }
    memcpy(buf, c->data + c->pos, cnt);
    c->pos += cnt;
}

bool DecodePng(const uint8_t* data, size_t len, uint8_t** out, int* ow, int* oh, int* os) {
    png_structp ps = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!ps) {
        return false;
    }
    png_infop pi = png_create_info_struct(ps);
    if (!pi) {
        png_destroy_read_struct(&ps, nullptr, nullptr);
        return false;
    }
    if (setjmp(png_jmpbuf(ps))) {
        png_destroy_read_struct(&ps, &pi, nullptr);
        return false;
    }
    PngCtx ctx{data, 0, len};
    png_set_read_fn(ps, &ctx, PngRead);
    png_read_info(ps, pi);
    int w = static_cast<int>(png_get_image_width(ps, pi));
    int h = static_cast<int>(png_get_image_height(ps, pi));
    png_set_expand(ps);
    png_set_strip_16(ps);
    png_set_gray_to_rgb(ps);
    png_set_filler(ps, 0xFF, PNG_FILLER_AFTER);
    png_set_bgr(ps);
    png_read_update_info(ps, pi);
    auto* rgba = static_cast<uint8_t*>(AllocBig(static_cast<size_t>(w) * h * 4));
    if (!rgba) {
        png_destroy_read_struct(&ps, &pi, nullptr);
        return false;
    }
    std::vector<png_bytep> rows(static_cast<size_t>(h));
    for (int y = 0; y < h; ++y) {
        rows[static_cast<size_t>(y)] = rgba + static_cast<size_t>(y) * w * 4;
    }
    png_read_image(ps, rows.data());
    png_destroy_read_struct(&ps, &pi, nullptr);
    auto* rgb565 = static_cast<uint8_t*>(AllocBig(static_cast<size_t>(w) * h * 2));
    if (!rgb565) {
        heap_caps_free(rgba);
        return false;
    }
    for (int i = 0; i < w * h; ++i) {
        const uint8_t b = rgba[i * 4], g = rgba[i * 4 + 1], r = rgba[i * 4 + 2];
        const uint16_t v =
            static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        rgb565[i * 2] = static_cast<uint8_t>(v & 0xFF);
        rgb565[i * 2 + 1] = static_cast<uint8_t>(v >> 8);
    }
    heap_caps_free(rgba);
    *out = rgb565;
    *ow = w;
    *oh = h;
    *os = w;
    return true;
}

#endif  // ESP_PLATFORM

struct GalleryUi {
    lv_obj_t* root = nullptr;
    lv_obj_t* img = nullptr;
    lv_obj_t* hint = nullptr;
    lv_timer_t* slide_timer = nullptr;
};

GalleryUi s_ui;
std::vector<ImageEntry> s_images;
int s_current_index = 0;
int s_fail_streak = 0;
std::atomic<bool> s_decode_busy{false};
std::atomic<bool> s_alive{false};
std::atomic<bool> s_active{false};
#if defined(ESP_PLATFORM)
std::atomic<bool> s_worker_stop{false};
std::atomic<int> s_job_index{-1};
TaskHandle_t s_worker = nullptr;
lv_timer_t* s_decode_defer_timer = nullptr;
constexpr uint32_t kGalleryDecodeDeferMs =
    (DISPLAY_WIDTH == 360 && DISPLAY_HEIGHT == 360) ? 2500u : 600u;
#endif

uint8_t* s_show_buf = nullptr;
lv_image_dsc_t s_show_dsc{};

struct PendingFrame {
    uint8_t* buf;
    int w;
    int h;
};
std::atomic<PendingFrame*> s_pending{nullptr};

void StopSlideTimer();
void StartSlideTimer();
void StartDecode(int index);
void ArmRetryTimer();

#if defined(ESP_PLATFORM)

constexpr uint32_t kRetryAfterFailMs = 10000;

void FillDsc(lv_image_dsc_t* dsc, const uint8_t* data, int w, int h) {
    memset(dsc, 0, sizeof(*dsc));
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.w = static_cast<uint32_t>(w);
    dsc->header.h = static_cast<uint32_t>(h);
    dsc->header.stride = static_cast<uint32_t>(w * 2);
    dsc->data_size = static_cast<uint32_t>(w) * h * 2;
    dsc->data = data;
}

void ApplyPendingFrame(void* /*arg*/) {
    s_decode_busy.store(false, std::memory_order_release);
    if (!s_alive.load(std::memory_order_relaxed) ||
        !s_active.load(std::memory_order_relaxed)) {
        PendingFrame* drop = s_pending.exchange(nullptr, std::memory_order_acq_rel);
        if (drop) {
            heap_caps_free(drop->buf);
            delete drop;
        }
        return;
    }
    PendingFrame* pf = s_pending.exchange(nullptr, std::memory_order_acq_rel);
    if (!pf) {
        return;
    }
    if (s_show_buf) {
        heap_caps_free(s_show_buf);
    }
    s_show_buf = pf->buf;
    FillDsc(&s_show_dsc, s_show_buf, pf->w, pf->h);
    delete pf;
    s_fail_streak = 0;
    if (s_ui.img) {
        lv_image_set_src(s_ui.img, nullptr);
        lv_obj_set_size(s_ui.img, kScreenSize, kScreenSize);
        lv_obj_center(s_ui.img);
        lv_image_set_src(s_ui.img, &s_show_dsc);
    }
    if (s_ui.hint) {
        lv_obj_add_flag(s_ui.hint, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_ui.slide_timer) {
        lv_timer_reset(s_ui.slide_timer);
    } else if (s_active.load(std::memory_order_relaxed)) {
        StartSlideTimer();
    }
    ESP_LOGI(TAG, "show ok idx=%d/%d", s_current_index,
             static_cast<int>(s_images.size()));
}

// 仅在「目录有图但一张都没成功上过屏」时显示失败文案；已有画面则保留并稍后重试。
void OnRoundAllFailed() {
    StopSlideTimer();
    if (s_show_buf != nullptr) {
        ESP_LOGW(TAG,
                 "round failed but keep last frame on screen, retry in %u ms",
                 static_cast<unsigned>(kRetryAfterFailMs));
        ArmRetryTimer();
        return;
    }
    if (s_ui.img) {
        lv_image_set_src(s_ui.img, nullptr);
    }
    if (s_ui.hint) {
        if (s_images.empty()) {
            lv_label_set_text(s_ui.hint, I18n::T("在 SD 卡\n/badge 或 /bagclip\n放入图片"));
        } else {
            lv_label_set_text(s_ui.hint, I18n::T("图片读取失败\n已跳过无法显示的文件"));
        }
        lv_obj_remove_flag(s_ui.hint, LV_OBJ_FLAG_HIDDEN);
    }
}

lv_timer_t* s_retry_timer = nullptr;

void OnRetryTimer(lv_timer_t* t) {
    if (s_retry_timer == t) {
        s_retry_timer = nullptr;
    }
    if (!s_alive.load(std::memory_order_relaxed) ||
        !s_active.load(std::memory_order_relaxed) || s_images.empty()) {
        return;
    }
    s_fail_streak = 0;
    ESP_LOGI(TAG, "retry slideshow from idx=%d", s_current_index);
    StartDecode(s_current_index);
    StartSlideTimer();
}

void ArmRetryTimer() {
    if (s_retry_timer) {
        lv_timer_delete(s_retry_timer);
        s_retry_timer = nullptr;
    }
    s_retry_timer = lv_timer_create(OnRetryTimer, kRetryAfterFailMs, nullptr);
    lv_timer_set_repeat_count(s_retry_timer, 1);
}

void CancelRetryTimer() {
    if (s_retry_timer) {
        lv_timer_delete(s_retry_timer);
        s_retry_timer = nullptr;
    }
}

// 读/解码失败：不删列表，只切到下一张继续轮播。
void OnDecodeFailed(void* arg) {
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    s_decode_busy.store(false, std::memory_order_release);
    if (!s_alive.load(std::memory_order_relaxed) ||
        !s_active.load(std::memory_order_relaxed) || s_images.empty()) {
        return;
    }
    s_fail_streak++;
    const int n = static_cast<int>(s_images.size());
    if (s_fail_streak >= n) {
        ESP_LOGW(TAG, "all %d images failed this round", n);
        OnRoundAllFailed();
        return;
    }
    s_current_index = (index + 1) % n;
    ESP_LOGI(TAG, "try next after skip -> idx=%d/%d", s_current_index, n);
    StartDecode(s_current_index);
}

void RequestSkipImage(int index, const char* reason, const char* path) {
    ESP_LOGW(TAG, "%s, skip (keep switching): %s", reason, path ? path : "?");
    if (s_alive.load(std::memory_order_relaxed)) {
        screen_async_call(OnDecodeFailed,
                      reinterpret_cast<void*>(static_cast<intptr_t>(index)));
    } else {
        s_decode_busy.store(false, std::memory_order_release);
    }
}

void DecodeOne(int index) {
    if (index < 0 || index >= static_cast<int>(s_images.size())) {
        s_decode_busy.store(false, std::memory_order_release);
        return;
    }
    const ImageEntry& entry = s_images[static_cast<size_t>(index)];
    const std::string path = entry.path;
    const bool jpeg = entry.jpeg;
    ESP_LOGI(TAG, "decode idx=%d/%d %s", index, static_cast<int>(s_images.size()),
             path.c_str());
    uint8_t* fdata = nullptr;
    size_t flen = 0;
    if (!LoadFile(path, &fdata, &flen)) {
        RequestSkipImage(index, "load failed", path.c_str());
        return;
    }
    uint8_t* raw = nullptr;
    int rw = 0, rh = 0, rs = 0;
    const bool ok = jpeg ? DecodeJpeg(fdata, flen, &raw, &rw, &rh, &rs)
                         : DecodePng(fdata, flen, &raw, &rw, &rh, &rs);
    heap_caps_free(fdata);
    if (!ok || !raw) {
        if (raw) {
            heap_caps_free(raw);
        }
        RequestSkipImage(index, "decode failed", path.c_str());
        return;
    }
    uint8_t* cover = CoverScale(raw, rw, rh, rs, kScreenSize);
    heap_caps_free(raw);
    if (!cover) {
        RequestSkipImage(index, "cover scale OOM", path.c_str());
        return;
    }
    auto* pf = new PendingFrame{cover, kScreenSize, kScreenSize};
    PendingFrame* old = s_pending.exchange(pf, std::memory_order_acq_rel);
    if (old) {
        heap_caps_free(old->buf);
        delete old;
    }
    if (s_alive.load(std::memory_order_relaxed) &&
        s_active.load(std::memory_order_relaxed)) {
        screen_async_call(ApplyPendingFrame, nullptr);
    } else {
        PendingFrame* stale = s_pending.exchange(nullptr, std::memory_order_acq_rel);
        if (stale) {
            heap_caps_free(stale->buf);
            delete stale;
        }
        s_decode_busy.store(false, std::memory_order_release);
    }
}

void WorkerTask(void* /*arg*/) {
    while (!s_worker_stop.load(std::memory_order_relaxed)) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_worker_stop.load(std::memory_order_relaxed)) {
            break;
        }
        const int index = s_job_index.exchange(-1, std::memory_order_acq_rel);
        if (index < 0) {
            s_decode_busy.store(false, std::memory_order_release);
            continue;
        }
        DecodeOne(index);
    }
    s_worker = nullptr;
    vTaskDelete(nullptr);
}

bool EnsureWorker() {
    if (s_worker != nullptr) {
        return true;
    }
    s_worker_stop.store(false, std::memory_order_release);
    // 必须低于 LVGL（adapter task_priority=4），否则 JPEG/PNG 解码会饿死触摸，
    // 表现为待机无法滑切换、长按也回不了主页。
    static constexpr UBaseType_t kWorkerPrio = 2;
    static constexpr uint32_t kStackBytes[] = {8 * 1024, 6 * 1024};
    for (uint32_t bytes : kStackBytes) {
        if (xTaskCreateWithCaps(WorkerTask, "stby_gal", bytes, nullptr, kWorkerPrio,
                                &s_worker, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) ==
            pdPASS) {
            return true;
        }
        s_worker = nullptr;
    }
    ESP_LOGW(TAG, "worker create failed heap=%u internal=%u largest_int=%u",
             static_cast<unsigned>(esp_get_free_heap_size()),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(
                 heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    return false;
}

void StopWorker() {
    if (s_worker == nullptr) {
        return;
    }
    s_worker_stop.store(true, std::memory_order_release);
    xTaskNotifyGive(s_worker);
    // worker 自删；给一点时间退出当前 decode
    for (int i = 0; i < 50 && s_worker != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_worker = nullptr;
}

void CancelDecodeDeferTimer() {
    if (s_decode_defer_timer != nullptr) {
        lv_timer_delete(s_decode_defer_timer);
        s_decode_defer_timer = nullptr;
    }
}

void OnDecodeDeferTimer(lv_timer_t* timer) {
    if (s_decode_defer_timer == timer) {
        s_decode_defer_timer = nullptr;
    }
    if (!s_alive.load(std::memory_order_relaxed) ||
        !s_active.load(std::memory_order_relaxed) || s_images.empty()) {
        return;
    }
    if (s_show_buf == nullptr) {
        StartDecode(s_current_index);
    }
}

void ScheduleDecodeAfterSettle() {
    CancelDecodeDeferTimer();
    s_decode_defer_timer = lv_timer_create(OnDecodeDeferTimer, kGalleryDecodeDeferMs, nullptr);
    lv_timer_set_repeat_count(s_decode_defer_timer, 1);
}

void StartDecode(int index) {
    if (s_images.empty()) {
        return;
    }
    if (index < 0 || index >= static_cast<int>(s_images.size())) {
        index = 0;
    }
    if (s_decode_busy.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (!EnsureWorker()) {
        screen_async_call(OnDecodeFailed,
                      reinterpret_cast<void*>(static_cast<intptr_t>(index)));
        return;
    }
    s_job_index.store(index, std::memory_order_release);
    xTaskNotifyGive(s_worker);
}
#else
void StartDecode(int /*index*/) {}
void CancelRetryTimer() {}
void StopWorker() {}
#endif

void OnSlideTimer(lv_timer_t* /*t*/) {
    if (!s_alive.load(std::memory_order_relaxed) ||
        !s_active.load(std::memory_order_relaxed) || s_images.empty()) {
        return;
    }
    // 上一张还在解时跳过本拍，避免叠任务打爆内部 DMA。
    if (s_decode_busy.load(std::memory_order_relaxed)) {
        return;
    }
    s_current_index = (s_current_index + 1) % static_cast<int>(s_images.size());
    ESP_LOGI(TAG, "slide -> idx=%d/%d", s_current_index,
             static_cast<int>(s_images.size()));
    StartDecode(s_current_index);
}

void StopSlideTimer() {
    if (s_ui.slide_timer) {
        lv_timer_delete(s_ui.slide_timer);
        s_ui.slide_timer = nullptr;
    }
}

void StartSlideTimer() {
    StopSlideTimer();
    if (s_images.empty()) {
        return;
    }
    s_ui.slide_timer = lv_timer_create(OnSlideTimer, kSlideIntervalMs, nullptr);
}

}  // namespace

lv_obj_t* StandbyGallery_Create(lv_obj_t* parent) {
    StandbyGallery_Destroy();

    lv_obj_t* root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, kScreenSize, kScreenSize);
    lv_obj_center(root);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_CLICKABLE);
    screen_make_input_passive(root);
    s_ui.root = root;

    lv_obj_t* img = lv_image_create(root);
    lv_obj_set_size(img, kScreenSize, kScreenSize);
    lv_obj_center(img);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_STRETCH);
    screen_make_input_passive(img);
    s_ui.img = img;

    lv_obj_t* hint = lv_label_create(root);
    lv_label_set_text(hint, I18n::T("在 SD 卡\n/badge 或 /bagclip\n放入图片"));
    lv_obj_set_style_text_color(hint, lv_color_hex(0xAAAAAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_center(hint);
    screen_make_input_passive(hint);
    s_ui.hint = hint;

    s_images.clear();
    s_current_index = 0;
    s_fail_streak = 0;
    ScanDir(kBadgeDir, 0, &s_images);
    if (s_images.empty()) {
        ScanDir(kBagclipDir, 0, &s_images);
    }
    ESP_LOGI(TAG, "gallery images=%d (prefer badge)", static_cast<int>(s_images.size()));
    if (!s_images.empty()) {
        lv_obj_add_flag(hint, LV_OBJ_FLAG_HIDDEN);
    }

    s_alive.store(true, std::memory_order_release);
    s_active.store(false, std::memory_order_release);
#if defined(ESP_PLATFORM)
    // 尽早建常驻 worker，避免运行中内部 DRAM 碎裂后建不了任务。
    EnsureWorker();
#endif
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    return root;
}

void StandbyGallery_SetActive(bool active) {
    if (s_ui.root == nullptr) {
        return;
    }
    const bool was_active = s_active.load(std::memory_order_relaxed);
    // 已激活时仅同步显隐；勿重复触发解码。
    if (was_active == active) {
        if (active) {
            lv_obj_remove_flag(s_ui.root, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_ui.root, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    s_active.store(active, std::memory_order_release);
    if (active) {
        lv_obj_remove_flag(s_ui.root, LV_OBJ_FLAG_HIDDEN);
        s_fail_streak = 0;
#if defined(ESP_PLATFORM)
        CancelRetryTimer();
#endif
        ESP_LOGI(TAG, "gallery face on, images=%d idx=%d",
                 static_cast<int>(s_images.size()), s_current_index);
        if (!s_images.empty()) {
            if (s_show_buf == nullptr) {
                ScheduleDecodeAfterSettle();
            }
            StartSlideTimer();
        } else if (s_ui.hint) {
            lv_label_set_text(s_ui.hint, I18n::T("在 SD 卡\n/badge 或 /bagclip\n放入图片"));
            lv_obj_remove_flag(s_ui.hint, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        lv_obj_add_flag(s_ui.root, LV_OBJ_FLAG_HIDDEN);
        StopSlideTimer();
        CancelDecodeDeferTimer();
#if defined(ESP_PLATFORM)
        CancelRetryTimer();
#endif
        ESP_LOGI(TAG, "gallery face off");
    }
}

void StandbyGallery_Destroy() {
    s_alive.store(false, std::memory_order_release);
    s_active.store(false, std::memory_order_release);
    StopSlideTimer();
#if defined(ESP_PLATFORM)
    CancelDecodeDeferTimer();
    CancelRetryTimer();
    StopWorker();
#endif

    PendingFrame* pf = s_pending.exchange(nullptr, std::memory_order_acq_rel);
    if (pf) {
        heap_caps_free(pf->buf);
        delete pf;
    }
    if (s_show_buf) {
        if (s_ui.img) {
            lv_image_set_src(s_ui.img, nullptr);
        }
        heap_caps_free(s_show_buf);
        s_show_buf = nullptr;
    }
    s_ui = {};
    s_images.clear();
    s_current_index = 0;
    s_fail_streak = 0;
    s_decode_busy.store(false, std::memory_order_release);
}
