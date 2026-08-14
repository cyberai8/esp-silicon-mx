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

#include "config.h"
#include "home_screen/home_screen.h"
#include "i18n.h"
#include "screen_util.h"
#include "SdCardManager.hpp"

LV_FONT_DECLARE(font_puhui_20_4);

namespace {

constexpr const char* TAG = "AlbumScreen";

constexpr int kScanMaxDepth = 5;
constexpr size_t kMaxPhotos = 300;
// 缩略图常驻上限：68*68*2 ≈ 9KB 一张，48 张不到 0.5MB，超了按“离视口最远”淘汰。
constexpr size_t kMaxThumbs = 48;
// 单张图片读进内存的上限。再大的图解码期间峰值内存不可控，直接标记为不可预览。
constexpr size_t kMaxFileBytes = 6u * 1024 * 1024;
// EXIF 的 APP1 段长度字段是 16 位的，内嵌缩略图必然落在文件头这一段里。
constexpr size_t kExifProbeBytes = 72u * 1024;
// 非 JPEG（PNG/SJPG）交给 LVGL 自己解，只放过小文件，避免它一次性吃掉几 MB。
constexpr uint32_t kLvglDecodeMaxKb = 400;
constexpr uint32_t kPngMaxPixels = 1200u * 1000u;

constexpr uint32_t kColorBg = 0x0B0D10;
constexpr uint32_t kColorBgGrad = 0x14171C;
constexpr uint32_t kColorText = 0xFFFFFF;
constexpr uint32_t kColorMuted = 0x8B92A3;
constexpr uint32_t kColorAccent = 0xFFC061;
constexpr uint32_t kColorCell = 0x1A1E26;
constexpr uint32_t kColorCellPressed = 0x252A34;

// 360 圆屏：可视区是内切圆，半径 180。下面每个 y 都按 sqrt(180²-dy²) 反推过可用
// 宽度，改布局要一起复算，否则内容会被圆角切掉。
constexpr int32_t kPanel = DISPLAY_WIDTH;
constexpr int32_t kBackBtnSize = 36;
constexpr int32_t kBackBtnX = 36;
constexpr int32_t kBackBtnY = 28;
constexpr int32_t kTopLabelY = 24;

constexpr int32_t kThumb = 68;
constexpr int32_t kGridCols = 3;
constexpr int32_t kGridGap = 6;
constexpr int32_t kGridPad = 4;
constexpr int32_t kGridBoxW = kThumb * kGridCols + kGridGap * (kGridCols - 1) + kGridPad * 2;
constexpr int32_t kGridTop = 56;
constexpr int32_t kGridH = 258;
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
    int src_w = 0;  // 0 = 还不知道原始尺寸
    int src_h = 0;

