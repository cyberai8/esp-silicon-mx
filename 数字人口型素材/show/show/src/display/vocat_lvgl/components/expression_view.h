#pragma once

#include "expression_types.h"

#include <lvgl.h>
#include <cstdint>

namespace vocat {

/**
 * Pure-LVGL expression widget.
 * Pose table + optional blink transition (scheme §五 / §六).
 */
class ExpressionView {
public:
    void Create(lv_obj_t* parent, int size = 300);
    void Destroy();

    void SetState(ExpressionState state);
    /** Apply even when already in the same state (gesture temporary feedback). */
    void SetStateForced(ExpressionState state);
    void SetEmotion(const char* emotion);
    void SetEmotionForced(const char* emotion);
    void SetVisible(bool visible);
    void StopAnimation();
    /**
     * Stop blink/float/pulse but keep the last healed frame visible.
     * Used during standby drag — never hide the face for SPI reasons.
     */
    void FreezeCurrentPose();
    /** Speaking mouth envelope — SpeakingSmall/Medium/Large. */
    void SetMouth(MouthShape mouth);
    /**
     * Live chat: disable blink (CancelAnims mid-blink can freeze scale_y≈0).
     * Idle keeps blink on.
     */
    void SetBlinkEnabled(bool enabled);
    /** Structured visibility dump for SyncHomeEmotionPresentation. */
    void LogDiagnostics(const char* why) const;

    lv_obj_t* Root() const { return root_; }
    ExpressionState State() const { return state_; }
    bool IsCreated() const { return root_ != nullptr; }

private:
    void LayoutEyes();
    void ApplyPose(const ExpressionPose& pose);
    void ApplyVisual();
    void StartIdleAnims();
    void ScheduleBlink();
    void CancelAnims();
    /** After killing blink/pulse mid-flight, scale_y can stick near 0 → invisible eyes. */
    void ResetEyeTransforms();
    void SetArcEye(lv_obj_t* arc, int16_t start, int16_t end, int16_t rot_01deg, int width,
                   int span_w);
    void SetMouthArc(int16_t start, int16_t end, int w, int h, int line_w);
    void BeginTransition(ExpressionState target);

    static void OnBlinkTimer(lv_timer_t* t);
    static void OnFloatAnim(void* var, int32_t v);
    static void OnBlinkAnim(void* var, int32_t v);
    static void OnBlinkAnimReady(lv_anim_t* a);
    static void OnSpeakPulse(void* var, int32_t v);
    static void OnTearAnim(void* var, int32_t v);
    static void OnLookAnim(lv_anim_t* anim, int32_t offset);
    static void OnSparkleAnim(void* var, int32_t v);
    static void OnTransitionAnim(void* var, int32_t v);
    static void OnTransitionReady(lv_anim_t* a);

    lv_obj_t* root_ = nullptr;
    lv_obj_t* left_arc_ = nullptr;
    lv_obj_t* right_arc_ = nullptr;
    lv_obj_t* left_ring_ = nullptr;
    lv_obj_t* right_ring_ = nullptr;
    lv_obj_t* left_pupil_ = nullptr;
    lv_obj_t* right_pupil_ = nullptr;
    lv_obj_t* left_blush_ = nullptr;
    lv_obj_t* right_blush_ = nullptr;
    lv_obj_t* left_tear_ = nullptr;
    lv_obj_t* right_tear_ = nullptr;
    lv_obj_t* sweat_ = nullptr;
    lv_obj_t* sparkle_l_ = nullptr;
    lv_obj_t* sparkle_r_ = nullptr;
    lv_obj_t* shade_l_ = nullptr;
    lv_obj_t* shade_r_ = nullptr;
    lv_obj_t* left_glint_ = nullptr;
    lv_obj_t* right_glint_ = nullptr;
    lv_obj_t* fx_mark_ = nullptr;
    lv_obj_t* mouth_ = nullptr;       // 圆嘴 / 说话口型（椭圆填充）
    lv_obj_t* mouth_arc_ = nullptr;   // U 形微笑 / 下弯（arc）
    lv_timer_t* blink_timer_ = nullptr;

    ExpressionState state_ = ExpressionState::Neutral;
    ExpressionState pending_state_ = ExpressionState::Neutral;
    ExpressionPose pose_{};
    MouthShape mouth_override_ = MouthShape::None;
    bool mouth_override_active_ = false;
    int size_ = 300;
    int rest_y_ = 0;
    int left_pupil_base_x_ = 0;
    int right_pupil_base_x_ = 0;
    bool blinking_ = false;
    bool use_open_eyes_ = false;
    bool transitioning_ = false;
    bool blink_enabled_ = true;
};

ExpressionState ExpressionStateFromEmotion(const char* emotion);
const char* ExpressionStateName(ExpressionState state);

}  // namespace vocat
