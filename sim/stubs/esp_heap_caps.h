#pragma once

// heap_caps_* 的 PC 替身。PSRAM/内部 RAM 的区分在 PC 上没有意义，但保留
// MALLOC_CAP_* 常量，屏幕代码不用改；largest_free_block 返回一个够大的数，
// 让「够不够一整张图」这类判断在仿真里能通过。
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#define MALLOC_CAP_DEFAULT (1 << 0)
#define MALLOC_CAP_INTERNAL (1 << 1)
#define MALLOC_CAP_SPIRAM (1 << 2)
#define MALLOC_CAP_DMA (1 << 3)
#define MALLOC_CAP_8BIT (1 << 4)
#define MALLOC_CAP_32BIT (1 << 5)

#ifdef __cplusplus
extern "C" {
#endif

void* heap_caps_malloc(size_t size, uint32_t caps);
void* heap_caps_calloc(size_t n, size_t size, uint32_t caps);
void* heap_caps_realloc(void* ptr, size_t size, uint32_t caps);
void* heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps);
void heap_caps_free(void* ptr);
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
size_t heap_caps_get_total_size(uint32_t caps);

// 仿真额外提供：看看当前占用峰值，方便判断真机上会不会爆内存。
size_t sim_heap_in_use(void);
size_t sim_heap_peak(void);

#ifdef __cplusplus
}
#endif
