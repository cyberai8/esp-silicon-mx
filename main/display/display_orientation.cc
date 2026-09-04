#include "display_orientation.h"

#include <cmath>
#include <cstdlib>

#include "config.h"
#include "display_refresh_blank.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_timer.h"
#include "lvgl.h"

namespace {

constexpr const char* TAG = "DispOrient";

typedef struct {
    int m00;
    int m01;
    int m10;
    int m11;
} OrientationMatrix;

esp_lcd_panel_handle_t s_panel = nullptr;
esp_lcd_touch_handle_t s_touch = nullptr;
bool s_base_swap_xy = false;
bool s_base_mirror_x = false;
bool s_base_mirror_y = false;
esp_lv_adapter_rotation_t s_current = ESP_LV_ADAPTER_ROTATE_0;
esp_lv_adapter_rotation_t s_candidate = ESP_LV_ADAPTER_ROTATE_0;
esp_lv_adapter_rotation_t s_raw_detected = ESP_LV_ADAPTER_ROTATE_0;
int s_stable_count = 0;
int32_t s_gyro_z_accum = 0;
int64_t s_last_log_us = 0;

OrientationMatrix MatrixFromFlags(bool swap_xy, bool mirror_x, bool mirror_y) {
    int axis_x_x = 1;
    int axis_x_y = 0;
    int axis_y_x = 0;
    int axis_y_y = 1;

    if (swap_xy) {
        const int tmp_x = axis_x_x;
        const int tmp_y = axis_x_y;
        axis_x_x = axis_y_x;
        axis_x_y = axis_y_y;
        axis_y_x = tmp_x;
        axis_y_y = tmp_y;
    }
    if (mirror_x) {
        axis_x_x = -axis_x_x;
        axis_x_y = -axis_x_y;
    }
    if (mirror_y) {
        axis_y_x = -axis_y_x;
        axis_y_y = -axis_y_y;
    }

    return OrientationMatrix{
        .m00 = axis_x_x,
        .m01 = axis_y_x,
        .m10 = axis_x_y,
        .m11 = axis_y_y,
    };
}

OrientationMatrix MatrixMultiply(OrientationMatrix lhs, OrientationMatrix rhs) {
    return OrientationMatrix{
        .m00 = lhs.m00 * rhs.m00 + lhs.m01 * rhs.m10,
        .m01 = lhs.m00 * rhs.m01 + lhs.m01 * rhs.m11,
        .m10 = lhs.m10 * rhs.m00 + lhs.m11 * rhs.m10,
        .m11 = lhs.m10 * rhs.m01 + lhs.m11 * rhs.m11,
    };
}

OrientationMatrix MatrixFromRotation(esp_lv_adapter_rotation_t rotation) {
    switch (rotation) {
    case ESP_LV_ADAPTER_ROTATE_90:
        return MatrixFromFlags(true, true, false);
    case ESP_LV_ADAPTER_ROTATE_180:
        return MatrixFromFlags(false, true, true);
    case ESP_LV_ADAPTER_ROTATE_270:
        return MatrixFromFlags(true, false, true);
    case ESP_LV_ADAPTER_ROTATE_0:
    default:
        return MatrixFromFlags(false, false, false);
    }
}

void GetCombinedOrientationFlags(esp_lv_adapter_rotation_t rotation, bool* swap_xy,
                                 bool* mirror_x, bool* mirror_y) {
    const OrientationMatrix base =
        MatrixFromFlags(s_base_swap_xy, s_base_mirror_x, s_base_mirror_y);
    const OrientationMatrix rotation_matrix = MatrixFromRotation(rotation);
    const OrientationMatrix desired = MatrixMultiply(rotation_matrix, base);

    for (int test_swap = 0; test_swap <= 1; ++test_swap) {
        for (int test_mirror_x = 0; test_mirror_x <= 1; ++test_mirror_x) {
            for (int test_mirror_y = 0; test_mirror_y <= 1; ++test_mirror_y) {
                const OrientationMatrix candidate =
                    MatrixFromFlags(test_swap != 0, test_mirror_x != 0, test_mirror_y != 0);
                if (candidate.m00 == desired.m00 && candidate.m01 == desired.m01 &&
                    candidate.m10 == desired.m10 && candidate.m11 == desired.m11) {
                    if (swap_xy != nullptr) {
                        *swap_xy = test_swap != 0;
                    }
                    if (mirror_x != nullptr) {
                        *mirror_x = test_mirror_x != 0;
                    }
                    if (mirror_y != nullptr) {
                        *mirror_y = test_mirror_y != 0;
                    }
                    return;
                }
            }
        }
    }

    if (swap_xy != nullptr) {
        *swap_xy = s_base_swap_xy;
    }
    if (mirror_x != nullptr) {
        *mirror_x = s_base_mirror_x;
    }
    if (mirror_y != nullptr) {
        *mirror_y = s_base_mirror_y;
    }
}

float GravityMagnitude(int16_t ax, int16_t ay, int16_t az) {
    return std::sqrtf(static_cast<float>(ax) * ax + static_cast<float>(ay) * ay +
                      static_cast<float>(az) * az);
}

// 屏幕大致水平（像章平放）：|az|/|g| 较大，绕屏法向转要用陀螺仪。
bool IsNearlyFlat(int16_t ax, int16_t ay, int16_t az) {
    const float g = GravityMagnitude(ax, ay, az);
    if (g < 3000.0f) {
        return false;
    }
    return std::fabsf(static_cast<float>(az)) / g > 0.72f;
}

esp_lv_adapter_rotation_t RotationFromTilt(int16_t ax, int16_t ay, bool* valid) {
    if (valid != nullptr) {
        *valid = false;
    }
#if defined(DISPLAY_ORIENT_INVERT_X)
    ax = static_cast<int16_t>(-ax);
#endif
#if defined(DISPLAY_ORIENT_INVERT_Y)
    ay = static_cast<int16_t>(-ay);
#endif
#if defined(DISPLAY_ORIENT_SWAP_XY)
    const int16_t tmp = ax;
    ax = ay;
    ay = tmp;
#endif

    const int abs_x = std::abs(static_cast<int>(ax));
    const int abs_y = std::abs(static_cast<int>(ay));
    constexpr int kMinInPlane = 2800;
    if (abs_x < kMinInPlane && abs_y < kMinInPlane) {
        return s_current;
    }

    if (valid != nullptr) {
        *valid = true;
    }
    const float angle_deg =
        std::atan2f(static_cast<float>(ax), static_cast<float>(ay)) * (180.0f / 3.14159265f);
    if (angle_deg >= -45.0f && angle_deg < 45.0f) {
        return ESP_LV_ADAPTER_ROTATE_0;
    }
    if (angle_deg >= 45.0f && angle_deg < 135.0f) {
        return ESP_LV_ADAPTER_ROTATE_90;
    }
    if (angle_deg >= -135.0f && angle_deg < -45.0f) {
        return ESP_LV_ADAPTER_ROTATE_270;
    }
    return ESP_LV_ADAPTER_ROTATE_180;
}

esp_lv_adapter_rotation_t NextRotation90(esp_lv_adapter_rotation_t cur, bool clockwise) {
    const int deg = static_cast<int>(cur);
    const int next = clockwise ? (deg + 90) % 360 : (deg + 270) % 360;
    return static_cast<esp_lv_adapter_rotation_t>(next);
}

esp_lv_adapter_rotation_t ApplyOrientationOffset(esp_lv_adapter_rotation_t rotation) {
#if defined(DISPLAY_ORIENT_OFFSET_DEG) && DISPLAY_ORIENT_OFFSET_DEG != 0
    const int deg =
        (static_cast<int>(rotation) + DISPLAY_ORIENT_OFFSET_DEG) % 360;
    return static_cast<esp_lv_adapter_rotation_t>(deg);
#else
    return rotation;
#endif
}

esp_lv_adapter_rotation_t DetectTargetRotation(const ImuSample& sample, int dt_ms) {
    if (IsNearlyFlat(sample.ax, sample.ay, sample.az)) {
        // 陀螺仪饱和时忽略，避免误触发连跳。
        constexpr int16_t kGyroSaturation = 30000;
        if (std::abs(static_cast<int>(sample.gz)) < kGyroSaturation) {
            s_gyro_z_accum += static_cast<int32_t>(sample.gz) * dt_ms;
        }
        constexpr int32_t kSpinAccumThreshold = 120000;
        if (s_gyro_z_accum > kSpinAccumThreshold) {
            s_gyro_z_accum = 0;
            return NextRotation90(s_current, true);
        }
        if (s_gyro_z_accum < -kSpinAccumThreshold) {
            s_gyro_z_accum = 0;
            return NextRotation90(s_current, false);
        }
        return s_current;
    }

    s_gyro_z_accum = 0;
    bool tilt_valid = false;
    const esp_lv_adapter_rotation_t tilt =
        RotationFromTilt(sample.ax, sample.ay, &tilt_valid);
    if (!tilt_valid) {
        return s_current;
    }
    return ApplyOrientationOffset(tilt);
}

}  // namespace

