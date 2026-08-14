#pragma once

// FreeRTOS 的 PC 替身（只覆盖 UI 代码用到的部分）：tick 就是毫秒，
// 任务是 pthread，互斥量/事件组用标准库实现。
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;

#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0

#define configTICK_RATE_HZ 1000
#define portTICK_PERIOD_MS 1
#define portMAX_DELAY ((TickType_t)0xFFFFFFFFu)

#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define pdTICKS_TO_MS(t) ((uint32_t)(t))

#define tskNO_AFFINITY 0x7FFFFFFF
