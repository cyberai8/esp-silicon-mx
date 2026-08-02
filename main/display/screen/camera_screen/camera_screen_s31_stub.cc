#include "camera_screen/camera_screen.h"

#include <esp_log.h>

// ESP32-S31：MetalioClaw4 相机 App 仍是 P4 MIPI-CSI 路径，暂用空壳保证链接。
// 板级 DVP/OV3660 可后续按官方小智 EspVideo 接入。

static const char* TAG = "CameraScreen";

lv_obj_t* CameraScreen::Create() {
    ESP_LOGW(TAG, "Camera app not available on ESP32-S31 build yet");
    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_t* lbl = lv_label_create(scr);
    lv_label_set_text(lbl, "Camera N/A on S31");
    lv_obj_center(lbl);
    return scr;
}

void CameraScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    (void)event;
}

bool CameraScreen::PreparePreviewBuffer(PreviewBuffer* out) {
    if (out) {
        out->data = nullptr;
        out->width = 0;
        out->height = 0;
    }
    return false;
}

esp_err_t CameraScreen::StartExternalPreview(lv_obj_t* canvas) {
    (void)canvas;
    return ESP_ERR_NOT_SUPPORTED;
}

void CameraScreen::StopExternalPreview() {}
