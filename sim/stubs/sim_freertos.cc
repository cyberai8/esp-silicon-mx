// FreeRTOS + esp_timer 的 PC 实现。
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace {

const std::chrono::steady_clock::time_point kBoot = std::chrono::steady_clock::now();

struct SimSem {
    std::mutex m;
    std::condition_variable cv;
    int count = 0;
    int max_count = 1;
};

struct SimEventGroup {
    std::mutex m;
    std::condition_variable cv;
    EventBits_t bits = 0;
};

}  // namespace

// ---------------------------------------------------------------------------
// 任务
// ---------------------------------------------------------------------------

extern "C" BaseType_t xTaskCreate(TaskFunction_t fn, const char* name, uint32_t /*stack_depth*/,
                                  void* arg, UBaseType_t /*priority*/,
                                  TaskHandle_t* out_handle) {
    if (out_handle != nullptr) {
        *out_handle = nullptr;
    }
    try {
        std::thread t([fn, arg]() { fn(arg); });
        t.detach();
    } catch (...) {
        fprintf(stderr, "[sim] xTaskCreate(%s) failed\n", name != nullptr ? name : "?");
        return pdFAIL;
    }
    return pdPASS;
}

extern "C" BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char* name,
                                              uint32_t stack_depth, void* arg,
                                              UBaseType_t priority, TaskHandle_t* out_handle,
                                              BaseType_t /*core_id*/) {
    // PC 上不绑核，绑核只影响真机上的抢占关系。
    return xTaskCreate(fn, name, stack_depth, arg, priority, out_handle);
}

extern "C" void vTaskDelete(TaskHandle_t handle) {
    if (handle != nullptr) {
        fprintf(stderr, "[sim] vTaskDelete(other task) 在仿真里不支持，已忽略\n");
    }
}

extern "C" void vTaskDelay(TickType_t ticks) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ticks));
}

extern "C" TickType_t xTaskGetTickCount(void) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - kBoot)
                        .count();
    return static_cast<TickType_t>(ms);
}

extern "C" TaskHandle_t xTaskGetCurrentTaskHandle(void) {
    thread_local int self = 0;
    return &self;
}

extern "C" UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t /*handle*/) {
    return 4096;
}

extern "C" void taskYIELD(void) {
    std::this_thread::yield();
}

// ---------------------------------------------------------------------------
// 信号量
// ---------------------------------------------------------------------------

extern "C" SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    auto* s = new SimSem();
    s->count = 1;
    s->max_count = 1;
    return s;
}

extern "C" SemaphoreHandle_t xSemaphoreCreateBinary(void) {
    auto* s = new SimSem();
    s->count = 0;
    s->max_count = 1;
    return s;
}

extern "C" SemaphoreHandle_t xSemaphoreCreateCounting(UBaseType_t max_count,
                                                      UBaseType_t initial_count) {
    auto* s = new SimSem();
    s->count = static_cast<int>(initial_count);
    s->max_count = static_cast<int>(max_count);
    return s;
}

extern "C" BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks_to_wait) {
    auto* s = static_cast<SimSem*>(sem);
    if (s == nullptr) {
        return pdFAIL;
    }
    std::unique_lock<std::mutex> lock(s->m);
    if (ticks_to_wait == portMAX_DELAY) {
        s->cv.wait(lock, [s] { return s->count > 0; });
    } else if (!s->cv.wait_for(lock, std::chrono::milliseconds(ticks_to_wait),
                               [s] { return s->count > 0; })) {
        return pdFAIL;
    }
    --s->count;
    return pdPASS;
}

extern "C" BaseType_t xSemaphoreGive(SemaphoreHandle_t sem) {
    auto* s = static_cast<SimSem*>(sem);
    if (s == nullptr) {
        return pdFAIL;
    }
    {
        std::lock_guard<std::mutex> lock(s->m);
        if (s->count >= s->max_count) {
            return pdFAIL;
        }
        ++s->count;
    }
    s->cv.notify_one();
    return pdPASS;
}

extern "C" void vSemaphoreDelete(SemaphoreHandle_t sem) {
    delete static_cast<SimSem*>(sem);
}

// ---------------------------------------------------------------------------
// 事件组
// ---------------------------------------------------------------------------

extern "C" EventGroupHandle_t xEventGroupCreate(void) {
    return new SimEventGroup();
}

extern "C" EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits) {
    auto* g = static_cast<SimEventGroup*>(group);
    EventBits_t now = 0;
    {
        std::lock_guard<std::mutex> lock(g->m);
        g->bits |= bits;
        now = g->bits;
    }
    g->cv.notify_all();
    return now;
}

