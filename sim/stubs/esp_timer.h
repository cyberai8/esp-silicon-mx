#pragma once

// esp_timer 的 PC 替身：只做到屏幕代码用得上的部分（取时间 + 周期回调）。
// 回调在独立线程里跑，和真机上「不在 LVGL 线程」的约束一致。
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

int64_t esp_timer_get_time(void);

typedef void (*esp_timer_cb_t)(void* arg);
typedef struct esp_timer_sim* esp_timer_handle_t;

typedef enum {
    ESP_TIMER_TASK,
    ESP_TIMER_ISR,
} esp_timer_dispatch_t;

typedef struct {
    esp_timer_cb_t callback;
    void* arg;
    esp_timer_dispatch_t dispatch_method;
    const char* name;
    bool skip_unhandled_events;
} esp_timer_create_args_t;

esp_err_t esp_timer_create(const esp_timer_create_args_t* args, esp_timer_handle_t* out);
esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period_us);
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us);
esp_err_t esp_timer_stop(esp_timer_handle_t timer);
esp_err_t esp_timer_delete(esp_timer_handle_t timer);

#ifdef __cplusplus
}
#endif
