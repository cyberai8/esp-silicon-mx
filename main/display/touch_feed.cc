#include "touch_feed.h"

#include <driver/gpio.h>
#include <esp_intr_alloc.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace {

constexpr const char* kTag = "TouchFeed";
constexpr uint32_t kIdlePeriodMs = 16;
constexpr uint32_t kPressedPeriodMs = 8;
constexpr UBaseType_t kReaderPriority = 6;

esp_lcd_touch_handle_t s_handle = nullptr;
SemaphoreHandle_t s_mutex = nullptr;
TaskHandle_t s_task = nullptr;
volatile bool s_run = false;
uint32_t s_period_ms = kIdlePeriodMs;

volatile bool s_int_latched = false;
bool s_int_isr_installed = false;
gpio_num_t s_int_gpio = GPIO_NUM_NC;

struct TouchSnapshot {
    bool pressed = false;
    int16_t x = 0;
    int16_t y = 0;
};

TouchSnapshot s_snap;

#if TOUCH_FEED_DEBUG
bool s_log_was_pressed = false;
int s_log_last_x = -1;
int s_log_last_y = -1;

void LogSnapshotIfChanged(const TouchSnapshot& next) {
    if (!next.pressed) {
        if (s_log_was_pressed) {
            ESP_LOGI(kTag, "released");
            s_log_was_pressed = false;
            s_log_last_x = -1;
            s_log_last_y = -1;
        }
        return;
    }

    const int dx = (s_log_last_x >= 0) ? (next.x - s_log_last_x) : 0;
    const int dy = (s_log_last_y >= 0) ? (next.y - s_log_last_y) : 0;
    const bool moved = !s_log_was_pressed || dx != 0 || dy != 0;
    if (!s_log_was_pressed) {
        ESP_LOGI(kTag, "down: (%d,%d)", next.x, next.y);
    } else if (moved) {
        ESP_LOGD(kTag, "move: (%d,%d) d=(%+d,%+d)", next.x, next.y, dx, dy);
    }

    s_log_was_pressed = true;
    s_log_last_x = next.x;
    s_log_last_y = next.y;
}
#endif

bool TouchIntLevelActive() {
    if (s_handle == nullptr || s_int_gpio == GPIO_NUM_NC) {
        return s_int_gpio == GPIO_NUM_NC;
    }
    const int level = gpio_get_level(s_int_gpio);
    const int active = s_handle->config.levels.interrupt ? 1 : 0;
    return level == active;
}

bool ShouldReadChip() {
    if (s_handle == nullptr) {
        return false;
    }
    if (s_int_gpio == GPIO_NUM_NC) {
        return true;
    }
    // 边沿锁存：短 INT 脉冲即使已恢复，也强制读一次，避免漏点。
    return TouchIntLevelActive() || s_int_latched || s_snap.pressed;
}

void IRAM_ATTR TouchIntIsr(void* /*arg*/) {
    s_int_latched = true;
    BaseType_t hp = pdFALSE;
    if (s_task != nullptr) {
        vTaskNotifyGiveFromISR(s_task, &hp);
    }
    portYIELD_FROM_ISR(hp);
}

void PublishSnapshot(const TouchSnapshot& next) {
    if (s_mutex != nullptr &&
        xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        s_snap = next;
        xSemaphoreGive(s_mutex);
    }
#if TOUCH_FEED_DEBUG
    LogSnapshotIfChanged(next);
#endif
}

void UpdateSnapshotFromChip() {
    if (s_handle == nullptr) {
        return;
    }

    if (!ShouldReadChip()) {
        TouchSnapshot next = s_snap;
        next.pressed = false;
        PublishSnapshot(next);
        return;
    }

    // 消费本次锁存；读失败时若电平仍有效，下一轮还会再读。
    s_int_latched = false;

    if (esp_lcd_touch_read_data(s_handle) != ESP_OK) {
        // 短脉冲后偶发 NACK：保留 latched，稍后重试，避免吞掉按下。
        s_int_latched = true;
        return;
    }

    esp_lcd_touch_point_data_t points[1] = {};
    uint8_t cnt = 0;
    if (esp_lcd_touch_get_data(s_handle, points, &cnt, 1) != ESP_OK) {
        s_int_latched = true;
        return;
    }

    TouchSnapshot next = s_snap;
    if (cnt > 0) {
        next.pressed = true;
        next.x = static_cast<int16_t>(points[0].x);
        next.y = static_cast<int16_t>(points[0].y);
    } else {
        next.pressed = false;
    }
    PublishSnapshot(next);
}