void DisplayOrientationInit(esp_lcd_panel_handle_t panel, esp_lcd_touch_handle_t touch,
                            bool base_swap_xy, bool base_mirror_x, bool base_mirror_y) {
    s_panel = panel;
    s_touch = touch;
    s_base_swap_xy = base_swap_xy;
    s_base_mirror_x = base_mirror_x;
    s_base_mirror_y = base_mirror_y;
    s_current = ESP_LV_ADAPTER_ROTATE_0;
    s_candidate = ESP_LV_ADAPTER_ROTATE_0;
    s_raw_detected = ESP_LV_ADAPTER_ROTATE_0;
    s_stable_count = 0;
    s_gyro_z_accum = 0;
    s_last_log_us = 0;
}

esp_lv_adapter_rotation_t DisplayOrientationGet() { return s_current; }

esp_err_t DisplayOrientationApply(esp_lv_adapter_rotation_t rotation) {
    if (s_panel == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (rotation == s_current) {
        return ESP_OK;
    }

    bool swap_xy = false;
    bool mirror_x = false;
    bool mirror_y = false;
    GetCombinedOrientationFlags(rotation, &swap_xy, &mirror_x, &mirror_y);

    esp_err_t err = esp_lcd_panel_swap_xy(s_panel, swap_xy);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel swap_xy failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_lcd_panel_mirror(s_panel, mirror_x, mirror_y);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel mirror failed: %s", esp_err_to_name(err));
        return err;
    }

    if (s_touch != nullptr) {
        esp_lcd_touch_set_swap_xy(s_touch, swap_xy);
        esp_lcd_touch_set_mirror_x(s_touch, mirror_x);
        esp_lcd_touch_set_mirror_y(s_touch, mirror_y);
    }

    s_current = rotation;
    s_candidate = rotation;
    s_stable_count = 0;
    ESP_LOGI(TAG, "rotation=%d swap=%d mx=%d my=%d", static_cast<int>(rotation),
             swap_xy ? 1 : 0, mirror_x ? 1 : 0, mirror_y ? 1 : 0);

    lv_obj_t* scr = lv_screen_active();
    if (scr != nullptr) {
        // 关背光后再条带刷，避免旋转时整页从上往下扫出来。
        DisplayRefreshBlank blank;
        lv_obj_invalidate(scr);
        lv_refr_now(lv_display_get_default());
    }
    return ESP_OK;
}