    // 缩略图：worker 线程产出，UI 线程挂载与释放。
    uint8_t* thumb = nullptr;
    lv_image_dsc_t thumb_dsc = {};
    bool thumb_ready = false;   // 数据好了，还没挂到 cell
    bool thumb_skip = false;    // 不做缩略图（非 JPEG / 解码失败 / 太大）
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

bool IsImageName(const char* name) {
    return IsJpegName(name) || ExtEquals(name, "png") || ExtEquals(name, "sjpg") ||
           ExtEquals(name, "spng");
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
            if (!p->jpeg) {
                // 非 JPEG 没法降采样解码，缩略图交给 LVGL 或直接占位。
                p->thumb_skip = true;
                ReadPngSize(path, &p->src_w, &p->src_h);
            } else if (st.st_size > static_cast<off_t>(kMaxFileBytes)) {
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
    vTaskDelete(nullptr);
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
    if (xTaskCreate(ScanTask, "album_scan", 6144, nullptr, 4, nullptr) != pdPASS) {
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

bool ParseJpegSize(const uint8_t* data, size_t len, int* w, int* h) {
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    jpeg_dec_handle_t dec = nullptr;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        return false;
    }
    jpeg_dec_io_t io = {};
    io.inbuf = const_cast<uint8_t*>(data);
    io.inbuf_len = static_cast<int>(len);
    jpeg_dec_header_info_t info = {};
    const jpeg_error_t err = jpeg_dec_parse_header(dec, &io, &info);
    jpeg_dec_close(dec);
    if (err != JPEG_ERR_OK || info.width == 0 || info.height == 0) {
        return false;
    }
    *w = info.width;
    *h = info.height;
    return true;
}

// 解出「不小于 min_w×min_h」的最小一档（1、1/2、1/4、1/8）。
// cover=true 时按长边贴合，用于方形缩略图；false 按短边贴合，用于整图铺满。
bool DecodeJpegScaled(const uint8_t* data, size_t len, int min_w, int min_h, bool cover,
                      uint8_t** out, int* out_w, int* out_h, int* out_stride_px, int* orig_w,
                      int* orig_h) {
    *out = nullptr;
    int sw = 0;
    int sh = 0;
    if (!ParseJpegSize(data, len, &sw, &sh)) {
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
bool MakeThumb(const uint8_t* data, size_t len, uint8_t* dst) {
    uint8_t* dec = nullptr;
    int dw = 0;
    int dh = 0;
    int stride_px = 0;
    if (!DecodeJpegScaled(data, len, kThumb, kThumb, true, &dec, &dw, &dh, &stride_px, nullptr,
                          nullptr)) {
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
    if (!ok) {
        uint8_t* head = nullptr;
        size_t head_len = 0;
        if (LoadFilePrefix(p->path, kExifProbeBytes, &head, &head_len)) {
            ParseJpegSize(head, head_len, &ow, &oh);  // 头部就有 SOF，顺手拿原始尺寸
            size_t t_off = 0;
            size_t t_len = 0;
            if (FindExifThumb(head, head_len, &t_off, &t_len)) {
                ok = MakeThumb(head + t_off, t_len, thumb);
            } else if (head_len < kExifProbeBytes) {
                ok = MakeThumb(head, head_len, thumb);  // 整张图本来就没超过这一段
            }
            heap_caps_free(head);
        }
    }
    if (!ok) {
        // 没有内嵌小图，只能把整张读进来降采样解。
        uint8_t* file = nullptr;
        size_t file_len = 0;
        if (LoadFile(p->path, &file, &file_len)) {
            if (ow <= 0) {
                ParseJpegSize(file, file_len, &ow, &oh);
            }
            ok = MakeThumb(file, file_len, thumb);
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

void DecodeForView(const PhotoListPtr& list, int index, int gen) {
    Photo* p = ItemAt(list, index);
    ViewImage result;
    result.index = index;
    if (p != nullptr) {
        uint8_t* file = nullptr;
        size_t file_len = 0;
        if (LoadFile(p->path, &file, &file_len)) {
            int sw = 0;
            int sh = 0;
            if (ParseJpegSize(file, file_len, &sw, &sh)) {
                {
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
                if (DecodeJpegScaled(file, file_len, tw, th, false, &dec, &dw, &dh, &stride_px,
                                     nullptr, nullptr)) {
                    if (dw == tw && dh == th && stride_px == dw) {
                        result.buf = dec;
                        result.w = dw;
                        result.h = dh;
                        dec = nullptr;
                    } else {
                        // 降采样只有 1/2 这样的整档，剩下的零头用盒式缩放贴到目标尺寸。
                        auto* fit = static_cast<uint8_t*>(
                            AllocBig(static_cast<size_t>(tw) * th * 2));
                        if (fit != nullptr) {
                            BoxScaleRgb565(dec, stride_px * 2, 0, 0, dw, dh, fit, tw, th);
                            result.buf = fit;
                            result.w = tw;
                            result.h = th;
                        }
                    }
                    if (dec != nullptr) {
                        heap_caps_free(dec);
                    }
                }
            }
            heap_caps_free(file);
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
    vTaskDelete(nullptr);
}

void StartWorker() {
    const int gen = s_worker_gen.fetch_add(1, std::memory_order_relaxed) + 1;
    s_view_req.store(-1, std::memory_order_relaxed);
    // 解码钉在 core 0，LVGL 固定在 core 1，两边不抢核。JPEG 软解栈吃得多，给足 8K。
    if (xTaskCreatePinnedToCore(WorkerTask, "album_worker", 8192,
                                reinterpret_cast<void*>(static_cast<intptr_t>(gen)), 3, nullptr,
                                0) != pdPASS) {
        ESP_LOGE(TAG, "worker task create failed");
    }
}

// 只是换代号，不等它退出：老 worker 手里的照片快照会保着它用到的内存。
void StopWorker() {
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
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xFFFFFF), Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_bg_opa(btn, LV_OPA_20, Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, kBackBtnX, kBackBtnY);
    lv_obj_set_ext_click_area(btn, 10);
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
    ReleaseViewCurrent();
    UpdateViewTexts();

    if (p->jpeg) {
        SetViewHint(I18n::T("解码中…"));
        s_view_req.store(index, std::memory_order_relaxed);
        return;
    }

    const bool too_big = p->size_kb > kLvglDecodeMaxKb ||
                         (p->src_w > 0 && static_cast<uint32_t>(p->src_w) *
                                              static_cast<uint32_t>(p->src_h) >
                                              kPngMaxPixels);
    if (too_big || p->path.size() + 3 > sizeof(s_view_lv_path)) {
        SetViewHint(I18n::T("图片过大，无法预览"));
        return;
    }
    snprintf(s_view_lv_path, sizeof(s_view_lv_path), "S:%s", p->path.c_str());
    // PNG 由 LVGL 自己解，我们只能给它一个「不会被圆屏切到」的框，让它按比例缩进去。
    int box_w = 0;
    int box_h = 0;
    FitInCircle(p->src_w, p->src_h, &box_w, &box_h);
    lv_obj_set_size(s_ui.view_img, box_w, box_h);
    lv_obj_center(s_ui.view_img);
    SetViewHint(nullptr);
    lv_image_set_src(s_ui.view_img, s_view_lv_path);
}

void CloseViewer() {
    if (s_ui.view_layer == nullptr) {
        return;
    }
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
    if (!EventFromSelf(e)) {
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
}

void OnViewReleased(lv_event_t* e) {
    if (!EventFromSelf(e)) {
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

    if (adx >= kSwipeMinPx && ady <= kSwipeMaxDy) {
        ShowPhoto(dx > 0 ? s_view_index - 1 : s_view_index + 1);
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
    lv_obj_add_event_cb(layer, OnViewReleased, LV_EVENT_RELEASED, nullptr);
    s_ui.view_layer = layer;

    s_ui.view_img = lv_image_create(layer);
    // 尺寸每张图单独定（见 FitInCircle），这里只给个安全初值。
    lv_obj_set_size(s_ui.view_img, kViewSafeBox, kViewSafeBox);
    lv_obj_center(s_ui.view_img);
    lv_image_set_inner_align(s_ui.view_img, LV_IMAGE_ALIGN_CONTAIN);
    lv_image_set_antialias(s_ui.view_img, true);
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
        lv_obj_set_style_radius(cell, 10, LV_PART_MAIN);
        lv_obj_set_style_clip_corner(cell, true, LV_PART_MAIN);
        lv_obj_set_style_bg_color(cell, lv_color_hex(kColorCell), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(cell, lv_color_hex(kColorCellPressed),
                                  Sel(LV_PART_MAIN, LV_STATE_PRESSED));
        lv_image_set_inner_align(cell, LV_IMAGE_ALIGN_CONTAIN);
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
        } else if (!p->jpeg) {
            // PNG/SJPG 没法降采样，小文件让 LVGL 自己缩，大文件只留角标。
            const bool ok = p->size_kb <= kLvglDecodeMaxKb &&
                            !(p->src_w > 0 && static_cast<uint32_t>(p->src_w) *
                                                      static_cast<uint32_t>(p->src_h) >
                                                  kPngMaxPixels);
            if (ok) {
                // lv_image_set_src 会自己 strdup 路径，这里用临时缓冲就够。
                char lv_path[280];
                snprintf(lv_path, sizeof(lv_path), "S:%s", p->path.c_str());
                lv_image_set_src(cell, lv_path);
            } else {
                lv_obj_t* tag = MakeLabel(cell, ExtLabel(p->name), kColorMuted, 0);
                lv_obj_center(tag);
            }
        } else if (p->thumb_skip) {
            // 大到不敢解的 JPEG：格子里摆个角标，别只留一块空砖。
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
            ShowStateLayer(I18n::T("SD 卡里没有图片"),
                           I18n::T("支持 JPG / PNG / SJPG"), true);
        } else {
            HideStateLayer();
        }
    }

    SyncViewCenterFromScroll();
    ApplyReadyThumbs();
    EvictFarThumbs();
    ApplyPendingView();
}

void OnSwipeBack() {
    GoHome();
}

void OnScreenUnloaded(lv_event_t* /*e*/) {
    if (s_ui.tick != nullptr) {
        lv_timer_delete(s_ui.tick);
    }
    s_screen_active = false;
    s_view_index = -1;
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
