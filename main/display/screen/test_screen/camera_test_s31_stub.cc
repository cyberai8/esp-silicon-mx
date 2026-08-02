#include "camera_test.h"

#include <esp_log.h>

// ESP32-S31：P4 相机测试项不可用，提供空实现保证链接。

namespace CameraTest {

namespace {
constexpr const char* TAG = "CameraTest";
}

void BuildRow(lv_obj_t* /*list*/) {
    ESP_LOGW(TAG, "Camera test not available on ESP32-S31");
}

void OnLoad() {}
void OnUnload() {}
void Poll() {}

}  // namespace CameraTest
