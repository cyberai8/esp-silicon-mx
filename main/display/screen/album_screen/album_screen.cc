#include "album_screen.h"

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_jpeg_common.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if defined(ESP_PLATFORM)
#include "freertos/idf_additions.h"
#include <png.h>
#include <setjmp.h>
#include <jpeglib.h>
#endif

#include "config.h"
#include "home_screen/home_screen.h"
#include "i18n.h"
#include "screen_util.h"
#include "SdCardManager.hpp"

LV_FONT_DECLARE(font_puhui_20_4);

#if !defined(ESP_PLATFORM)
extern "C" unsigned lodepng_decode32(unsigned char** out, unsigned* w, unsigned* h,
                                     const unsigned char* in, size_t insize);
#endif

namespace {

constexpr const char* TAG = "AlbumScreen";

constexpr int kScanMaxDepth = 5;
constexpr size_t kMaxPhotos = 300;
// 缩略图常驻上限：68*68*2 ≈ 9KB 一张，48 张不到 0.5MB，超了按“离视口最远”淘汰。
constexpr size_t kMaxThumbs = 48;
// 单张图片读进内存的上限。再大的图解码期间峰值内存不可控，直接标记为不可预览。
constexpr size_t kMaxFileBytes = 6u * 1024u * 1024u;
// EXIF 的 APP1 段长度字段是 16 位的，内嵌缩略图必然落在文件头这一段里。
constexpr size_t kExifProbeBytes = 72u * 1024;
// PNG 先按整数倍抽行抽列降到这块预算以内，再盒式缩到目标尺寸。
constexpr size_t kPngDecodeMaxBytes = 2u * 1024u * 1024u;
constexpr int kPngMaxSide = 8192;
// progressive 要把整图 DCT 系数摊在内存里，大约 3 字节/像素。预算按 PSRAM
// 最大连续块动态给，超了再走 DC 扫描 1/8 预览，避免直接放弃显示。
constexpr size_t kJpegSoftMinBytes = 4u * 1024u * 1024u;
constexpr size_t kJpegSoftMaxBytes = 16u * 1024u * 1024u;
constexpr size_t kJpegSoftKeepBytes = 2u * 1024u * 1024u;

constexpr uint32_t kColorBg = 0x0B0D10;
constexpr uint32_t kColorBgGrad = 0x14171C;
constexpr uint32_t kColorText = 0xFFFFFF;
constexpr uint32_t kColorMuted = 0x8B92A3;
constexpr uint32_t kColorAccent = 0xFFC061;
constexpr uint32_t kColorCell = 0x1A1E26;
constexpr uint32_t kColorCellPressed = 0x252A34;

// 360 圆屏：可视区是内切圆，半径 180。下面每个 y 都按 sqrt(180²-dy²) 反推过可用
// 宽度，改布局要一起复算，否则内容会被圆角切掉。
// 返回键贴在 (36,28) 时左上角会落到圆外，只剩一角；沿圆弧内收到
// (78,56)，四角距圆心约 160，留 ~20px 边距，整颗按钮（含圆底）都看得见。
constexpr int32_t kPanel = DISPLAY_WIDTH;
constexpr int32_t kBackBtnSize = 40;
constexpr int32_t kBackBtnX = 78;
constexpr int32_t kBackBtnY = 56;
constexpr int32_t kTopLabelY = 20;

constexpr int32_t kThumb = 68;
constexpr int32_t kGridCols = 3;
constexpr int32_t kGridGap = 6;
constexpr int32_t kGridPad = 4;
constexpr int32_t kGridBoxW = kThumb * kGridCols + kGridGap * (kGridCols - 1) + kGridPad * 2;
// 网格顶边要避开返回键底（56+40=96），再留一点空隙。
constexpr int32_t kGridTop = 104;
constexpr int32_t kGridH = 210;
constexpr int32_t kRowStride = kThumb + kGridGap;
constexpr size_t kThumbBytes = static_cast<size_t>(kThumb) * kThumb * 2;

// 看图页：整张图要落在内切圆里才不会被圆屏边缘吃掉，所以按「对角线不超过直径」
// 缩放（4:3 的图约 288×216），而不是铺满 360 让四角被切。
constexpr int32_t kViewDiag = 352;
// 尺寸未知时的保守方框，边长取内接正方形（180*√2≈254）再留点余量。
constexpr int32_t kViewSafeBox = 248;
constexpr int32_t kViewCounterY = 20;
constexpr int32_t kViewInfoY = 312;
constexpr int32_t kViewTextW = 240;
constexpr int32_t kSwipeMinPx = 50;
constexpr int32_t kSwipeMaxDy = 70;
constexpr int32_t kTapMaxPx = 14;

struct Photo {
    std::string path;
    std::string name;
    uint32_t size_kb = 0;
    uint32_t cache_key = 0;  // 路径+大小+修改时间的散列，用来找落盘的缩略图
    bool jpeg = false;
    bool png = false;
    int src_w = 0;  // 0 = 还不知道原始尺寸
    int src_h = 0;

    // 缩略图：worker 线程产出，UI 线程挂载与释放。
    uint8_t* thumb = nullptr;
    lv_image_dsc_t thumb_dsc = {};
    bool thumb_ready = false;   // 数据好了，还没挂到 cell
    bool thumb_skip = false;    // 不做缩略图（解码失败 / 太大）
    bool generating = false;    // worker 正在处理
};

// 一次扫描的结果快照。UI 线程和 worker 各持一份 shared_ptr，谁最后放手谁负责回收，
// 于是换扫描结果时不需要「等 worker 停下来」——它手里的 Photo* 一直有效。
struct PhotoList {
    std::vector<Photo*> items;

    ~PhotoList() {
        for (Photo* p : items) {
            if (p->thumb != nullptr) {
                // 只还内存。LVGL 那边的引用由 UI 线程在放弃快照前先摘干净。
                heap_caps_free(p->thumb);
            }
            delete p;
        }
    }
};
using PhotoListPtr = std::shared_ptr<PhotoList>;

struct ViewImage {
    uint8_t* buf = nullptr;
    int w = 0;
    int h = 0;
    int index = -1;
};

struct AlbumUi {
    lv_obj_t* scr = nullptr;
    lv_obj_t* lbl_top = nullptr;
    lv_obj_t* grid = nullptr;

    lv_obj_t* view_layer = nullptr;
    lv_obj_t* view_img = nullptr;
    lv_obj_t* view_counter = nullptr;
    lv_obj_t* view_info = nullptr;
    lv_obj_t* view_hint = nullptr;
    lv_obj_t* view_back = nullptr;

    lv_obj_t* state_layer = nullptr;
    lv_obj_t* state_title = nullptr;
    lv_obj_t* state_sub = nullptr;
    lv_obj_t* state_btn = nullptr;

    lv_timer_t* tick = nullptr;
};

AlbumUi s_ui;
std::vector<lv_obj_t*> s_cells;  // 与 s_ui_list 同序，只有 UI 线程碰
bool s_screen_active = false;
bool s_chrome_visible = true;
bool s_state_shown = false;
int32_t s_press_x = 0;
int32_t s_press_y = 0;
uint32_t s_press_tick = 0;
int32_t s_view_drag = 0;
bool s_view_sliding = false;
std::atomic<bool> s_grid_scrolling{false};

std::mutex s_photos_mutex;
PhotoListPtr s_photos;  // 扫描线程发布，worker 取快照；只有这个指针受锁保护
PhotoListPtr s_ui_list;  // UI 线程自己那一份，只有 UI 线程碰，所以取 Photo* 不用锁

std::mutex s_thumb_mutex;   // 保护 Photo 里的缩略图字段
std::atomic<int> s_view_center{0};

std::atomic<bool> s_scanning{false};
std::atomic<bool> s_scan_done{false};
std::atomic<bool> s_scan_abort{false};
std::atomic<int> s_scan_found{0};
std::atomic<bool> s_sd_ready{false};
std::atomic<bool> s_need_worker{false};

// worker 的生命周期用「代号」管：换一代就等于让老 worker 自己退，调用方不用阻塞等
// 待，老 worker 手里的照片列表由 shared_ptr 兜着，不会被提前释放。
std::atomic<int> s_worker_gen{0};
std::atomic<int> s_view_req{-1};
std::mutex s_view_mutex;
ViewImage s_view_pending;   // worker -> UI
ViewImage s_view_current;   // UI 持有，正在显示
lv_image_dsc_t s_view_dsc;  // 看图页固定用这一个描述符，换图前记得丢缓存
int s_view_index = -1;      // 当前看的是第几张，-1 = 没开看图页

inline lv_style_selector_t Sel(lv_part_t part, lv_state_t state) {
    return static_cast<lv_style_selector_t>(part) | static_cast<lv_style_selector_t>(state);
}

// ---------------------------------------------------------------------------
// 文件名 / 文件头
// ---------------------------------------------------------------------------

bool ExtEquals(const char* name, const char* ext) {
    const char* dot = strrchr(name, '.');
    if (dot == nullptr) {
        return false;
    }
    ++dot;
    while (*dot != '\0' && *ext != '\0') {
        if (tolower(static_cast<unsigned char>(*dot)) != *ext) {
            return false;
        }
        ++dot;
        ++ext;
    }
    return *dot == '\0' && *ext == '\0';
}

bool IsJpegName(const char* name) {
    return ExtEquals(name, "jpg") || ExtEquals(name, "jpeg");
}

bool IsPngName(const char* name) {
    return ExtEquals(name, "png");
}

bool IsImageName(const char* name) {
    return IsJpegName(name) || IsPngName(name);
}

const char* ExtLabel(const std::string& name) {
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos) {
        return "IMG";
    }
    static char ext[8];
    size_t n = 0;
    for (size_t i = dot + 1; i < name.size() && n + 1 < sizeof(ext); ++i, ++n) {
        ext[n] = static_cast<char>(toupper(static_cast<unsigned char>(name[i])));
    }
    ext[n] = '\0';
    return ext;
}

