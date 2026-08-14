// ESP-IDF 那几个 C API 在 PC 上的实现：日志、堆、esp_timer、随机数。
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <malloc.h>
#include <random>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

// ---------------------------------------------------------------------------
// 日志
// ---------------------------------------------------------------------------

namespace {
esp_log_level_t s_level = ESP_LOG_INFO;
std::atomic<size_t> s_in_use{0};
std::atomic<size_t> s_peak{0};

int64_t NowUs() {
    timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}
}  // namespace

extern "C" void sim_log_set_level(esp_log_level_t level) {
    s_level = level;
}

extern "C" esp_log_level_t sim_log_level(void) {
    return s_level;
}

extern "C" void sim_log_write(esp_log_level_t level, char tag_char, const char* tag,
                              const char* fmt, ...) {
    if (level > s_level) {
        return;
    }
    fprintf(stderr, "%c (%lld) %s: ", tag_char, static_cast<long long>(NowUs() / 1000), tag);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

// ---------------------------------------------------------------------------
// 堆
// ---------------------------------------------------------------------------

namespace {
void Track(void* p) {
    if (p != nullptr) {
        const size_t n = malloc_usable_size(p);
        const size_t now = s_in_use.fetch_add(n) + n;
        size_t peak = s_peak.load();
        while (now > peak && !s_peak.compare_exchange_weak(peak, now)) {
        }
    }
}
}  // namespace

extern "C" void* heap_caps_malloc(size_t size, uint32_t /*caps*/) {
    void* p = malloc(size);
    Track(p);
    return p;
}

extern "C" void* heap_caps_calloc(size_t n, size_t size, uint32_t /*caps*/) {
    void* p = calloc(n, size);
    Track(p);
    return p;
}

extern "C" void* heap_caps_realloc(void* ptr, size_t size, uint32_t /*caps*/) {
    if (ptr != nullptr) {
        s_in_use.fetch_sub(malloc_usable_size(ptr));
    }
    void* p = realloc(ptr, size);
    Track(p);
    return p;
}

extern "C" void* heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t /*caps*/) {
    void* p = nullptr;
    if (posix_memalign(&p, alignment < sizeof(void*) ? sizeof(void*) : alignment, size) != 0) {
        return nullptr;
    }
    Track(p);
    return p;
}

extern "C" void heap_caps_free(void* ptr) {
    if (ptr != nullptr) {
        s_in_use.fetch_sub(malloc_usable_size(ptr));
    }
    free(ptr);
}

extern "C" size_t heap_caps_get_free_size(uint32_t /*caps*/) {
    return 8u * 1024 * 1024;
}

extern "C" size_t heap_caps_get_largest_free_block(uint32_t /*caps*/) {
    // 真机上这是 PSRAM 里最大的连续块。仿真里给一个「和 8MB PSRAM 差不多」的值，
    // 让相册那类「先问问放不放得下」的判断走到和真机一致的分支。
    return 7u * 1024 * 1024;
}

extern "C" size_t heap_caps_get_total_size(uint32_t /*caps*/) {
    return 8u * 1024 * 1024;
}

extern "C" size_t sim_heap_in_use(void) {
    return s_in_use.load();
}

extern "C" size_t sim_heap_peak(void) {
    return s_peak.load();
}

// ---------------------------------------------------------------------------
// esp_timer / 随机数
// ---------------------------------------------------------------------------

extern "C" int64_t esp_timer_get_time(void) {
    return NowUs();
}

extern "C" uint32_t esp_random(void) {
    static std::mt19937 rng{12345};  // 固定种子，仿真结果可复现
    return static_cast<uint32_t>(rng());
}
