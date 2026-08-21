#include "listen_indicator.h"

#include <esp_log.h>

#define TAG "ListenInd"

namespace vocat {
namespace {

/** Compact wave under status bar — must not cover eye band (~y≥120). */
constexpr int kRootW = 120;
constexpr int kRootH = 28;
/** Status bar ends ≈58; keep a tight strip above the face. */
constexpr int kTopOffsetY = 52;
constexpr int kBarW = 5;
constexpr int kBarGap = 5;
constexpr int kBarMinH = 6;
constexpr int kBarMaxH = 22;

}  // namespace

void ListenIndicator::Create(lv_obj_t* parent)
{
    if (parent == nullptr || root_ != nullptr) {
        return;
    }

    root_ = lv_obj_create(parent);
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, kRootW, kRootH);
    lv_obj_align(root_, LV_ALIGN_TOP_MID, 0, kTopOffsetY);
    lv_obj_set_style_bg_opa(root_, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(root_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(root_, kBarGap, 0);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(root_, OnRootClicked, LV_EVENT_CLICKED, this);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < kBarCount; ++i) {
        bars_[i] = lv_obj_create(root_);
        lv_obj_remove_style_all(bars_[i]);
        lv_obj_set_size(bars_[i], kBarW, kBarMinH);
        lv_obj_set_style_radius(bars_[i], kBarW / 2, 0);
        lv_obj_set_style_bg_color(bars_[i], lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(bars_[i], LV_OPA_COVER, 0);
        // Parent root owns the tap; bars must not steal/block.
        lv_obj_clear_flag(bars_[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(bars_[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    ESP_LOGI(TAG, "listen indicator created (vertical bars, tap=pause)");
}

void ListenIndicator::Destroy()
{
    StopWave();
    root_ = nullptr;
    for (int i = 0; i < kBarCount; ++i) {
        bars_[i] = nullptr;
    }
    active_ = false;
    on_tap_ = nullptr;
}

void ListenIndicator::SetTapCallback(TapCallback cb)
{
    on_tap_ = std::move(cb);
}

void ListenIndicator::SetActive(bool active)
{
    if (root_ == nullptr) {
        return;
    }
    if (active_ == active) {
        if (active) {
            lv_obj_clear_flag(root_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_align(root_, LV_ALIGN_TOP_MID, 0, kTopOffsetY);
            // Caller owns z-order vs face; do not yank to absolute top here.
        }
        return;
    }
    active_ = active;
    if (active) {
        lv_obj_clear_flag(root_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(root_, LV_ALIGN_TOP_MID, 0, kTopOffsetY);
        StartWave();
        ESP_LOGI(TAG, "listen indicator ON");
    } else {
        StopWave();
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "listen indicator OFF");
    }
}

void ListenIndicator::StartWave()
{
    StopWave();
    static const int kPeak[kBarCount] = {14, 22, 18, 22, 14};
    static const int kDur[kBarCount] = {420, 520, 380, 560, 460};
    static const int kDelay[kBarCount] = {0, 80, 40, 120, 60};

    for (int i = 0; i < kBarCount; ++i) {
        if (bars_[i] == nullptr) {
            continue;
        }
        lv_obj_set_height(bars_[i], kBarMinH);

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, bars_[i]);
        lv_anim_set_values(&a, kBarMinH, kPeak[i]);
        lv_anim_set_duration(&a, kDur[i]);
        lv_anim_set_playback_duration(&a, kDur[i]);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_delay(&a, kDelay[i]);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&a, OnBarHeight);
        lv_anim_start(&a);
    }
    (void)kBarMaxH;
}

void ListenIndicator::StopWave()
{
    for (int i = 0; i < kBarCount; ++i) {
        if (bars_[i] != nullptr) {
            lv_anim_delete(bars_[i], OnBarHeight);
            lv_obj_set_height(bars_[i], kBarMinH);
        }
    }
}

void ListenIndicator::OnBarHeight(void* var, int32_t v)
{
    lv_obj_set_height(static_cast<lv_obj_t*>(var), v);
}

void ListenIndicator::OnRootClicked(lv_event_t* e)
{
    auto* self = static_cast<ListenIndicator*>(lv_event_get_user_data(e));
    if (self == nullptr || !self->active_) {
        return;
    }
    ESP_LOGI(TAG, "listen indicator tap");
    if (self->on_tap_) {
        self->on_tap_();
    }
}

}  // namespace vocat