void ReaderTask(void* /*arg*/) {
#if TOUCH_FEED_DEBUG
    ESP_LOGI(kTag, "reader started, idle=%u ms pressed=%u ms latch=%d",
             static_cast<unsigned>(s_period_ms),
             static_cast<unsigned>(kPressedPeriodMs),
             s_int_isr_installed ? 1 : 0);
#endif

    while (s_run) {
        UpdateSnapshotFromChip();

        const bool pressed = s_snap.pressed || s_int_latched;
        const uint32_t wait_ms = pressed ? kPressedPeriodMs : s_period_ms;
        // INT 到来时 ISR 会 notify，立刻醒来读，不等满周期。
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
    }

    s_task = nullptr;
    vTaskDelete(nullptr);
}

void IndevReadCb(lv_indev_t* indev, lv_indev_data_t* data) {
    (void)indev;
    if (data == nullptr) {
        return;
    }

    TouchSnapshot snap;
    if (s_mutex != nullptr &&
        xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        snap = s_snap;
        xSemaphoreGive(s_mutex);
    }

    data->point.x = snap.x;
    data->point.y = snap.y;
    data->state =
        snap.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

bool InstallIntLatchIsr() {
    if (s_handle == nullptr) {
        return false;
    }
    s_int_gpio = s_handle->config.int_gpio_num;
    if (s_int_gpio == GPIO_NUM_NC) {
        return false;
    }
    if (s_int_isr_installed) {
        return true;
    }

    esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(kTag, "gpio_install_isr_service: %s", esp_err_to_name(err));
        return false;
    }

    // active-low → 下降沿；active-high → 上升沿
    const gpio_int_type_t edge = s_handle->config.levels.interrupt
                                     ? GPIO_INTR_POSEDGE
                                     : GPIO_INTR_NEGEDGE;
    gpio_set_intr_type(s_int_gpio, edge);

    // 若面板驱动已占用该 GPIO 的 ISR，尝试卸掉再挂我们的锁存。
    err = gpio_isr_handler_add(s_int_gpio, TouchIntIsr, nullptr);
    if (err == ESP_ERR_INVALID_STATE) {
        gpio_isr_handler_remove(s_int_gpio);
        err = gpio_isr_handler_add(s_int_gpio, TouchIntIsr, nullptr);
    }
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "gpio_isr_handler_add GPIO%d: %s",
                 static_cast<int>(s_int_gpio), esp_err_to_name(err));
        return false;
    }

    s_int_isr_installed = true;
    ESP_LOGI(kTag, "INT edge latch on GPIO%d", static_cast<int>(s_int_gpio));
    return true;
}

void UninstallIntLatchIsr() {
    if (s_int_isr_installed && s_int_gpio != GPIO_NUM_NC) {
        gpio_isr_handler_remove(s_int_gpio);
        s_int_isr_installed = false;
    }
    s_int_latched = false;
    s_int_gpio = GPIO_NUM_NC;
}

}  // namespace

void touch_feed_init(esp_lcd_touch_handle_t handle, uint32_t period_ms) {
    touch_feed_stop();

    s_handle = handle;
    s_period_ms = (period_ms == 0) ? kIdlePeriodMs : period_ms;
    s_int_latched = false;

    if (s_mutex == nullptr) {
        s_mutex = xSemaphoreCreateMutex();
    }
    if (s_mutex == nullptr) {
        ESP_LOGE(kTag, "mutex create failed");
        return;
    }

    {
        TouchSnapshot cleared;
        if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
            s_snap = cleared;
            xSemaphoreGive(s_mutex);
        }
    }
#if TOUCH_FEED_DEBUG
    s_log_was_pressed = false;
    s_log_last_x = -1;
    s_log_last_y = -1;
#endif

    s_run = true;
    if (xTaskCreate(ReaderTask, "touch_feed", 4096, nullptr, kReaderPriority,
                    &s_task) != pdPASS) {
        s_run = false;
        s_task = nullptr;
        ESP_LOGE(kTag, "xTaskCreate failed");
        return;
    }

    if (!InstallIntLatchIsr()) {
        ESP_LOGW(kTag, "INT latch unavailable, fallback to level poll");
    }
}

void touch_feed_attach_indev(lv_indev_t* indev) {
    if (indev == nullptr) {
        ESP_LOGW(kTag, "attach_indev: null indev");
        return;
    }
    lv_indev_set_read_cb(indev, IndevReadCb);
}

void touch_feed_stop() {
    if (s_task != nullptr) {
        s_run = false;
        xTaskNotifyGive(s_task);
        for (int i = 0; i < 50 && s_task != nullptr; ++i) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (s_task != nullptr) {
            vTaskDelete(s_task);
            s_task = nullptr;
        }
    }
    s_run = false;
    UninstallIntLatchIsr();
    s_handle = nullptr;
}
