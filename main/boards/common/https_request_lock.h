#pragma once

#include <esp_heap_caps.h>
#include <mutex>

// TLS：sdkconfig 已设 MBEDTLS_EXTERNAL_MEM_ALLOC + 关闭 HARDWARE_AES，
// 让 mbedTLS 堆与软件 AES 走 PSRAM，避免 esp-aes 抢内部 DMA。
inline std::mutex& HttpsRequestLock() {
    static std::mutex lock;
    return lock;
}

inline size_t HttpsLargestInternalBlock() {
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
}

inline size_t HttpsLargestSpiramBlock() {
    return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

// 仍保留内部块检查：WiFi/MQTT 控制面与 DMA 仍需少量 internal。
inline bool HttpsInternalRamReady(size_t min_bytes = 16 * 1024) {
    return HttpsLargestInternalBlock() >= min_bytes;
}
