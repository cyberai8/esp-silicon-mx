#pragma once

#include <esp_heap_caps.h>
#include <mutex>

// TLS/AES 缓冲走内部 DRAM，并发 HTTPS 容易 esp-aes Failed to allocate memory。
inline std::mutex& HttpsRequestLock() {
    static std::mutex lock;
    return lock;
}

inline size_t HttpsLargestInternalBlock() {
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
}

inline bool HttpsInternalRamReady(size_t min_bytes = 24 * 1024) {
    return HttpsLargestInternalBlock() >= min_bytes;
}