std::string StripExt(const std::string& name) {
    const size_t dot = name.rfind('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

// PNG 的 IHDR 就在头 24 字节里，扫描时顺手读出来，用来拦住像素数过大的图。
void ReadPngSize(const std::string& path, int* w, int* h) {
    FILE* fp = fopen(path.c_str(), "rb");
    if (fp == nullptr) {
        return;
    }
    uint8_t head[24] = {};
    const size_t n = fread(head, 1, sizeof(head), fp);
    fclose(fp);
    if (n < sizeof(head) || memcmp(head, "\x89PNG", 4) != 0 ||
        memcmp(head + 12, "IHDR", 4) != 0) {
        return;
    }
    *w = (head[16] << 24) | (head[17] << 16) | (head[18] << 8) | head[19];
    *h = (head[20] << 24) | (head[21] << 16) | (head[22] << 8) | head[23];
}

// ---------------------------------------------------------------------------
// 缩略图落盘缓存
// ---------------------------------------------------------------------------

// 目录名以点开头，扫描时会被跳过，不会被当成照片目录。
constexpr const char* kThumbCacheSubdir = ".albumthm";
constexpr uint32_t kThumbCacheMagic = 0x31485441;  // 'ATH1'

struct ThumbCacheHeader {
    uint32_t magic;
    uint16_t w;
    uint16_t h;
    uint16_t src_w;  // 原图尺寸也存下来，命中缓存时信息栏不用再去读文件
    uint16_t src_h;
};

uint32_t ThumbCacheKey(const std::string& path, uint32_t size, uint32_t mtime) {
    uint32_t h = 2166136261u;  // FNV-1a
    for (const char c : path) {
        h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
    }
    h = (h ^ size) * 16777619u;
    h = (h ^ mtime) * 16777619u;
    return h;
}

// 8.3 短名，避免受 FATFS 长文件名/代码页设置影响。
void ThumbCachePath(uint32_t key, char* out, size_t out_len) {
    snprintf(out, out_len, "%s/%s/%08lX.THM", SdCardManager::kMountPoint, kThumbCacheSubdir,
             static_cast<unsigned long>(key));
}

bool ThumbCacheRead(uint32_t key, uint8_t* dst, int* src_w, int* src_h) {
    char path[64];
    ThumbCachePath(key, path, sizeof(path));
    FILE* fp = fopen(path, "rb");
    if (fp == nullptr) {
        return false;
    }
    ThumbCacheHeader head = {};
    const bool ok = fread(&head, 1, sizeof(head), fp) == sizeof(head) &&
                    head.magic == kThumbCacheMagic && head.w == kThumb && head.h == kThumb &&
                    fread(dst, 1, kThumbBytes, fp) == kThumbBytes;
    fclose(fp);
    if (!ok) {
        unlink(path);  // 半截或换过格式的缓存直接扔掉，下次重新生成
        return false;
    }
    *src_w = head.src_w;
    *src_h = head.src_h;
    return true;
}

void ThumbCacheWrite(uint32_t key, const uint8_t* src, int src_w, int src_h) {
    static bool s_dir_ready = false;
    if (!s_dir_ready) {
        char dir[64];
        snprintf(dir, sizeof(dir), "%s/%s", SdCardManager::kMountPoint, kThumbCacheSubdir);
        struct stat st = {};
        if (stat(dir, &st) != 0 && mkdir(dir, 0777) != 0) {
            ESP_LOGW(TAG, "thumb cache dir unavailable");
            return;
        }
        s_dir_ready = true;
    }
    char path[64];
    ThumbCachePath(key, path, sizeof(path));
    FILE* fp = fopen(path, "wb");
    if (fp == nullptr) {
        return;
    }
    const ThumbCacheHeader head = {kThumbCacheMagic, kThumb, kThumb,
                                   static_cast<uint16_t>(src_w > 0 ? src_w : 0),
                                   static_cast<uint16_t>(src_h > 0 ? src_h : 0)};
    if (fwrite(&head, 1, sizeof(head), fp) != sizeof(head) ||
        fwrite(src, 1, kThumbBytes, fp) != kThumbBytes) {
        fclose(fp);
        unlink(path);
        return;
    }
    fclose(fp);
}

// ---------------------------------------------------------------------------
// 扫描
// ---------------------------------------------------------------------------

void ScanDir(const std::string& dir, int depth, std::vector<Photo*>* out) {
    if (depth > kScanMaxDepth || out->size() >= kMaxPhotos ||
        s_scan_abort.load(std::memory_order_relaxed)) {
        return;
    }
    DIR* d = opendir(dir.c_str());
    if (d == nullptr) {
        return;
    }
    while (out->size() < kMaxPhotos && !s_scan_abort.load(std::memory_order_relaxed)) {
        const dirent* ent = readdir(d);
        if (ent == nullptr) {
            break;
        }
        if (ent->d_name[0] == '.') {
            continue;
        }
        std::string path = dir;
        path += '/';
        path += ent->d_name;

        struct stat st = {};
        if (stat(path.c_str(), &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            // system/ 放的是数字人表情等系统资源，不当照片看。
            if (strcmp(ent->d_name, "system") == 0) {
                continue;
            }
            ScanDir(path, depth + 1, out);
        } else if (S_ISREG(st.st_mode) && IsImageName(ent->d_name)) {
            auto* p = new Photo;
            p->path = path;
            p->name = ent->d_name;
            p->size_kb = static_cast<uint32_t>(st.st_size / 1024);
            p->cache_key = ThumbCacheKey(path, static_cast<uint32_t>(st.st_size),
                                         static_cast<uint32_t>(st.st_mtime));
            p->jpeg = IsJpegName(ent->d_name);
            p->png = IsPngName(ent->d_name);
            if (p->png) {
                ReadPngSize(path, &p->src_w, &p->src_h);
            }
            if (st.st_size > static_cast<off_t>(kMaxFileBytes)) {
                p->thumb_skip = true;
            }
            out->push_back(p);
            s_scan_found.store(static_cast<int>(out->size()), std::memory_order_relaxed);
        }
    }
    closedir(d);
}

void ScanTask(void* /*arg*/) {
    auto list = std::make_shared<PhotoList>();
    const bool mounted = SdCardManager::GetInstance().Mount();
    s_sd_ready.store(mounted, std::memory_order_relaxed);
    if (mounted) {
        ScanDir(SdCardManager::kMountPoint, 0, &list->items);
    } else {
        ESP_LOGW(TAG, "SD mount failed, album empty");
    }
    ESP_LOGI(TAG, "scanned %u photos", static_cast<unsigned>(list->items.size()));

    {
        // 中止判断要和发布放在同一把锁里：否则可能在 UI 已经清空列表之后才发布，
        // 那批 Photo 就没人回收了。
        std::lock_guard<std::mutex> lock(s_photos_mutex);
        if (!s_scan_abort.load(std::memory_order_relaxed)) {
            s_photos = list;
        }
    }
    list.reset();  // 中止时这里就把整批连内存一起回收了

    s_scan_done.store(true, std::memory_order_relaxed);
    s_scanning.store(false, std::memory_order_relaxed);
#if defined(ESP_PLATFORM)
    vTaskDeleteWithCaps(nullptr);
#else
    vTaskDelete(nullptr);
#endif
}

// UI 线程：先摘掉 LVGL 对旧缩略图的引用，再放弃自己那份快照。
void ReleaseUiList() {
    if (!s_ui_list) {
        return;
    }
    std::lock_guard<std::mutex> lock(s_thumb_mutex);
    for (Photo* p : s_ui_list->items) {
        if (p->thumb != nullptr) {
            lv_image_cache_drop(&p->thumb_dsc);
        }
    }
    s_ui_list.reset();
}

bool SpawnAlbumTask(TaskFunction_t fn, const char* name, uint32_t stack_words, void* arg,
                    UBaseType_t prio, BaseType_t core) {
    for (int i = 0; i < 8; ++i) {
#if defined(ESP_PLATFORM)
        const uint32_t stack_bytes = stack_words * sizeof(StackType_t);
        const BaseType_t ok =
            (core >= 0)
                ? xTaskCreatePinnedToCoreWithCaps(fn, name, stack_bytes, arg, prio, nullptr,
                                                  core, MALLOC_CAP_SPIRAM)
                : xTaskCreateWithCaps(fn, name, stack_bytes, arg, prio, nullptr,
                                      MALLOC_CAP_SPIRAM);
#else
        const BaseType_t ok =
            (core >= 0) ? xTaskCreatePinnedToCore(fn, name, stack_words, arg, prio, nullptr,
                                                  core)
                        : xTaskCreate(fn, name, stack_words, arg, prio, nullptr);
#endif
        if (ok == pdPASS) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return false;
}

void StartScan() {
    // 上一轮扫描可能刚被中止、还在收尾。它看到 abort 后退得很快，等一小会儿就好。
    for (int i = 0; i < 12 && s_scanning.load(std::memory_order_relaxed); ++i) {
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    if (s_scanning.load(std::memory_order_relaxed)) {
        ESP_LOGW(TAG, "previous scan still running, skip");
        return;
    }
    ReleaseUiList();
    {
        std::lock_guard<std::mutex> lock(s_photos_mutex);
        s_photos.reset();
    }
    s_scan_found.store(0, std::memory_order_relaxed);
    s_scan_done.store(false, std::memory_order_relaxed);
    s_scan_abort.store(false, std::memory_order_relaxed);
    s_scanning.store(true, std::memory_order_relaxed);
    if (!SpawnAlbumTask(ScanTask, "album_scan", 6144, nullptr, 4, -1)) {
        // 建不出任务就别把界面永久卡在「正在扫描」，让它落到空态去。
        ESP_LOGE(TAG, "scan task create failed");
        s_scanning.store(false, std::memory_order_relaxed);
        s_scan_done.store(true, std::memory_order_relaxed);
    }
}

// ---------------------------------------------------------------------------
// JPEG 解码：降采样 + 盒式缩放
// ---------------------------------------------------------------------------

void* AllocBig(size_t len) {
    len = (len + 15) & ~static_cast<size_t>(15);
    void* p = heap_caps_aligned_alloc(16, len, MALLOC_CAP_SPIRAM);
    // 缩略图这种小块内部 RAM 还能兜一下；大块绝不挤内部堆，否则 WiFi/音频要出事。
    if (p == nullptr && len <= 32u * 1024) {
        p = heap_caps_aligned_alloc(16, len, MALLOC_CAP_DEFAULT);
    }
    return p;
}

// 只读文件头一段，用来找 EXIF 内嵌缩略图。
bool LoadFilePrefix(const std::string& path, size_t want, uint8_t** out, size_t* out_len) {
    *out = nullptr;
    *out_len = 0;
    FILE* fp = fopen(path.c_str(), "rb");
    if (fp == nullptr) {
        return false;
    }
    auto* buf = static_cast<uint8_t*>(AllocBig(want));
    if (buf == nullptr) {
        fclose(fp);
        return false;
    }
    const size_t got = fread(buf, 1, want, fp);
    fclose(fp);
    if (got < 128) {
        heap_caps_free(buf);
        return false;
    }
    *out = buf;
    *out_len = got;
    return true;
}

bool LoadFile(const std::string& path, uint8_t** out, size_t* out_len) {
    *out = nullptr;
    *out_len = 0;
    FILE* fp = fopen(path.c_str(), "rb");
    if (fp == nullptr) {
        return false;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return false;
    }
    const long size = ftell(fp);
    rewind(fp);
    if (size <= 0 || static_cast<size_t>(size) > kMaxFileBytes) {
        fclose(fp);
        return false;
    }
    // 整张图要一次性进内存，先问问 PSRAM 有没有这么大一块连续空间，
    // 不然解码到一半失败更难看。
    if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) <
        static_cast<size_t>(size) + 64 * 1024) {
        ESP_LOGW(TAG, "psram too fragmented for %ld bytes", size);
        fclose(fp);
        return false;
    }
    auto* buf = static_cast<uint8_t*>(AllocBig(static_cast<size_t>(size)));
    if (buf == nullptr) {
        fclose(fp);
        return false;
    }
    const size_t got = fread(buf, 1, static_cast<size_t>(size), fp);
    fclose(fp);
    if (got != static_cast<size_t>(size)) {
        heap_caps_free(buf);
        return false;
    }
    *out = buf;
    *out_len = got;
    return true;
}

uint32_t Rd16(const uint8_t* p, bool le) {
    return le ? static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
              : (static_cast<uint32_t>(p[0]) << 8) | static_cast<uint32_t>(p[1]);
}

uint32_t Rd32(const uint8_t* p, bool le) {
    return le ? static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                    (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24)
              : (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
                    (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

// 手机和相机拍的 JPEG 基本都在 APP1(EXIF) 里塞了一张 160×120 的缩略图，位置写在
// 第二个 IFD 的 0x0201/0x0202 两个标签里。找到它就不用把几 MB 的原图读进来了。
bool FindExifThumb(const uint8_t* buf, size_t len, size_t* off, size_t* size) {
    if (len < 16 || buf[0] != 0xFF || buf[1] != 0xD8) {
        return false;
    }
    size_t p = 2;
    while (p + 4 <= len) {
        if (buf[p] != 0xFF) {
            return false;
        }
        const uint8_t marker = buf[p + 1];
        if (marker == 0xFF) {  // 填充字节
            ++p;
            continue;
        }
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            p += 2;
            continue;
        }
        if (marker == 0xDA || marker == 0xD9) {
            return false;  // 走到扫描数据了，这张图没有 EXIF 缩略图
        }
        const size_t seg_len = Rd16(buf + p + 2, false);
        if (seg_len < 2 || p + 2 + seg_len > len) {
            return false;
        }
        if (marker != 0xE1 || seg_len < 16 || memcmp(buf + p + 4, "Exif\0\0", 6) != 0) {
            p += 2 + seg_len;
            continue;
        }

        const uint8_t* tiff = buf + p + 10;  // 跳过 FFE1 + 长度 + "Exif\0\0"
        const size_t tiff_len = seg_len - 8;
        bool le = true;
        if (memcmp(tiff, "II", 2) == 0) {
            le = true;
        } else if (memcmp(tiff, "MM", 2) == 0) {
            le = false;
        } else {
            return false;
        }
        const uint32_t ifd0 = Rd32(tiff + 4, le);
        if (ifd0 < 8 || ifd0 + 2 > tiff_len) {
            return false;
        }
        const uint32_t n0 = Rd16(tiff + ifd0, le);
        const uint32_t next_pos = ifd0 + 2 + n0 * 12;
        if (next_pos + 4 > tiff_len) {
            return false;
        }
        const uint32_t ifd1 = Rd32(tiff + next_pos, le);
        if (ifd1 < 8 || ifd1 + 2 > tiff_len) {
            return false;
        }
        const uint32_t n1 = Rd16(tiff + ifd1, le);
        uint32_t t_off = 0;
        uint32_t t_len = 0;
        for (uint32_t i = 0; i < n1; ++i) {
            const uint32_t e = ifd1 + 2 + i * 12;
            if (e + 12 > tiff_len) {
                break;
            }
            const uint32_t tag = Rd16(tiff + e, le);
            if (tag == 0x0201) {
                t_off = Rd32(tiff + e + 8, le);
            } else if (tag == 0x0202) {
                t_len = Rd32(tiff + e + 8, le);
            }
        }
        if (t_off == 0 || t_len < 256) {
            return false;
        }
        const size_t abs = static_cast<size_t>(tiff - buf) + t_off;
        if (abs + t_len > len || buf[abs] != 0xFF || buf[abs + 1] != 0xD8) {
            return false;
        }
        *off = abs;
        *size = t_len;
        return true;
    }
    return false;
}

// esp_new_jpeg 只认 baseline（SOF0/SOF1）。progressive（SOF2）和算术编码走
// libjpeg-turbo 软解；自己扫 SOF 是为了选解码器，避免把 progressive 喂给硬件
// 解码器刷 JPEG_DEC: Not supported JPEG standard。
enum class JpegStd : uint8_t { Invalid, Baseline, Progressive, Other };

struct JpegSof {
    JpegStd std = JpegStd::Invalid;
    int w = 0;
    int h = 0;
    int nf = 0;  // 1=灰度 3=YCbCr 4=CMYK，解码器只吃 1/3
};

const char* JpegStdName(JpegStd s) {
    switch (s) {
        case JpegStd::Baseline:
            return "baseline";
        case JpegStd::Progressive:
            return "progressive";
        case JpegStd::Other:
            return "unsupported";
        default:
            return "not-jpeg";
    }
}

JpegSof ParseJpegSof(const uint8_t* data, size_t len) {
    JpegSof out;
    if (data == nullptr || len < 4 || data[0] != 0xFF || data[1] != 0xD8) {
        return out;
    }
    size_t p = 2;
    while (p + 4 <= len) {
        if (data[p] != 0xFF) {
            ++p;
            continue;
        }
        const uint8_t marker = data[p + 1];
        if (marker == 0xFF) {
            ++p;
            continue;
        }
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            p += 2;
            continue;
        }
        if (marker == 0xD9 || marker == 0xDA) {
            break;
        }
        const size_t seg_len = Rd16(data + p + 2, false);
        if (seg_len < 2 || p + 2 + seg_len > len) {
            break;
        }
        const bool is_sof = (marker >= 0xC0 && marker <= 0xCF) && marker != 0xC4 &&
                            marker != 0xC8 && marker != 0xCC;
        if (is_sof && seg_len >= 8) {
            out.h = static_cast<int>(Rd16(data + p + 5, false));
            out.w = static_cast<int>(Rd16(data + p + 7, false));
            out.nf = data[p + 9];
            if (marker == 0xC0 || marker == 0xC1) {
                out.std = JpegStd::Baseline;
            } else if (marker == 0xC2) {
                out.std = JpegStd::Progressive;
            } else {
                out.std = JpegStd::Other;
            }
            return out;
        }
        p += 2 + seg_len;
    }
    return out;
}

bool JpegCanHwDecode(const JpegSof& sof) {
    return sof.std == JpegStd::Baseline && sof.w > 0 && sof.h > 0 &&
           (sof.nf == 1 || sof.nf == 3);
}

bool JpegCanSoftDecode(const JpegSof& sof) {
    return (sof.std == JpegStd::Progressive || sof.std == JpegStd::Other) && sof.w > 0 &&
           sof.h > 0 && sof.w <= kPngMaxSide && sof.h <= kPngMaxSide &&
           (sof.nf == 1 || sof.nf == 3);
}

bool DecodeJpegSoftIo(const uint8_t* data, size_t len, FILE* fp, int min_w, int min_h,
                      bool cover, uint8_t** out, int* out_w, int* out_h, int* out_stride_px,
                      int* orig_w, int* orig_h);
bool DecodeJpegSoftFromFile(const char* path, int min_w, int min_h, bool cover, uint8_t** out,
                            int* out_w, int* out_h, int* out_stride_px, int* orig_w,
                            int* orig_h);
bool DecodeJpegDcPreview(const uint8_t* data, size_t len, FILE* fp, uint8_t** out, int* out_w,
                         int* out_h, int* out_stride_px, int* orig_w, int* orig_h);

// 解出「不小于 min_w×min_h」的最小一档（1、1/2、1/4、1/8）。
// cover=true 时按长边贴合，用于方形缩略图；false 按短边贴合，用于整图铺满。
bool DecodeJpegScaled(const uint8_t* data, size_t len, int min_w, int min_h, bool cover,
                      uint8_t** out, int* out_w, int* out_h, int* out_stride_px, int* orig_w,
                      int* orig_h) {
    *out = nullptr;
    const JpegSof sof = ParseJpegSof(data, len);
    if (JpegCanSoftDecode(sof)) {
        if (DecodeJpegSoftIo(data, len, nullptr, min_w, min_h, cover, out, out_w, out_h,
                             out_stride_px, orig_w, orig_h)) {
            return true;
        }
        ESP_LOGW(TAG, "libjpeg progressive failed, try DC preview");
        return DecodeJpegDcPreview(data, len, nullptr, out, out_w, out_h, out_stride_px,
                                   orig_w, orig_h);
    }
    if (!JpegCanHwDecode(sof)) {
        return false;
    }
    const int sw = sof.w;
    const int sh = sof.h;
    if (orig_w != nullptr) {
        *orig_w = sw;
    }
    if (orig_h != nullptr) {
        *orig_h = sh;
    }

    const float fx = static_cast<float>(min_w) / static_cast<float>(sw);
    const float fy = static_cast<float>(min_h) / static_cast<float>(sh);
    const float need = cover ? (fx > fy ? fx : fy) : (fx < fy ? fx : fy);
    int div = 1;
    if (need <= 0.125f) {
        div = 8;
    } else if (need <= 0.25f) {
        div = 4;
    } else if (need <= 0.5f) {
        div = 2;
    }

    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    int dw = sw;
    int dh = sh;
    if (div > 1) {
        // scale 必须是 8 的整数倍，且不能小于原图的 1/8，所以向上取整。
        dw = ((sw / div) + 7) / 8 * 8;
        dh = ((sh / div) + 7) / 8 * 8;
        cfg.scale.width = dw;
        cfg.scale.height = dh;
    }

    jpeg_dec_handle_t dec = nullptr;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        return false;
    }
    jpeg_dec_io_t io = {};
    io.inbuf = const_cast<uint8_t*>(data);
    io.inbuf_len = static_cast<int>(len);
    jpeg_dec_header_info_t info = {};
    if (jpeg_dec_parse_header(dec, &io, &info) != JPEG_ERR_OK) {
        jpeg_dec_close(dec);
        return false;
    }

    int need_len = 0;
    if (jpeg_dec_get_outbuf_len(dec, &need_len) != JPEG_ERR_OK || need_len <= 0) {
        need_len = dw * dh * 2;
    }
    const size_t buf_len = static_cast<size_t>(need_len) > static_cast<size_t>(dw) * dh * 2
                               ? static_cast<size_t>(need_len)
                               : static_cast<size_t>(dw) * dh * 2;
    auto* buf = static_cast<uint8_t*>(AllocBig(buf_len));
    if (buf == nullptr) {
        ESP_LOGW(TAG, "no mem for %dx%d decode", dw, dh);
        jpeg_dec_close(dec);
        return false;
    }
    io.outbuf = buf;
    const jpeg_error_t err = jpeg_dec_process(dec, &io);
    jpeg_dec_close(dec);
    if (err != JPEG_ERR_OK) {
        heap_caps_free(buf);
        return false;
    }

    // 正常情况下输出就是紧排的 dw*dh*2；万一解码器按 16 对齐补了边，
    // 行距要跟着变，否则整张图会斜。
    int stride_px = dw;
    if (static_cast<size_t>(need_len) > static_cast<size_t>(dw) * dh * 2) {
        const int aw = (dw + 15) & ~15;
        const int ah = (dh + 15) & ~15;
        if (static_cast<size_t>(aw) * ah * 2 == static_cast<size_t>(need_len)) {
            stride_px = aw;
        }
    }

    *out = buf;
    *out_w = dw;
    *out_h = dh;
    *out_stride_px = stride_px;
    return true;
}

uint16_t PackRgb565(uint32_t r, uint32_t g, uint32_t b) {
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

size_t JpegSoftBudget() {
    const size_t largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t budget = (largest > kJpegSoftKeepBytes) ? (largest - kJpegSoftKeepBytes)
                                                   : kJpegSoftMinBytes;
    if (budget < kJpegSoftMinBytes) {
        budget = kJpegSoftMinBytes;
    }
    if (budget > kJpegSoftMaxBytes) {
        budget = kJpegSoftMaxBytes;
    }
    return budget;
}

// Progressive JPEG 太大时 libjpeg 会把整图 DCT 摊开。第一段 SOS 通常是 DC
// 扫描（Ss=Se=0），只解 DC 就能得到约 1/8 分辨率的预览，内存只要几百 KB。
struct JpegByteSrc {
    FILE* fp = nullptr;
    const uint8_t* mem = nullptr;
    size_t len = 0;
    size_t pos = 0;
};

int JpegSrcGet(JpegByteSrc* src) {
    if (src->fp != nullptr) {
        return fgetc(src->fp);
    }
    if (src->pos >= src->len) {
        return EOF;
    }
    return src->mem[src->pos++];
}

bool JpegSrcRead(JpegByteSrc* src, uint8_t* dst, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const int b = JpegSrcGet(src);
        if (b == EOF) {
            return false;
        }
        dst[i] = static_cast<uint8_t>(b);
    }
    return true;
}

uint16_t JpegSrcU16(JpegByteSrc* src) {
    const int a = JpegSrcGet(src);
    const int b = JpegSrcGet(src);
    if (a == EOF || b == EOF) {
        return 0;
    }
    return static_cast<uint16_t>((a << 8) | b);
}

struct JpegBitIn {
    JpegByteSrc* src = nullptr;
    uint32_t acc = 0;
    int nbits = 0;
    int unread_marker = 0;
};

int JpegNextByte(JpegBitIn* in) {
    for (;;) {
        const int b = JpegSrcGet(in->src);
        if (b != 0xFF) {
            return b;
        }
        int n = JpegSrcGet(in->src);
        while (n == 0xFF) {
            n = JpegSrcGet(in->src);
        }
        if (n == 0x00) {
            return 0xFF;
        }
        if (n == EOF) {
            return EOF;
        }
        in->unread_marker = n;
        return EOF;
    }
}

int JpegGetBits(JpegBitIn* in, int n) {
    while (in->nbits < n) {
        const int b = JpegNextByte(in);
        if (b == EOF) {
            return -1;
        }
        in->acc = (in->acc << 8) | static_cast<uint32_t>(b);
        in->nbits += 8;
    }
    in->nbits -= n;
    return static_cast<int>((in->acc >> in->nbits) & ((1 << n) - 1));
}

struct JpegHuff {
    int mincode[17];
    int maxcode[17];
    int valptr[17];
    uint8_t huffval[256];
    bool ok = false;
};

bool JpegBuildHuff(JpegHuff* t, const uint8_t bits[17], const uint8_t* vals, int nval) {
    memset(t, 0, sizeof(*t));
    if (nval < 0 || nval > 256) {
        return false;
    }
    memcpy(t->huffval, vals, static_cast<size_t>(nval));
    int code = 0;
    int p = 0;
    for (int l = 1; l <= 16; ++l) {
        t->valptr[l] = p;
        t->mincode[l] = code;
        p += bits[l];
        if (p > nval) {
            return false;
        }
        t->maxcode[l] = (bits[l] != 0) ? (code + bits[l] - 1) : -1;
        code = (code + bits[l]) << 1;
    }
    t->ok = true;
    return true;
}

int JpegDecodeHuff(JpegBitIn* in, const JpegHuff* t) {
    if (!t->ok) {
        return -1;
    }
    int code = JpegGetBits(in, 1);
    if (code < 0) {
        return -1;
    }
    for (int l = 1; l <= 16; ++l) {
        if (t->maxcode[l] >= 0 && code <= t->maxcode[l]) {
            return t->huffval[t->valptr[l] + (code - t->mincode[l])];
        }
        const int b = JpegGetBits(in, 1);
        if (b < 0) {
            return -1;
        }
        code = (code << 1) | b;
    }
    return -1;
}

int JpegReceiveExtend(JpegBitIn* in, int ssss) {
    if (ssss <= 0) {
        return 0;
    }
    const int v = JpegGetBits(in, ssss);
    if (v < 0) {
        return 0;
    }
    const int vt = 1 << (ssss - 1);
    if (v < vt) {
        return v + ((-1) << ssss) + 1;
    }
    return v;
}

int JpegClamp8(int v) {
    if (v < 0) {
        return 0;
    }
    if (v > 255) {
        return 255;
    }
    return v;
}

bool DecodeJpegDcPreview(const uint8_t* data, size_t len, FILE* fp, uint8_t** out, int* out_w,
                         int* out_h, int* out_stride_px, int* orig_w, int* orig_h) {
    *out = nullptr;
    JpegByteSrc src;
    src.fp = fp;
    src.mem = data;
    src.len = len;
    if (fp != nullptr) {
        fseek(fp, 0, SEEK_SET);
    }

    JpegHuff dc_huff[4];
    int qt_dc[4] = {16, 16, 16, 16};
    int width = 0;
    int height = 0;
    int nf = 0;
    int cid[4] = {};
    int ch[4] = {};
    int cv[4] = {};
    int ctq[4] = {};
    int restart = 0;
    bool sof2 = false;

    if (JpegSrcGet(&src) != 0xFF || JpegSrcGet(&src) != 0xD8) {
        return false;
    }

    for (;;) {
        int b = JpegSrcGet(&src);
        while (b == 0xFF) {
            b = JpegSrcGet(&src);
        }
        if (b == EOF) {
            return false;
        }
        const int marker = b;
        if (marker == 0xDA) {
            break;
        }
        if (marker == 0xD9) {
            return false;
        }
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            continue;
        }
        const uint16_t seglen = JpegSrcU16(&src);
        if (seglen < 2) {
            return false;
        }
        const int payload = static_cast<int>(seglen) - 2;
        if (marker == 0xC2 || marker == 0xC0 || marker == 0xC1) {
            sof2 = (marker == 0xC2);
            uint8_t hdr[6];
            if (payload < 6 || !JpegSrcRead(&src, hdr, 6)) {
                return false;
            }
            if (hdr[0] != 8) {
                return false;
            }
            height = (hdr[1] << 8) | hdr[2];
            width = (hdr[3] << 8) | hdr[4];
            nf = hdr[5];
            if (nf < 1 || nf > 4 || width < 8 || height < 8) {
                return false;
            }
            for (int i = 0; i < nf; ++i) {
                uint8_t c[3];
                if (!JpegSrcRead(&src, c, 3)) {
                    return false;
                }
                cid[i] = c[0];
                ch[i] = c[1] >> 4;
                cv[i] = c[1] & 0x0F;
                ctq[i] = c[2] & 0x0F;
                if (ch[i] < 1 || cv[i] < 1) {
                    return false;
                }
            }
            continue;
        }
        if (marker == 0xDB) {
            int left = payload;
            while (left > 0) {
                const int pq_tq = JpegSrcGet(&src);
                if (pq_tq == EOF) {
                    return false;
                }
                --left;
                const int tq = pq_tq & 0x0F;
                const int pq = pq_tq >> 4;
                const int step = (pq != 0) ? 2 : 1;
                for (int i = 0; i < 64; ++i) {
                    int v = JpegSrcGet(&src);
                    if (pq != 0) {
                        const int v2 = JpegSrcGet(&src);
                        v = (v << 8) | v2;
                        left -= 2;
                    } else {
                        --left;
                    }
                    if (i == 0 && tq >= 0 && tq < 4) {
                        qt_dc[tq] = (v > 0) ? v : 16;
                    }
                }
            }
            continue;
        }
        if (marker == 0xC4) {
            int left = payload;
            while (left > 0) {
                const int tc_th = JpegSrcGet(&src);
                if (tc_th == EOF) {
                    return false;
                }
                --left;
                uint8_t bits[17] = {};
                int nval = 0;
                for (int i = 1; i <= 16; ++i) {
                    const int li = JpegSrcGet(&src);
                    if (li == EOF) {
                        return false;
                    }
                    bits[i] = static_cast<uint8_t>(li);
                    nval += li;
                    --left;
                }
                uint8_t vals[256];
                if (nval < 0 || nval > 256 || left < nval) {
                    return false;
                }
                if (!JpegSrcRead(&src, vals, static_cast<size_t>(nval))) {
                    return false;
                }
                left -= nval;
                const int tc = tc_th >> 4;
                const int th = tc_th & 0x0F;
                if (tc == 0 && th >= 0 && th < 4) {
                    JpegBuildHuff(&dc_huff[th], bits, vals, nval);
                }
            }
            continue;
        }
        if (marker == 0xDD && payload >= 2) {
            restart = JpegSrcU16(&src);
            for (int i = 2; i < payload; ++i) {
                JpegSrcGet(&src);
            }
            continue;
        }
        for (int i = 0; i < payload; ++i) {
            if (JpegSrcGet(&src) == EOF) {
                return false;
            }
        }
    }

    if (!sof2 || width < 8 || height < 8) {
        return false;
    }

    const uint16_t sos_len = JpegSrcU16(&src);
    if (sos_len < 6) {
        return false;
    }
    const int ns = JpegSrcGet(&src);
    if (ns < 1 || ns > nf) {
        return false;
    }
    int scan_i[4] = {};
    int scan_td[4] = {};
    for (int i = 0; i < ns; ++i) {
        const int cs = JpegSrcGet(&src);
        const int tdta = JpegSrcGet(&src);
        int found = -1;
        for (int c = 0; c < nf; ++c) {
            if (cid[c] == cs) {
                found = c;
                break;
            }
        }
        if (found < 0) {
            return false;
        }
        scan_i[i] = found;
        scan_td[i] = (tdta >> 4) & 0x0F;
    }
    const int ss = JpegSrcGet(&src);
    const int se = JpegSrcGet(&src);
    const int ah_al = JpegSrcGet(&src);
    if (ss != 0 || se != 0) {
        ESP_LOGW(TAG, "progressive first scan Ss=%d Se=%d, skip DC preview", ss, se);
        return false;
    }
    const int al = ah_al & 0x0F;
    (void)sos_len;

    int hmax = 1;
    int vmax = 1;
    for (int i = 0; i < nf; ++i) {
        if (ch[i] > hmax) {
            hmax = ch[i];
        }
        if (cv[i] > vmax) {
            vmax = cv[i];
        }
    }
    const int mcu_x = (width + hmax * 8 - 1) / (hmax * 8);
    const int mcu_y = (height + vmax * 8 - 1) / (vmax * 8);

    int16_t* plane[4] = {};
    int pbx[4] = {};
    int pby[4] = {};
    for (int i = 0; i < nf; ++i) {
        pbx[i] = (width * ch[i] + hmax * 8 - 1) / (hmax * 8);
        pby[i] = (height * cv[i] + vmax * 8 - 1) / (vmax * 8);
        if (pbx[i] < 1) {
            pbx[i] = 1;
        }
        if (pby[i] < 1) {
            pby[i] = 1;
        }
        const size_t cells = static_cast<size_t>(pbx[i]) * static_cast<size_t>(pby[i]);
        plane[i] = static_cast<int16_t*>(AllocBig(cells * sizeof(int16_t)));
        if (plane[i] == nullptr) {
            for (int j = 0; j < i; ++j) {
                heap_caps_free(plane[j]);
            }
            return false;
        }
        memset(plane[i], 0, cells * sizeof(int16_t));
    }

    JpegBitIn bits;
    bits.src = &src;
    int last_dc[4] = {};
    int mcu_count = 0;
    bool ok = true;
    for (int my = 0; my < mcu_y && ok; ++my) {
        for (int mx = 0; mx < mcu_x && ok; ++mx) {
            if (restart > 0 && mcu_count > 0 && (mcu_count % restart) == 0) {
                bits.nbits = 0;
                bits.acc = 0;
                memset(last_dc, 0, sizeof(last_dc));
                if (bits.unread_marker == 0) {
                    while (bits.unread_marker == 0) {
                        if (JpegNextByte(&bits) == EOF) {
                            break;
                        }
                    }
                }
                if (bits.unread_marker >= 0xD0 && bits.unread_marker <= 0xD7) {
                    bits.unread_marker = 0;
                }
            }
            for (int s = 0; s < ns && ok; ++s) {
                const int ci = scan_i[s];
                const int td = scan_td[s];
                if (td < 0 || td > 3 || !dc_huff[td].ok) {
                    ok = false;
                    break;
                }
                for (int iy = 0; iy < cv[ci] && ok; ++iy) {
                    for (int ix = 0; ix < ch[ci]; ++ix) {
                        const int bx = mx * ch[ci] + ix;
                        const int by = my * cv[ci] + iy;
                        const int ssss = JpegDecodeHuff(&bits, &dc_huff[td]);
                        if (ssss < 0) {
                            ok = false;
                            break;
                        }
                        last_dc[ci] += JpegReceiveExtend(&bits, ssss);
                        if (bx < pbx[ci] && by < pby[ci]) {
                            plane[ci][by * pbx[ci] + bx] =
                                static_cast<int16_t>(last_dc[ci] << al);
                        }
                    }
                }
            }
            ++mcu_count;
        }
    }

    const int dw = (width + 7) / 8;
    const int dh = (height + 7) / 8;
    uint8_t* rgb = nullptr;
    if (ok) {
        rgb = static_cast<uint8_t*>(AllocBig(static_cast<size_t>(dw) * dh * 2));
        ok = rgb != nullptr;
    }
    if (ok) {
        const int y_i = 0;
        int cb_i = (nf > 1) ? 1 : 0;
        int cr_i = (nf > 2) ? 2 : cb_i;
        for (int i = 0; i < nf; ++i) {
            if (cid[i] == 2) {
                cb_i = i;
            }
            if (cid[i] == 3) {
                cr_i = i;
            }
        }
        for (int y = 0; y < dh; ++y) {
            for (int x = 0; x < dw; ++x) {
                const int yx = (x * pbx[y_i]) / dw;
                const int yy = (y * pby[y_i]) / dh;
                int yv = plane[y_i][yy * pbx[y_i] + yx] * qt_dc[ctq[y_i] & 3];
                yv = JpegClamp8((yv / 8) + 128);
                int cb = 128;
                int cr = 128;
                if (nf >= 3) {
                    const int cx = (x * pbx[cb_i]) / dw;
                    const int cy = (y * pby[cb_i]) / dh;
                    const int rx = (x * pbx[cr_i]) / dw;
                    const int ry = (y * pby[cr_i]) / dh;
                    cb = JpegClamp8(
                        (plane[cb_i][cy * pbx[cb_i] + cx] * qt_dc[ctq[cb_i] & 3] / 8) + 128);
                    cr = JpegClamp8(
                        (plane[cr_i][ry * pbx[cr_i] + rx] * qt_dc[ctq[cr_i] & 3] / 8) + 128);
                }
                const int cbr = cb - 128;
                const int crr = cr - 128;
                const int r = JpegClamp8(yv + crr + (crr >> 2) + (crr >> 3) + (crr >> 5));
                const int g = JpegClamp8(yv - ((cbr >> 2) + (cbr >> 4) + (cbr >> 5)) -
                                         ((crr >> 1) + (crr >> 3) + (crr >> 4) + (crr >> 5)));
                const int b = JpegClamp8(yv + cbr + (cbr >> 1) + (cbr >> 2) + (cbr >> 6));
                const uint16_t pix = PackRgb565(static_cast<uint32_t>(r), static_cast<uint32_t>(g),
                                                static_cast<uint32_t>(b));
                uint8_t* d = rgb + (static_cast<size_t>(y) * dw + x) * 2;
                d[0] = static_cast<uint8_t>(pix & 0xFF);
                d[1] = static_cast<uint8_t>(pix >> 8);
            }
        }
    }

    for (int i = 0; i < nf; ++i) {
        heap_caps_free(plane[i]);
    }
    if (!ok) {
        heap_caps_free(rgb);
        return false;
    }
    if (orig_w != nullptr) {
        *orig_w = width;
    }
    if (orig_h != nullptr) {
        *orig_h = height;
    }
    *out = rgb;
    *out_w = dw;
    *out_h = dh;
    *out_stride_px = dw;
    ESP_LOGI(TAG, "progressive DC preview %dx%d -> %dx%d", width, height, dw, dh);
    return true;
}

#if defined(ESP_PLATFORM)

struct JpegErr {
    jpeg_error_mgr pub;
    jmp_buf jmp;
};

extern "C" void JpegErrorExit(j_common_ptr cinfo) {
    auto* err = reinterpret_cast<JpegErr*>(cinfo->err);
    char msg[JMSG_LENGTH_MAX];
    (*cinfo->err->format_message)(cinfo, msg);
    ESP_LOGW(TAG, "libjpeg: %s", msg);
    longjmp(err->jmp, 1);
}

extern "C" void JpegSilence(j_common_ptr) {}

bool DecodeJpegSoftIo(const uint8_t* data, size_t len, FILE* fp, int min_w, int min_h,
                      bool cover, uint8_t** out, int* out_w, int* out_h, int* out_stride_px,
                      int* orig_w, int* orig_h) {
    *out = nullptr;
    if ((fp == nullptr && (data == nullptr || len < 4)) || min_w < 1 || min_h < 1) {
        return false;
    }

    jpeg_decompress_struct cinfo;
    JpegErr jerr;
    memset(&cinfo, 0, sizeof(cinfo));
    volatile uint8_t* buf_v = nullptr;
    volatile uint8_t* row_v = nullptr;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = JpegErrorExit;
    jerr.pub.output_message = JpegSilence;
    if (setjmp(jerr.jmp)) {
        heap_caps_free(const_cast<uint8_t*>(buf_v));
        heap_caps_free(const_cast<uint8_t*>(row_v));
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    jpeg_create_decompress(&cinfo);
    if (cinfo.mem != nullptr) {
        cinfo.mem->max_memory_to_use = static_cast<long>(JpegSoftBudget());
    }
    if (fp != nullptr) {
        jpeg_stdio_src(&cinfo, fp);
    } else {
        jpeg_mem_src(&cinfo, data, static_cast<unsigned long>(len));
    }
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    const int sw = static_cast<int>(cinfo.image_width);
    const int sh = static_cast<int>(cinfo.image_height);
    if (sw < 1 || sh < 1 || sw > kPngMaxSide || sh > kPngMaxSide) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    if (orig_w != nullptr) {
        *orig_w = sw;
    }
    if (orig_h != nullptr) {
        *orig_h = sh;
    }

    const float fx = static_cast<float>(min_w) / static_cast<float>(sw);
    const float fy = static_cast<float>(min_h) / static_cast<float>(sh);
    const float need = cover ? (fx > fy ? fx : fy) : (fx < fy ? fx : fy);
    int div = 1;
    if (cinfo.progressive_mode) {
        div = 8;
        if (need > 0.5f) {
            div = 2;
        } else if (need > 0.25f) {
            div = 4;
        }
    } else if (need <= 0.125f) {
        div = 8;
    } else if (need <= 0.25f) {
        div = 4;
    } else if (need <= 0.5f) {
        div = 2;
    }

    cinfo.scale_num = 1;
    cinfo.scale_denom = div;
    cinfo.out_color_space = JCS_RGB;
    cinfo.dct_method = JDCT_IFAST;
    cinfo.do_fancy_upsampling = FALSE;
    cinfo.do_block_smoothing = FALSE;
    jpeg_calc_output_dimensions(&cinfo);

    const int dw = static_cast<int>(cinfo.output_width);
    const int dh = static_cast<int>(cinfo.output_height);
    int extra = 1;
    while (extra < 64) {
        const int nw = dw / extra;
        const int nh = dh / extra;
        if (nw < 1 || nh < 1) {
            break;
        }
        if (static_cast<size_t>(nw) * static_cast<size_t>(nh) * 2u <= kPngDecodeMaxBytes) {
            break;
        }
        extra *= 2;
    }
    const int ow = dw / extra;
    const int oh = dh / extra;
    if (ow < 1 || oh < 1) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    auto* buf = static_cast<uint8_t*>(AllocBig(static_cast<size_t>(ow) * oh * 2));
    auto* row = static_cast<uint8_t*>(AllocBig(static_cast<size_t>(dw) * 3));
    buf_v = buf;
    row_v = row;
    if (buf == nullptr || row == nullptr) {
        heap_caps_free(buf);
        heap_caps_free(row);
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    jpeg_start_decompress(&cinfo);
    JSAMPROW rows[1] = {row};
    int dy = 0;
    while (cinfo.output_scanline < cinfo.output_height) {
        jpeg_read_scanlines(&cinfo, rows, 1);
        const int y = static_cast<int>(cinfo.output_scanline) - 1;
        if ((y % extra) != 0 || dy >= oh) {
            continue;
        }
        uint8_t* dst = buf + static_cast<size_t>(dy) * ow * 2;
        for (int x = 0; x < ow; ++x) {
            const uint8_t* p = row + static_cast<size_t>(x * extra) * 3;
            const uint16_t v = PackRgb565(p[0], p[1], p[2]);
            dst[x * 2] = static_cast<uint8_t>(v & 0xFF);
            dst[x * 2 + 1] = static_cast<uint8_t>(v >> 8);
        }
        ++dy;
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    heap_caps_free(row);

    *out = buf;
    *out_w = ow;
    *out_h = oh;
    *out_stride_px = ow;
    return true;
}

bool DecodeJpegSoftFromFile(const char* path, int min_w, int min_h, bool cover, uint8_t** out,
                            int* out_w, int* out_h, int* out_stride_px, int* orig_w,
                            int* orig_h) {
    FILE* fp = fopen(path, "rb");
    if (fp == nullptr) {
        return false;
    }
    const bool ok = DecodeJpegSoftIo(nullptr, 0, fp, min_w, min_h, cover, out, out_w, out_h,
                                     out_stride_px, orig_w, orig_h);
    if (ok) {
        fclose(fp);
        return true;
    }
    ESP_LOGW(TAG, "libjpeg progressive failed, try DC preview");
    const bool dc = DecodeJpegDcPreview(nullptr, 0, fp, out, out_w, out_h, out_stride_px, orig_w,
                                        orig_h);
    fclose(fp);
    return dc;
}

#else

bool DecodeJpegSoftIo(const uint8_t* data, size_t len, FILE* fp, int min_w, int min_h,
                      bool cover, uint8_t** out, int* out_w, int* out_h, int* out_stride_px,
                      int* orig_w, int* orig_h) {
    (void)data;
    (void)len;
    (void)fp;
    (void)min_w;
    (void)min_h;
    (void)cover;
    (void)orig_w;
    (void)orig_h;
    *out = nullptr;
    *out_w = 0;
    *out_h = 0;
    *out_stride_px = 0;
    return false;
}

bool DecodeJpegSoftFromFile(const char* path, int min_w, int min_h, bool cover, uint8_t** out,
                            int* out_w, int* out_h, int* out_stride_px, int* orig_w,
                            int* orig_h) {
    (void)path;
    return DecodeJpegSoftIo(nullptr, 0, nullptr, min_w, min_h, cover, out, out_w, out_h,
                            out_stride_px, orig_w, orig_h);
}

#endif

void RgbaPixelTo565(const uint8_t* p, uint8_t* dst) {
    const uint32_t a = p[3];
    const uint32_t r = p[0] * a / 255;
    const uint32_t g = p[1] * a / 255;
    const uint32_t b = p[2] * a / 255;
    const uint16_t v = PackRgb565(r, g, b);
    dst[0] = static_cast<uint8_t>(v & 0xFF);
    dst[1] = static_cast<uint8_t>(v >> 8);
}

int PngDownsampleDiv(int sw, int sh, int min_w, int min_h, bool cover) {
    int div = 1;
    for (;;) {
        const int nd = div * 2;
        const int nw = sw / nd;
        const int nh = sh / nd;
        if (nw < 1 || nh < 1) {
            break;
        }
        const size_t bytes = static_cast<size_t>(nw) * static_cast<size_t>(nh) * 2u;
        if (bytes > kPngDecodeMaxBytes) {
            div = nd;
            continue;
        }
        if (cover) {
            if (nw < min_w || nh < min_h) {
                break;
            }
        } else if (nw < min_w && nh < min_h) {
            break;
        }
        div = nd;
        if (div >= 64) {
            break;
        }
    }
    return div < 1 ? 1 : div;
}

#if defined(ESP_PLATFORM)
struct PngMemSrc {
    const uint8_t* p;
    size_t left;
};

void PngReadFn(png_structp png, png_bytep out, png_size_t n) {
    auto* s = static_cast<PngMemSrc*>(png_get_io_ptr(png));
    if (s == nullptr || n > s->left) {
        png_error(png, "truncated");
    }
    memcpy(out, s->p, n);
    s->p += n;
    s->left -= n;
}

bool DecodePngScaledLibpng(const uint8_t* data, size_t len, int min_w, int min_h, bool cover,
                           uint8_t** out, int* out_w, int* out_h, int* out_stride_px, int* orig_w,
                           int* orig_h) {
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png != nullptr ? png_create_info_struct(png) : nullptr;
    if (png == nullptr || info == nullptr) {
        png_destroy_read_struct(png ? &png : nullptr, info ? &info : nullptr, nullptr);
        return false;
    }

    volatile uint8_t* buf_v = nullptr;
    volatile uint8_t* row_v = nullptr;
    if (setjmp(png_jmpbuf(png))) {
        heap_caps_free(const_cast<uint8_t*>(buf_v));
        heap_caps_free(const_cast<uint8_t*>(row_v));
        png_destroy_read_struct(&png, &info, nullptr);
        return false;
    }

    PngMemSrc src = {data, len};
    png_set_read_fn(png, &src, PngReadFn);
    png_read_info(png, info);

    const png_uint_32 sw = png_get_image_width(png, info);
    const png_uint_32 sh = png_get_image_height(png, info);
    if (sw < 1 || sh < 1 || sw > static_cast<png_uint_32>(kPngMaxSide) ||
        sh > static_cast<png_uint_32>(kPngMaxSide)) {
        png_destroy_read_struct(&png, &info, nullptr);
        return false;
    }
    if (orig_w != nullptr) {
        *orig_w = static_cast<int>(sw);
    }
    if (orig_h != nullptr) {
        *orig_h = static_cast<int>(sh);
    }

    if (png_get_interlace_type(png, info) != PNG_INTERLACE_NONE) {
        png_destroy_read_struct(&png, &info, nullptr);
        return false;
    }

    png_set_expand(png);
    png_set_gray_to_rgb(png);
    png_set_strip_16(png);
    png_set_packing(png);
    png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    png_read_update_info(png, info);
    if (png_get_channels(png, info) != 4) {
        png_destroy_read_struct(&png, &info, nullptr);
        return false;
    }

    const int div = PngDownsampleDiv(static_cast<int>(sw), static_cast<int>(sh), min_w, min_h,
                                     cover);
    const int dw = static_cast<int>(sw) / div;
    const int dh = static_cast<int>(sh) / div;
    if (dw < 1 || dh < 1) {
        png_destroy_read_struct(&png, &info, nullptr);
        return false;
    }

    auto* buf = static_cast<uint8_t*>(AllocBig(static_cast<size_t>(dw) * dh * 2));
    auto* row = static_cast<uint8_t*>(AllocBig(static_cast<size_t>(sw) * 4));
    buf_v = buf;
    row_v = row;
    if (buf == nullptr || row == nullptr) {
        png_destroy_read_struct(&png, &info, nullptr);
        heap_caps_free(buf);
        heap_caps_free(row);
        return false;
    }

    for (png_uint_32 y = 0; y < sh; ++y) {
        png_read_row(png, row, nullptr);
        if ((y % static_cast<png_uint_32>(div)) != 0) {
            continue;
        }
        const int dy = static_cast<int>(y / static_cast<png_uint_32>(div));
        if (dy >= dh) {
            continue;
        }
        uint8_t* dst = buf + static_cast<size_t>(dy) * dw * 2;
        for (int x = 0; x < dw; ++x) {
            RgbaPixelTo565(row + static_cast<size_t>(x * div) * 4, dst + x * 2);
        }
    }
    png_read_end(png, nullptr);
    png_destroy_read_struct(&png, &info, nullptr);
    heap_caps_free(row);

    *out = buf;
    *out_w = dw;
    *out_h = dh;
    *out_stride_px = dw;
    return true;
}
#else
bool DecodePngScaledLibpng(const uint8_t* data, size_t len, int min_w, int min_h, bool cover,
                           uint8_t** out, int* out_w, int* out_h, int* out_stride_px, int* orig_w,
                           int* orig_h) {
    unsigned char* rgba = nullptr;
    unsigned sw = 0;
    unsigned sh = 0;
    if (lodepng_decode32(&rgba, &sw, &sh, data, len) != 0 || rgba == nullptr || sw < 1 ||
        sh < 1) {
        lv_free(rgba);
        return false;
    }
    if (orig_w != nullptr) {
        *orig_w = static_cast<int>(sw);
    }
    if (orig_h != nullptr) {
        *orig_h = static_cast<int>(sh);
    }
    const int div =
        PngDownsampleDiv(static_cast<int>(sw), static_cast<int>(sh), min_w, min_h, cover);
    const int dw = static_cast<int>(sw) / div;
    const int dh = static_cast<int>(sh) / div;
    auto* buf = static_cast<uint8_t*>(AllocBig(static_cast<size_t>(dw) * dh * 2));
    if (buf == nullptr) {
        lv_free(rgba);
        return false;
    }
    for (int y = 0; y < dh; ++y) {
        const unsigned char* src =
            rgba + static_cast<size_t>(y * div) * sw * 4;
        uint8_t* dst = buf + static_cast<size_t>(y) * dw * 2;
        for (int x = 0; x < dw; ++x) {
            RgbaPixelTo565(src + static_cast<size_t>(x * div) * 4, dst + x * 2);
        }
    }
    lv_free(rgba);
    *out = buf;
    *out_w = dw;
    *out_h = dh;
    *out_stride_px = dw;
    return true;
}
#endif

bool DecodePngScaled(const uint8_t* data, size_t len, int min_w, int min_h, bool cover,
                     uint8_t** out, int* out_w, int* out_h, int* out_stride_px, int* orig_w,
                     int* orig_h) {
    *out = nullptr;
    if (data == nullptr || len < 24 || memcmp(data, "\x89PNG", 4) != 0) {
        return false;
    }
    return DecodePngScaledLibpng(data, len, min_w, min_h, cover, out, out_w, out_h,
                                 out_stride_px, orig_w, orig_h);
}

// RGB565 盒式降采样：把 src 的 (sx,sy,cw,ch) 区域压成 dw×dh。
void BoxScaleRgb565(const uint8_t* src, int src_stride, int sx, int sy, int cw, int ch,
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
            uint32_t r = 0;
            uint32_t g = 0;
            uint32_t b = 0;
            uint32_t n = 0;
            for (int yy = y0; yy < y1; ++yy) {
                const uint8_t* row = src + static_cast<size_t>(yy) * src_stride;
                for (int xx = x0; xx < x1; ++xx) {
                    const uint16_t v =
                        static_cast<uint16_t>(row[xx * 2] | (row[xx * 2 + 1] << 8));
                    r += (v >> 11) & 0x1F;
                    g += (v >> 5) & 0x3F;
                    b += v & 0x1F;
                    ++n;
                }
            }
            const uint16_t o = static_cast<uint16_t>(((r / n) << 11) | ((g / n) << 5) | (b / n));
            uint8_t* d = dst + (static_cast<size_t>(y) * dw + x) * 2;
            d[0] = static_cast<uint8_t>(o & 0xFF);
            d[1] = static_cast<uint8_t>(o >> 8);
        }
    }
}

// 圆屏上要整张图都看得见，就得让图的对角线不超过屏幕直径。
void FitInCircle(int w, int h, int* out_w, int* out_h) {
    if (w <= 0 || h <= 0) {
        *out_w = kViewSafeBox;
        *out_h = kViewSafeBox;
        return;
    }
    const float diag =
        sqrtf(static_cast<float>(w) * static_cast<float>(w) +
              static_cast<float>(h) * static_cast<float>(h));
    if (diag <= static_cast<float>(kViewDiag)) {
        *out_w = w;  // 本来就小，不放大
        *out_h = h;
        return;
    }
    const float f = static_cast<float>(kViewDiag) / diag;
    const int tw = static_cast<int>(static_cast<float>(w) * f);
    const int th = static_cast<int>(static_cast<float>(h) * f);
    *out_w = tw < 1 ? 1 : tw;
    *out_h = th < 1 ? 1 : th;
}

void FillImageDsc(lv_image_dsc_t* dsc, const uint8_t* data, int w, int h) {
    memset(dsc, 0, sizeof(*dsc));
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.w = static_cast<uint32_t>(w);
    dsc->header.h = static_cast<uint32_t>(h);
    dsc->header.stride = static_cast<uint32_t>(w * 2);
    dsc->data_size = static_cast<uint32_t>(w) * h * 2;
    dsc->data = data;
}

// ---------------------------------------------------------------------------
// worker：出缩略图 + 出看图页大图
// ---------------------------------------------------------------------------

Photo* ItemAt(const PhotoListPtr& list, int index) {
    if (!list || index < 0 || index >= static_cast<int>(list->items.size())) {
        return nullptr;
    }
    return list->items[static_cast<size_t>(index)];
}

// 下面两个只给 UI 线程用：s_ui_list 也只有 UI 线程改，所以拿到的 Photo* 稳定。
Photo* PhotoAt(int index) {
    return ItemAt(s_ui_list, index);
}

int PhotoCount() {
    return s_ui_list ? static_cast<int>(s_ui_list->items.size()) : 0;
}

PhotoListPtr PublishedPhotos() {
    std::lock_guard<std::mutex> lock(s_photos_mutex);
    return s_photos;
}

// 挑离视口最近、还没缩略图的一张。
int PickThumbTarget(const PhotoListPtr& list) {
    if (s_grid_scrolling.load(std::memory_order_relaxed)) {
        return -1;
    }
    std::lock_guard<std::mutex> tlock(s_thumb_mutex);
    const int center = s_view_center.load(std::memory_order_relaxed);
    const int count = static_cast<int>(list->items.size());
    int best = -1;
    int best_dist = 0;
    for (int i = 0; i < count; ++i) {
        Photo* p = list->items[static_cast<size_t>(i)];
        if (p->thumb != nullptr || p->thumb_skip || p->generating) {
            continue;
        }
        const int dist = i > center ? i - center : center - i;
        if (best < 0 || dist < best_dist) {
            best = i;
            best_dist = dist;
        }
    }
    if (best >= 0) {
        list->items[static_cast<size_t>(best)]->generating = true;
    }
    return best;
}

// 解码 → 居中裁成正方形 → 压到 kThumb。dst 要有 kThumbBytes。
bool MakeThumb(const uint8_t* data, size_t len, bool jpeg, uint8_t* dst, int* orig_w,
               int* orig_h) {
    uint8_t* dec = nullptr;
    int dw = 0;
    int dh = 0;
    int stride_px = 0;
    const bool ok =
        jpeg ? DecodeJpegScaled(data, len, kThumb, kThumb, true, &dec, &dw, &dh, &stride_px,
                                orig_w, orig_h)
             : DecodePngScaled(data, len, kThumb, kThumb, true, &dec, &dw, &dh, &stride_px,
                               orig_w, orig_h);
    if (!ok) {
        return false;
    }
    const int side = dw < dh ? dw : dh;
    BoxScaleRgb565(dec, stride_px * 2, (dw - side) / 2, (dh - side) / 2, side, side, dst,
                   kThumb, kThumb);
    heap_caps_free(dec);
    return true;
}

// 三条路径，由快到慢：落盘缓存 → EXIF 内嵌小图 → 整张图降采样。
void GenerateThumb(const PhotoListPtr& list, int index, int gen) {
    Photo* p = ItemAt(list, index);
    if (p == nullptr) {
        return;
    }
    auto* thumb = static_cast<uint8_t*>(AllocBig(kThumbBytes));
    if (thumb == nullptr) {
        // 连 9KB 都拿不到就别再重试这张了，否则 worker 会原地打转。
        ESP_LOGW(TAG, "no mem for thumb");
        std::lock_guard<std::mutex> lock(s_thumb_mutex);
        p->generating = false;
        p->thumb_skip = true;
        return;
    }

    int ow = 0;
    int oh = 0;
    bool ok = ThumbCacheRead(p->cache_key, thumb, &ow, &oh);
    const bool from_cache = ok;
    JpegSof sof;
    if (!ok && p->jpeg) {
        uint8_t* head = nullptr;
        size_t head_len = 0;
        if (LoadFilePrefix(p->path, kExifProbeBytes, &head, &head_len)) {
            sof = ParseJpegSof(head, head_len);
            if (sof.w > 0) {
                ow = sof.w;
                oh = sof.h;
            }
            size_t t_off = 0;
            size_t t_len = 0;
            if (FindExifThumb(head, head_len, &t_off, &t_len)) {
                ok = MakeThumb(head + t_off, t_len, true, thumb, nullptr, nullptr);
            } else if (head_len < kExifProbeBytes &&
                       (JpegCanHwDecode(sof) || JpegCanSoftDecode(sof))) {
                ok = MakeThumb(head, head_len, true, thumb, &ow, &oh);
            }
            heap_caps_free(head);
        }
    }
    if (!ok && p->jpeg && JpegCanSoftDecode(sof)) {
        // progressive：从 SD 流式软解，避免整文件和 DCT 系数表同时占 PSRAM。
        uint8_t* dec = nullptr;
        int dw = 0;
        int dh = 0;
        int stride_px = 0;
        if (DecodeJpegSoftFromFile(p->path.c_str(), kThumb, kThumb, true, &dec, &dw, &dh,
                                   &stride_px, &ow, &oh)) {
            const int side = dw < dh ? dw : dh;
            BoxScaleRgb565(dec, stride_px * 2, (dw - side) / 2, (dh - side) / 2, side, side,
                           thumb, kThumb, kThumb);
            heap_caps_free(dec);
            ok = true;
        } else {
            ESP_LOGW(TAG, "%s: progressive decode failed", p->name.c_str());
        }
    } else if (!ok && p->jpeg && sof.std != JpegStd::Invalid && !JpegCanHwDecode(sof)) {
        ESP_LOGW(TAG, "%s: %s JPEG nf=%d, skip decode", p->name.c_str(), JpegStdName(sof.std),
                 sof.nf);
    } else if (!ok) {
        uint8_t* file = nullptr;
        size_t file_len = 0;
        if (LoadFile(p->path, &file, &file_len)) {
            if (p->jpeg && sof.std == JpegStd::Invalid) {
                sof = ParseJpegSof(file, file_len);
                if (sof.w > 0) {
                    ow = sof.w;
                    oh = sof.h;
                }
            }
            if (p->jpeg && sof.std != JpegStd::Invalid && !JpegCanHwDecode(sof) &&
                !JpegCanSoftDecode(sof)) {
                ESP_LOGW(TAG, "%s: %s JPEG nf=%d, skip decode", p->name.c_str(),
                         JpegStdName(sof.std), sof.nf);
            } else {
                ok = MakeThumb(file, file_len, p->jpeg, thumb, &ow, &oh);
            }
            heap_caps_free(file);
        }
    }
    if (ok && !from_cache) {
        ThumbCacheWrite(p->cache_key, thumb, ow, oh);
    }

    std::lock_guard<std::mutex> lock(s_thumb_mutex);
    p->generating = false;
    if (ow > 0) {
        p->src_w = ow;
        p->src_h = oh;
    }
    if (!ok || gen != s_worker_gen.load(std::memory_order_relaxed)) {
        p->thumb_skip = !ok;
        heap_caps_free(thumb);
        return;
    }
    p->thumb = thumb;
    p->thumb_ready = true;
}

void CommitViewPixels(ViewImage* result, uint8_t* dec, int dw, int dh, int stride_px, int tw,
                      int th) {
    if (dec == nullptr || tw < 1 || th < 1) {
        heap_caps_free(dec);
        return;
    }
    if (dw == tw && dh == th && stride_px == dw) {
        result->buf = dec;
        result->w = dw;
        result->h = dh;
        return;
    }
    auto* fit = static_cast<uint8_t*>(AllocBig(static_cast<size_t>(tw) * th * 2));
    if (fit != nullptr) {
        BoxScaleRgb565(dec, stride_px * 2, 0, 0, dw, dh, fit, tw, th);
        result->buf = fit;
        result->w = tw;
        result->h = th;
    }
    heap_caps_free(dec);
}

void DecodeForView(const PhotoListPtr& list, int index, int gen) {
    Photo* p = ItemAt(list, index);
    ViewImage result;
    result.index = index;
    if (p != nullptr) {
        int sw = p->src_w;
        int sh = p->src_h;
        JpegSof sof;
        if (p->jpeg) {
            uint8_t* head = nullptr;
            size_t head_len = 0;
            if (LoadFilePrefix(p->path, kExifProbeBytes, &head, &head_len)) {
                sof = ParseJpegSof(head, head_len);
                heap_caps_free(head);
            }
            if (sof.w > 0) {
                sw = sof.w;
                sh = sof.h;
            }
        } else if (sw <= 0 || sh <= 0) {
            ReadPngSize(p->path, &sw, &sh);
        }
        if (sw > 0 && sh > 0) {
            std::lock_guard<std::mutex> lock(s_thumb_mutex);
            p->src_w = sw;
            p->src_h = sh;
        }
        int tw = 0;
        int th = 0;
        FitInCircle(sw, sh, &tw, &th);
        uint8_t* dec = nullptr;
        int dw = 0;
        int dh = 0;
        int stride_px = 0;
        bool decoded = false;
        if (p->jpeg && JpegCanSoftDecode(sof)) {
            decoded = DecodeJpegSoftFromFile(p->path.c_str(), tw, th, false, &dec, &dw, &dh,
                                            &stride_px, nullptr, nullptr);
            if (!decoded) {
                ESP_LOGW(TAG, "%s: progressive preview failed", p->name.c_str());
            }
        } else if (p->jpeg && !JpegCanHwDecode(sof) && sof.std != JpegStd::Invalid) {
            ESP_LOGW(TAG, "%s: %s JPEG nf=%d, cannot preview", p->name.c_str(),
                     JpegStdName(sof.std), sof.nf);
        } else {
            uint8_t* file = nullptr;
            size_t file_len = 0;
            if (LoadFile(p->path, &file, &file_len)) {
                decoded = p->jpeg ? DecodeJpegScaled(file, file_len, tw, th, false, &dec, &dw,
                                                     &dh, &stride_px, nullptr, nullptr)
                                  : DecodePngScaled(file, file_len, tw, th, false, &dec, &dw, &dh,
                                                    &stride_px, nullptr, nullptr);
                heap_caps_free(file);
            }
        }
        if (decoded) {
            CommitViewPixels(&result, dec, dw, dh, stride_px, tw, th);
        }
    }

    std::lock_guard<std::mutex> lock(s_view_mutex);
    if (gen != s_worker_gen.load(std::memory_order_relaxed)) {
        if (result.buf != nullptr) {
            heap_caps_free(result.buf);
        }
        return;
    }
    if (s_view_pending.buf != nullptr) {
        heap_caps_free(s_view_pending.buf);
    }
    s_view_pending = result;
}

void WorkerTask(void* arg) {
    const int gen = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    while (gen == s_worker_gen.load(std::memory_order_relaxed)) {
        // 每轮取一次快照：换扫描结果时手里这份仍然有效，不用和 UI 线程握手。
        PhotoListPtr list = PublishedPhotos();
        const int req = s_view_req.exchange(-1, std::memory_order_relaxed);
        if (req >= 0) {
            DecodeForView(list, req, gen);
            continue;
        }
        if (!list) {
            vTaskDelay(pdMS_TO_TICKS(80));
            continue;
        }
        const int target = PickThumbTarget(list);
        if (target < 0) {
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }
        GenerateThumb(list, target, gen);
        // 让出一点时间给 LVGL，缩略图慢点出来也比界面卡住好。
        vTaskDelay(pdMS_TO_TICKS(5));
    }
#if defined(ESP_PLATFORM)
    vTaskDeleteWithCaps(nullptr);
#else
    vTaskDelete(nullptr);
#endif
}

void StartWorker() {
    const int gen = s_worker_gen.fetch_add(1, std::memory_order_relaxed) + 1;
    s_view_req.store(-1, std::memory_order_relaxed);
    // 解码钉在 core 0，LVGL 固定在 core 1，两边不抢核。libjpeg progressive 栈吃得多。
    if (!SpawnAlbumTask(WorkerTask, "album_worker", 10240,
                        reinterpret_cast<void*>(static_cast<intptr_t>(gen)), 3, 0)) {
        ESP_LOGE(TAG, "worker task create failed");
        s_need_worker.store(true, std::memory_order_relaxed);
    } else {
        s_need_worker.store(false, std::memory_order_relaxed);
    }
}

// 只是换代号，不等它退出：老 worker 手里的照片快照会保着它用到的内存。
void StopWorker() {
    s_need_worker.store(false, std::memory_order_relaxed);
    s_worker_gen.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(s_view_mutex);
    if (s_view_pending.buf != nullptr) {
        heap_caps_free(s_view_pending.buf);
        s_view_pending = ViewImage{};
    }
}

// ---------------------------------------------------------------------------
// UI 构件
// ---------------------------------------------------------------------------

lv_obj_t* MakeLabel(lv_obj_t* parent, const char* text, uint32_t color, int32_t width) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    if (width > 0) {
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, width);
    }
    screen_make_input_passive(lbl);
    return lbl;
}

lv_obj_t* MakeBackButton(lv_obj_t* parent, lv_event_cb_t cb) {
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, kBackBtnSize, kBackBtnSize);
    // 磨砂圆底 + 淡描边：深色网格页能看出触控靶，看图页浅色图上也压得住。
    lv_obj_set_style_bg_color(btn, lv_color_hex(kColorCell), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xFFFFFF), Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_bg_opa(btn, LV_OPA_30, Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_opa(btn, LV_OPA_30, LV_PART_MAIN);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, kBackBtnX, kBackBtnY);
    lv_obj_set_ext_click_area(btn, 12);
    screen_swipe_back_ignore(btn, true);

    lv_obj_t* icon = lv_image_create(btn);
    lv_image_set_src(icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(icon);

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    return btn;
}

void GoHome() {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* home = HomeScreen::Create();
    lv_screen_load(home);
    if (old_scr != nullptr && old_scr != home) {
        lv_obj_delete_async(old_scr);
    }
}

bool ViewerOpen() {
    return s_ui.view_layer != nullptr && !lv_obj_has_flag(s_ui.view_layer, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------------------
// 看图页
// ---------------------------------------------------------------------------

// 非 JPEG 交给 LVGL 的 S: 盘自己解码，这里存的是它当前用的路径。
char s_view_lv_path[280];

void ReleaseViewCurrent() {
    if (s_ui.view_img != nullptr) {
        lv_image_set_src(s_ui.view_img, nullptr);
    }
    if (s_view_lv_path[0] != '\0') {
        // 上一张是 LVGL 解的 PNG，解出来的位图还压在图片缓存里，顺手丢掉。
        lv_image_cache_drop(s_view_lv_path);
        s_view_lv_path[0] = '\0';
    }
    if (s_view_current.buf != nullptr) {
        // 描述符地址是复用的，缓存里那条旧记录还指着即将释放的内存，先丢掉。
        lv_image_cache_drop(&s_view_dsc);
        heap_caps_free(s_view_current.buf);
    }
    s_view_current = ViewImage{};
}

void ApplyChromeVisibility() {
    if (s_ui.view_layer == nullptr) {
        return;
    }
    lv_obj_t* items[] = {s_ui.view_counter, s_ui.view_info, s_ui.view_back};
    for (lv_obj_t* obj : items) {
        if (obj == nullptr) {
            continue;
        }
        if (s_chrome_visible) {
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void UpdateViewTexts() {
    Photo* p = PhotoAt(s_view_index);
    if (p == nullptr) {
        return;
    }
    char buf[48];
    snprintf(buf, sizeof(buf), "%d / %d", s_view_index + 1, PhotoCount());
    if (s_ui.view_counter != nullptr) {
        lv_label_set_text(s_ui.view_counter, buf);
    }
    if (s_ui.view_info != nullptr) {
        char tail[24];
        if (p->src_w > 0 && p->src_h > 0) {
            snprintf(tail, sizeof(tail), "%d×%d", p->src_w, p->src_h);
        } else {
            snprintf(tail, sizeof(tail), "%u KB", static_cast<unsigned>(p->size_kb));
        }
        const std::string title = StripExt(p->name);
        lv_label_set_text_fmt(s_ui.view_info, "%s · %s", title.c_str(), tail);
    }
}

void SetViewHint(const char* text) {
    if (s_ui.view_hint == nullptr) {
        return;
    }
    if (text == nullptr) {
        lv_obj_add_flag(s_ui.view_hint, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(s_ui.view_hint, text);
    lv_obj_remove_flag(s_ui.view_hint, LV_OBJ_FLAG_HIDDEN);
}

void ViewSetDrag(int32_t x) {
    s_view_drag = x;
    if (s_ui.view_img != nullptr) {
        lv_obj_set_style_translate_x(s_ui.view_img, x, LV_PART_MAIN);
    }
}

void ViewAnimExec(void* /*obj*/, int32_t v) {
    ViewSetDrag(v);
}

void ViewCancelSlide() {
    if (s_ui.view_img != nullptr) {
        lv_anim_delete(s_ui.view_img, ViewAnimExec);
    }
    s_view_sliding = false;
    ViewSetDrag(0);
}

void ShowPhoto(int index);

void OnViewSlideDone(lv_anim_t* a) {
    const int next = static_cast<int>(reinterpret_cast<intptr_t>(lv_anim_get_user_data(a)));
    s_view_sliding = false;
    ViewSetDrag(0);
    if (next >= 0) {
        ShowPhoto(next);
    }
}

void ViewAnimateTo(int32_t from, int32_t to, int next_index, uint32_t ms) {
    if (s_ui.view_img == nullptr) {
        return;
    }
    lv_anim_delete(s_ui.view_img, ViewAnimExec);
    s_view_sliding = true;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_ui.view_img);
    lv_anim_set_exec_cb(&a, ViewAnimExec);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_user_data(&a, reinterpret_cast<void*>(static_cast<intptr_t>(next_index)));
    lv_anim_set_completed_cb(&a, OnViewSlideDone);
    lv_anim_start(&a);
}

void ShowPhoto(int index) {
    const int count = PhotoCount();
    if (count == 0) {
        return;
    }
    if (index < 0) {
        index = count - 1;
    } else if (index >= count) {
        index = 0;
    }
    Photo* p = PhotoAt(index);
    if (p == nullptr) {
        return;
    }
    s_view_index = index;
    s_view_center.store(index, std::memory_order_relaxed);
    ViewSetDrag(0);
    ReleaseViewCurrent();
    UpdateViewTexts();
    SetViewHint(I18n::T("解码中…"));
    s_view_req.store(index, std::memory_order_relaxed);
}

void CloseViewer() {
    if (s_ui.view_layer == nullptr) {
        return;
    }
    ViewCancelSlide();
    s_view_req.store(-1, std::memory_order_relaxed);
    ReleaseViewCurrent();
    lv_obj_add_flag(s_ui.view_layer, LV_OBJ_FLAG_HIDDEN);
    s_view_index = -1;
}

void OpenViewer(int index) {
    if (s_ui.view_layer == nullptr) {
        return;
    }
    s_chrome_visible = true;
    ApplyChromeVisibility();
    ViewCancelSlide();
    lv_obj_remove_flag(s_ui.view_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ui.view_layer);
    ShowPhoto(index);
}

// 屏幕加载时 LVGL 会给所有子对象打上 EVENT_BUBBLE，返回键的按下也会冒泡到这层，
// 那种事件不该当成看图手势。
bool EventFromSelf(lv_event_t* e) {
    return lv_event_get_target_obj(e) == lv_event_get_current_target_obj(e);
}

void OnViewPressed(lv_event_t* e) {
    if (!EventFromSelf(e) || s_view_sliding) {
        return;
    }
    lv_indev_t* indev = lv_indev_active();
    if (indev == nullptr) {
        return;
    }
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    s_press_x = pt.x;
    s_press_y = pt.y;
    s_press_tick = lv_tick_get();
    lv_anim_delete(s_ui.view_img, ViewAnimExec);
}

void OnViewPressing(lv_event_t* e) {
    if (!EventFromSelf(e) || s_view_sliding) {
        return;
    }
    lv_indev_t* indev = lv_indev_active();
    if (indev == nullptr) {
        return;
    }
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    ViewSetDrag(pt.x - s_press_x);
}

void OnViewReleased(lv_event_t* e) {
    if (!EventFromSelf(e) || s_view_sliding) {
        return;
    }
    lv_indev_t* indev = lv_indev_active();
    if (indev == nullptr) {
        return;
    }
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    const int32_t dx = pt.x - s_press_x;
    const int32_t dy = pt.y - s_press_y;
    const int32_t adx = dx < 0 ? -dx : dx;
    const int32_t ady = dy < 0 ? -dy : dy;
    const uint32_t dt = lv_tick_elaps(s_press_tick);

    bool swipe = adx >= kSwipeMinPx && ady <= kSwipeMaxDy;
    if (!swipe && adx >= 28 && adx > ady * 2 && dt < 240) {
        swipe = true;
    }

    if (swipe && PhotoCount() > 1) {
        int next = dx > 0 ? s_view_index - 1 : s_view_index + 1;
        if (next < 0) {
            next = PhotoCount() - 1;
        } else if (next >= PhotoCount()) {
            next = 0;
        }
        const int32_t out = dx > 0 ? kPanel : -kPanel;
        s_view_req.store(next, std::memory_order_relaxed);
        ViewAnimateTo(s_view_drag, out, next, 150);
        return;
    }
    if (s_view_drag != 0) {
        ViewAnimateTo(s_view_drag, 0, -1, 140);
        return;
    }
    if (adx <= kTapMaxPx && ady <= kTapMaxPx) {
        s_chrome_visible = !s_chrome_visible;
        ApplyChromeVisibility();
    }
}

void BuildViewLayer(lv_obj_t* parent) {
    lv_obj_t* layer = lv_obj_create(parent);
    lv_obj_remove_style_all(layer);
    lv_obj_add_flag(layer, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(layer, kPanel, kPanel);
    lv_obj_set_pos(layer, 0, 0);
    lv_obj_set_style_bg_color(layer, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(layer, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(layer, LV_OBJ_FLAG_HIDDEN);
    // 看图页自己吃左右滑动（上下张），所以屏级右滑返回在这层要关掉，退出走返回键。
    screen_swipe_back_ignore(layer, true);
    lv_obj_add_event_cb(layer, OnViewPressed, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(layer, OnViewPressing, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(layer, OnViewReleased, LV_EVENT_RELEASED, nullptr);
    s_ui.view_layer = layer;

    s_ui.view_img = lv_image_create(layer);
    // 尺寸每张图单独定（见 FitInCircle），这里只给个安全初值。
    lv_obj_set_size(s_ui.view_img, kViewSafeBox, kViewSafeBox);
    lv_obj_center(s_ui.view_img);
    lv_image_set_inner_align(s_ui.view_img, LV_IMAGE_ALIGN_CONTAIN);
    lv_image_set_antialias(s_ui.view_img, false);
    lv_obj_remove_flag(s_ui.view_img, LV_OBJ_FLAG_CLICKABLE);

    s_ui.view_hint = MakeLabel(layer, "", kColorMuted, kViewTextW);
    lv_obj_center(s_ui.view_hint);
    lv_obj_add_flag(s_ui.view_hint, LV_OBJ_FLAG_HIDDEN);

    s_ui.view_counter = MakeLabel(layer, "", kColorText, 120);
    lv_obj_align(s_ui.view_counter, LV_ALIGN_TOP_MID, 0, kViewCounterY);

    s_ui.view_info = MakeLabel(layer, "", kColorMuted, kViewTextW);
    lv_obj_align(s_ui.view_info, LV_ALIGN_TOP_MID, 0, kViewInfoY);

    s_ui.view_back = MakeBackButton(layer, [](lv_event_t*) { CloseViewer(); });
}

// ---------------------------------------------------------------------------
// 网格页
// ---------------------------------------------------------------------------

void OnCellClicked(lv_event_t* e) {
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    OpenViewer(index);
}

void BindThumbToCell(size_t i, Photo* p) {
    if (i >= s_cells.size() || s_cells[i] == nullptr) {
        return;
    }
    FillImageDsc(&p->thumb_dsc, p->thumb, kThumb, kThumb);
    lv_image_cache_drop(&p->thumb_dsc);
    lv_image_set_src(s_cells[i], &p->thumb_dsc);
}

void RebuildGrid() {
    if (s_ui.grid == nullptr) {
        return;
    }
    lv_obj_clean(s_ui.grid);
    s_cells.clear();
    // 格子全拆了，可以安全换到最新一份扫描结果。
    ReleaseUiList();
    s_ui_list = PublishedPhotos();

    const int count = PhotoCount();
    if (count == 0) {
        return;
    }
    s_cells.resize(static_cast<size_t>(count), nullptr);
    for (int i = 0; i < count; ++i) {
        Photo* p = PhotoAt(i);
        if (p == nullptr) {
            continue;
        }
        lv_obj_t* cell = lv_image_create(s_ui.grid);
        lv_obj_set_size(cell, kThumb, kThumb);
        // 不要 clip_corner：圆角裁切会走 intermediate layer，QSPI 短条带下一滑就卡。
        lv_obj_set_style_bg_color(cell, lv_color_hex(kColorCell), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(cell, lv_color_hex(kColorCellPressed),
                                  Sel(LV_PART_MAIN, LV_STATE_PRESSED));
        lv_image_set_inner_align(cell, LV_IMAGE_ALIGN_CENTER);
        lv_image_set_antialias(cell, false);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        // 格子是扫描完才建的，错过了屏幕加载时那次统一打标，自己补上，
        // 否则在格子上右滑回不了主界面。
        lv_obj_add_flag(cell, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_event_cb(cell, OnCellClicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        s_cells[static_cast<size_t>(i)] = cell;

        std::lock_guard<std::mutex> lock(s_thumb_mutex);
        if (p->thumb != nullptr) {
            BindThumbToCell(static_cast<size_t>(i), p);
            p->thumb_ready = false;
        } else if (p->thumb_skip) {
            // 大到不敢解的图：格子里摆个角标，别只留一块空砖。
            lv_obj_t* tag = MakeLabel(cell, ExtLabel(p->name), kColorMuted, 0);
            lv_obj_center(tag);
        }
    }
}

void RefreshTopLabel() {
    if (s_ui.lbl_top == nullptr) {
        return;
    }
    char buf[48];
    const int count = PhotoCount();
    if (count <= 0) {
        lv_label_set_text(s_ui.lbl_top, I18n::T("相册"));
        return;
    }
    snprintf(buf, sizeof(buf), "%s · %d %s", I18n::T("相册"), count, I18n::T("张"));
    lv_label_set_text(s_ui.lbl_top, buf);
}

void BuildGrid(lv_obj_t* parent) {
    lv_obj_t* grid = lv_obj_create(parent);
    screen_strip_obj_chrome(grid);
    lv_obj_set_size(grid, kGridBoxW, kGridH);
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, kGridTop);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(grid, kGridPad, LV_PART_MAIN);
    lv_obj_set_style_pad_row(grid, kGridGap, LV_PART_MAIN);
    lv_obj_set_style_pad_column(grid, kGridGap, LV_PART_MAIN);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(grid, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_add_flag(grid, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_add_event_cb(grid, [](lv_event_t*) { s_grid_scrolling.store(true); },
                        LV_EVENT_SCROLL_BEGIN, nullptr);
    lv_obj_add_event_cb(grid, [](lv_event_t*) { s_grid_scrolling.store(true); },
                        LV_EVENT_SCROLL_THROW_BEGIN, nullptr);
    lv_obj_add_event_cb(grid, [](lv_event_t*) { s_grid_scrolling.store(false); },
                        LV_EVENT_SCROLL_END, nullptr);
    s_ui.grid = grid;
}

// ---------------------------------------------------------------------------
// 扫描中 / 空态
// ---------------------------------------------------------------------------

void ShowStateLayer(const char* title, const char* sub, bool with_button);

void OnRescanClicked(lv_event_t* /*e*/) {
    // 先摘掉引用旧缩略图的格子，再换代号让旧 worker 退场；旧列表由它自己的快照
    // 兜着，这里不用等谁停下来。
    if (s_ui.grid != nullptr) {
        lv_obj_clean(s_ui.grid);
        s_cells.clear();
    }
    StopWorker();
    StartScan();
    RefreshTopLabel();
    ShowStateLayer(I18n::T("正在扫描 SD 卡"), "", false);
    StartWorker();
}

void BuildStateLayer(lv_obj_t* parent) {
    lv_obj_t* layer = lv_obj_create(parent);
    lv_obj_remove_style_all(layer);
    lv_obj_add_flag(layer, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(layer, kPanel, kPanel);
    lv_obj_set_pos(layer, 0, 0);
    lv_obj_set_style_bg_color(layer, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(layer, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(layer, LV_OBJ_FLAG_HIDDEN);
    s_ui.state_layer = layer;

    s_ui.state_title = MakeLabel(layer, "", kColorText, kViewTextW);
    lv_obj_align(s_ui.state_title, LV_ALIGN_CENTER, 0, -18);

    s_ui.state_sub = MakeLabel(layer, "", kColorMuted, kViewTextW);
    lv_obj_align(s_ui.state_sub, LV_ALIGN_CENTER, 0, 14);

    lv_obj_t* btn = lv_button_create(layer);
    lv_obj_set_size(btn, 108, 34);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 62);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(kColorCell), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(kColorCellPressed),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, OnRescanClicked, LV_EVENT_CLICKED, nullptr);
    s_ui.state_btn = btn;

    lv_obj_t* lbl = MakeLabel(btn, I18n::T("重新扫描"), kColorAccent, 0);
    lv_obj_center(lbl);

    // 这层是整屏覆盖的，会挡住网格页的返回键，自己再放一个。
    MakeBackButton(layer, [](lv_event_t*) { GoHome(); });
}

void ShowStateLayer(const char* title, const char* sub, bool with_button) {
    if (s_ui.state_layer == nullptr) {
        return;
    }
    lv_label_set_text(s_ui.state_title, title);
    lv_label_set_text(s_ui.state_sub, sub);
    if (with_button) {
        lv_obj_remove_flag(s_ui.state_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui.state_btn, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_remove_flag(s_ui.state_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ui.state_layer);
    s_state_shown = true;
}

void HideStateLayer() {
    if (s_ui.state_layer != nullptr) {
        lv_obj_add_flag(s_ui.state_layer, LV_OBJ_FLAG_HIDDEN);
    }
    s_state_shown = false;
}

// ---------------------------------------------------------------------------
// 定时刷新：挂新缩略图、淘汰远处缩略图、接大图
// ---------------------------------------------------------------------------

void ApplyReadyThumbs() {
    if (!s_ui_list) {
        return;
    }
    std::lock_guard<std::mutex> tlock(s_thumb_mutex);
    for (size_t i = 0; i < s_ui_list->items.size(); ++i) {
        Photo* p = s_ui_list->items[i];
        if (!p->thumb_ready || p->thumb == nullptr) {
            continue;
        }
        BindThumbToCell(i, p);
        p->thumb_ready = false;
    }
}

void EvictFarThumbs() {
    if (!s_ui_list) {
        return;
    }
    const int center = s_view_center.load(std::memory_order_relaxed);
    for (int guard = 0; guard < 4; ++guard) {
        int victim = -1;
        int victim_dist = -1;
        size_t live = 0;
        {
            std::lock_guard<std::mutex> tlock(s_thumb_mutex);
            for (size_t i = 0; i < s_ui_list->items.size(); ++i) {
                Photo* p = s_ui_list->items[i];
                if (p->thumb == nullptr) {
                    continue;
                }
                ++live;
                if (p->generating) {
                    continue;
                }
                const int dist = static_cast<int>(i) > center
                                     ? static_cast<int>(i) - center
                                     : center - static_cast<int>(i);
                if (dist > victim_dist) {
                    victim = static_cast<int>(i);
                    victim_dist = dist;
                }
            }
            if (live <= kMaxThumbs || victim < 0) {
                return;
            }
            Photo* p = s_ui_list->items[static_cast<size_t>(victim)];
            uint8_t* buf = p->thumb;
            p->thumb = nullptr;
            p->thumb_ready = false;
            // 先摘掉引用再放内存，绘制线程就是这条 UI 线程，不会踩空。
            if (static_cast<size_t>(victim) < s_cells.size() &&
                s_cells[static_cast<size_t>(victim)] != nullptr) {
                lv_image_set_src(s_cells[static_cast<size_t>(victim)], nullptr);
            }
            lv_image_cache_drop(&p->thumb_dsc);
            heap_caps_free(buf);
        }
    }
}

void ApplyPendingView() {
    ViewImage img;
    {
        std::lock_guard<std::mutex> lock(s_view_mutex);
        if (s_view_pending.index < 0) {
            return;
        }
        img = s_view_pending;
        s_view_pending = ViewImage{};
    }
    if (!ViewerOpen() || img.index != s_view_index) {
        if (img.buf != nullptr) {
            heap_caps_free(img.buf);
        }
        return;
    }
    if (img.buf == nullptr) {
        SetViewHint(I18n::T("这张图解不出来"));
        return;
    }
    ReleaseViewCurrent();
    s_view_current = img;
    lv_image_cache_drop(&s_view_dsc);
    FillImageDsc(&s_view_dsc, img.buf, img.w, img.h);
    // 解码时已经按内切圆算好尺寸了，控件跟着一样大，LVGL 就不用再缩放一次。
    lv_obj_set_size(s_ui.view_img, img.w, img.h);
    lv_obj_center(s_ui.view_img);
    SetViewHint(nullptr);
    lv_image_set_src(s_ui.view_img, &s_view_dsc);

    UpdateViewTexts();  // 解码时才拿到原始尺寸，信息栏跟着刷一次
}

void SyncViewCenterFromScroll() {
    if (s_ui.grid == nullptr || ViewerOpen()) {
        return;
    }
    const int32_t scroll = lv_obj_get_scroll_y(s_ui.grid);
    const int row = scroll > 0 ? scroll / kRowStride : 0;
    // 视口大概三行，取中间那行做“热点”。
    s_view_center.store((row + 1) * kGridCols + 1, std::memory_order_relaxed);
}

void OnTick(lv_timer_t* /*t*/) {
    if (!s_screen_active) {
        return;
    }

    if (s_scanning.load(std::memory_order_relaxed)) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%s %d %s", I18n::T("已找到"),
                 s_scan_found.load(std::memory_order_relaxed), I18n::T("张"));
        if (!s_state_shown) {
            ShowStateLayer(I18n::T("正在扫描 SD 卡"), buf, false);
        } else {
            lv_label_set_text(s_ui.state_sub, buf);
        }
        return;
    }

    if (s_scan_done.exchange(false, std::memory_order_relaxed)) {
        RebuildGrid();
        RefreshTopLabel();
        if (!s_sd_ready.load(std::memory_order_relaxed)) {
            ShowStateLayer(I18n::T("没有读到 SD 卡"), I18n::T("插好卡后重新扫描"), true);
        } else if (PhotoCount() == 0) {
            ShowStateLayer(I18n::T("SD 卡里没有图片"), I18n::T("支持 JPG / PNG"), true);
        } else {
            HideStateLayer();
        }
    }

    SyncViewCenterFromScroll();
    if (!s_grid_scrolling.load(std::memory_order_relaxed)) {
        ApplyReadyThumbs();
        EvictFarThumbs();
    }
    ApplyPendingView();

    if (s_need_worker.load(std::memory_order_relaxed) &&
        !s_scanning.load(std::memory_order_relaxed) && PhotoCount() > 0) {
        StartWorker();
    }
}

void OnSwipeBack() {
    GoHome();
}

void OnScreenUnloaded(lv_event_t* /*e*/) {
    if (s_ui.tick != nullptr) {
        lv_timer_delete(s_ui.tick);
    }
    s_screen_active = false;
    s_grid_scrolling.store(false);
    s_view_sliding = false;
    s_view_index = -1;
    for (lv_indev_t* indev = lv_indev_get_next(nullptr); indev != nullptr;
         indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            lv_indev_set_scroll_throw(indev, 10);
            lv_indev_set_scroll_limit(indev, 10);
        }
    }
    s_cells.clear();
    s_ui = AlbumUi{};
}

}  // namespace

lv_obj_t* AlbumScreen::Create() {
    s_ui = AlbumUi{};
    s_cells.clear();
    s_chrome_visible = true;
    s_state_shown = false;
    s_view_index = -1;
    s_view_current = ViewImage{};
    s_view_lv_path[0] = '\0';

    lv_obj_t* scr = lv_obj_create(nullptr);
    screen_strip_obj_chrome(scr);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(kColorBgGrad), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    s_ui.scr = scr;

    for (lv_indev_t* indev = lv_indev_get_next(nullptr); indev != nullptr;
         indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            // throw 越小惯性越长；limit 越小越容易跟手。离开相册时还原。
            lv_indev_set_scroll_throw(indev, 6);
            lv_indev_set_scroll_limit(indev, 6);
        }
    }

    s_ui.lbl_top = MakeLabel(scr, I18n::T("相册"), kColorMuted, 240);
    lv_obj_align(s_ui.lbl_top, LV_ALIGN_TOP_MID, 0, kTopLabelY);

    BuildGrid(scr);
    BuildViewLayer(scr);
    BuildStateLayer(scr);
    // 返回键最后建，保证在 z 序顶层能点到。
    MakeBackButton(scr, [](lv_event_t*) { GoHome(); });
    lv_obj_move_foreground(s_ui.view_layer);
    lv_obj_move_foreground(s_ui.state_layer);

    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);
    screen_attach_swipe_back(scr, OnSwipeBack);

    s_screen_active = true;
    RefreshTopLabel();
    // 扫描要等 LOAD 才启动，先摆上扫描态，别让第一帧是个空网格。
    ShowStateLayer(I18n::T("正在扫描 SD 卡"), "", false);
    s_ui.tick = lv_timer_create(OnTick, 200, nullptr);
    return scr;
}

void AlbumScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: album");
        s_view_center.store(0, std::memory_order_relaxed);
        StartScan();
        StartWorker();
    } else {
        ESP_LOGI(TAG, "unload: album");
        // 扫描线程看到 abort 后不会再发布结果，自己把那批 Photo 回收掉；
        // worker 换代后也会自己退，它手里的快照保着它正在用的内存。都不用等。
        s_scan_abort.store(true, std::memory_order_relaxed);
        StopWorker();
        {
            std::lock_guard<std::mutex> lock(s_photos_mutex);
            s_photos.reset();
        }
        // 到这里 UI 已经拆完了（OnScreenUnloaded 先跑），只剩裸内存要还。
        if (s_view_current.buf != nullptr) {
            // 描述符是静态的，缓存里那条记录还指着这块内存，下次进相册会被复用。
            lv_image_cache_drop(&s_view_dsc);
            heap_caps_free(s_view_current.buf);
            s_view_current = ViewImage{};
        }
        if (s_view_lv_path[0] != '\0') {
            lv_image_cache_drop(s_view_lv_path);
            s_view_lv_path[0] = '\0';
        }
        ReleaseUiList();
    }
}
