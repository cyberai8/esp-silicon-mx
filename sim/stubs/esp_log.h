#pragma once

// ESP_LOG* 的 PC 替身：直接打到 stderr，格式和 idf monitor 接近，方便对照。
#include <cstdio>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_LOG_NONE,
    ESP_LOG_ERROR,
    ESP_LOG_WARN,
    ESP_LOG_INFO,
    ESP_LOG_DEBUG,
    ESP_LOG_VERBOSE,
} esp_log_level_t;

void sim_log_set_level(esp_log_level_t level);
esp_log_level_t sim_log_level(void);
void sim_log_write(esp_log_level_t level, char tag_char, const char* tag, const char* fmt, ...)
    __attribute__((format(printf, 4, 5)));

#ifdef __cplusplus
}
#endif

#define ESP_LOG_LEVEL_LOCAL(level, tag, format, ...) \
    sim_log_write((level), 'I', (tag), format, ##__VA_ARGS__)

#define ESP_LOGE(tag, format, ...) sim_log_write(ESP_LOG_ERROR, 'E', tag, format, ##__VA_ARGS__)
#define ESP_LOGW(tag, format, ...) sim_log_write(ESP_LOG_WARN, 'W', tag, format, ##__VA_ARGS__)
#define ESP_LOGI(tag, format, ...) sim_log_write(ESP_LOG_INFO, 'I', tag, format, ##__VA_ARGS__)
#define ESP_LOGD(tag, format, ...) sim_log_write(ESP_LOG_DEBUG, 'D', tag, format, ##__VA_ARGS__)
#define ESP_LOGV(tag, format, ...) sim_log_write(ESP_LOG_VERBOSE, 'V', tag, format, ##__VA_ARGS__)

#define esp_log_level_set(tag, level) ((void)0)

#include <chrono>
inline uint32_t esp_log_timestamp() {
    using namespace std::chrono;
    return static_cast<uint32_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
            .count());
}
