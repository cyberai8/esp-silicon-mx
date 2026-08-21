#pragma once

#include <esp_err.h>

inline esp_err_t esp_lv_adapter_lock(int /*timeout_ms*/) { return ESP_OK; }
inline void esp_lv_adapter_unlock() {}
