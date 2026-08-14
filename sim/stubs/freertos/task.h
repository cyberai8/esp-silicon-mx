#pragma once

#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void* TaskHandle_t;
typedef void (*TaskFunction_t)(void*);

BaseType_t xTaskCreate(TaskFunction_t fn, const char* name, uint32_t stack_depth, void* arg,
                       UBaseType_t priority, TaskHandle_t* out_handle);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char* name, uint32_t stack_depth,
                                   void* arg, UBaseType_t priority, TaskHandle_t* out_handle,
                                   BaseType_t core_id);
// 注意：仿真里 vTaskDelete(NULL) 是空操作 —— 任务函数末尾调用它之后自然返回，
// 线程随之结束，语义和真机一致。删别的任务在 PC 上做不到，只会打一条日志。
void vTaskDelete(TaskHandle_t handle);
void vTaskDelay(TickType_t ticks);
TickType_t xTaskGetTickCount(void);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t handle);
void taskYIELD(void);

#ifdef __cplusplus
}
#endif