extern "C" EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits) {
    auto* g = static_cast<SimEventGroup*>(group);
    std::lock_guard<std::mutex> lock(g->m);
    const EventBits_t before = g->bits;
    g->bits &= ~bits;
    return before;
}

extern "C" EventBits_t xEventGroupGetBits(EventGroupHandle_t group) {
    auto* g = static_cast<SimEventGroup*>(group);
    std::lock_guard<std::mutex> lock(g->m);
    return g->bits;
}

extern "C" EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits_to_wait,
                                           BaseType_t clear_on_exit, BaseType_t wait_for_all,
                                           TickType_t ticks_to_wait) {
    auto* g = static_cast<SimEventGroup*>(group);
    std::unique_lock<std::mutex> lock(g->m);
    auto ready = [&] {
        return wait_for_all ? ((g->bits & bits_to_wait) == bits_to_wait)
                            : ((g->bits & bits_to_wait) != 0);
    };
    if (ticks_to_wait == portMAX_DELAY) {
        g->cv.wait(lock, ready);
    } else {
        g->cv.wait_for(lock, std::chrono::milliseconds(ticks_to_wait), ready);
    }
    const EventBits_t got = g->bits;
    if (clear_on_exit && ready()) {
        g->bits &= ~bits_to_wait;
    }
    return got;
}

extern "C" void vEventGroupDelete(EventGroupHandle_t group) {
    delete static_cast<SimEventGroup*>(group);
}

// ---------------------------------------------------------------------------
// esp_timer
// ---------------------------------------------------------------------------

struct esp_timer_sim {
    esp_timer_cb_t cb = nullptr;
    void* arg = nullptr;
    std::mutex m;
    std::condition_variable cv;
    bool running = false;
    bool quit = false;
    bool once = false;
    uint64_t period_us = 0;
    std::thread th;
};

namespace {

void TimerLoop(esp_timer_sim* t) {
    std::unique_lock<std::mutex> lock(t->m);
    while (!t->quit) {
        if (!t->running) {
            t->cv.wait(lock, [t] { return t->quit || t->running; });
            continue;
        }
        const uint64_t period = t->period_us;
        const bool once = t->once;
        if (t->cv.wait_for(lock, std::chrono::microseconds(period),
                           [t] { return t->quit || !t->running; })) {
            continue;  // 被 stop / delete 打断
        }
        esp_timer_cb_t cb = t->cb;
        void* arg = t->arg;
        if (once) {
            t->running = false;
        }
        lock.unlock();
        if (cb != nullptr) {
            cb(arg);
        }
        lock.lock();
    }
}

}  // namespace

extern "C" esp_err_t esp_timer_create(const esp_timer_create_args_t* args, esp_timer_handle_t* out) {
    if (args == nullptr || out == nullptr) {
        return ESP_FAIL;
    }
    auto* t = new esp_timer_sim();
    t->cb = args->callback;
    t->arg = args->arg;
    t->th = std::thread(TimerLoop, t);
    *out = t;
    return 0;
}

extern "C" esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period_us) {
    if (timer == nullptr) {
        return ESP_FAIL;
    }
    {
        std::lock_guard<std::mutex> lock(timer->m);
        timer->period_us = period_us;
        timer->once = false;
        timer->running = true;
    }
    timer->cv.notify_all();
    return 0;
}

extern "C" esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us) {
    if (timer == nullptr) {
        return ESP_FAIL;
    }
    {
        std::lock_guard<std::mutex> lock(timer->m);
        timer->period_us = timeout_us;
        timer->once = true;
        timer->running = true;
    }
    timer->cv.notify_all();
    return 0;
}

extern "C" esp_err_t esp_timer_stop(esp_timer_handle_t timer) {
    if (timer == nullptr) {
        return ESP_FAIL;
    }
    {
        std::lock_guard<std::mutex> lock(timer->m);
        timer->running = false;
    }
    timer->cv.notify_all();
    return 0;
}

extern "C" esp_err_t esp_timer_delete(esp_timer_handle_t timer) {
    if (timer == nullptr) {
        return ESP_FAIL;
    }
    {
        std::lock_guard<std::mutex> lock(timer->m);
        timer->quit = true;
        timer->running = false;
    }
    timer->cv.notify_all();
    if (timer->th.joinable()) {
        timer->th.join();
    }
    delete timer;
    return 0;
}
