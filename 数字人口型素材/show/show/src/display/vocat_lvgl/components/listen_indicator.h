#pragma once

#include <lvgl.h>

#include <functional>

namespace vocat {

/**
 * Emote listen_anim analogue: compact equalizer under the status bar.
 * Shown together with the face during connecting/listening/speaking — not a page swap.
 */
class ListenIndicator {
public:
    static constexpr int kBarCount = 5;

    using TapCallback = std::function<void()>;

    void Create(lv_obj_t* parent);
    void Destroy();
    void SetActive(bool active);
    void SetTapCallback(TapCallback cb);
    bool IsCreated() const { return root_ != nullptr; }
    lv_obj_t* Root() const { return root_; }

private:
    void StartWave();
    void StopWave();
    static void OnBarHeight(void* var, int32_t v);
    static void OnRootClicked(lv_event_t* e);

    lv_obj_t* root_ = nullptr;
    lv_obj_t* bars_[kBarCount] = {};
    bool active_ = false;
    TapCallback on_tap_;
};

}  // namespace vocat