esp_lv_adapter_rotation_t DisplayOrientationUpdate(const ImuSample& sample, int dt_ms) {
    if (!sample.ok || dt_ms <= 0) {
        return s_current;
    }

    s_raw_detected = DetectTargetRotation(sample, dt_ms);
    const esp_lv_adapter_rotation_t target = s_raw_detected;
    if (target == s_candidate) {
        ++s_stable_count;
    } else {
        s_candidate = target;
        s_stable_count = 1;
    }

    if (s_stable_count >= 3 && target != s_current) {
        s_stable_count = 0;
        return target;
    }
    return s_current;
}

void DisplayOrientationMaybeLog(const ImuSample& sample, esp_lv_adapter_rotation_t /*applied*/) {
#if !defined(DISPLAY_ORIENT_DEBUG) || DISPLAY_ORIENT_DEBUG
    const int64_t now = esp_timer_get_time();
    if (s_last_log_us != 0 && (now - s_last_log_us) < 2000000) {
        return;
    }
    s_last_log_us = now;
    const float g = GravityMagnitude(sample.ax, sample.ay, sample.az);
    const float tilt_z_pct =
        (g > 1.0f) ? (std::fabsf(static_cast<float>(sample.az)) * 100.0f / g) : 0.0f;
    // ESP_LOGI(TAG,
    //          "imu ax=%d ay=%d az=%d gz=%d flat=%d z%%=%.0f raw=%d cur=%d stable=%d accum=%ld",
    //          sample.ax, sample.ay, sample.az, sample.gz,
    //          IsNearlyFlat(sample.ax, sample.ay, sample.az) ? 1 : 0, tilt_z_pct,
    //          static_cast<int>(s_raw_detected), static_cast<int>(s_current), s_stable_count,
    //          static_cast<long>(s_gyro_z_accum));
#endif
}
