#pragma once

#include "expression_view.h"

#include <lvgl.h>

#include <cstdint>
#include <string>

namespace vocat {

class AvatarCompositor;

/**
 * Scheme §八 / §六.3: priority, temporary emotions, speaking mouth envelope.
 * Optional AvatarCompositor: when ready + viseme timeline, drives PNG mouth.
 */
class ExpressionController {
public:
    void Bind(ExpressionView* view);
    void BindAvatar(AvatarCompositor* avatar);
    void Unbind();

    void SetEmotion(const char* emotion);
    void ForceRedraw();
    void PlayTemporary(const char* emotion, uint32_t duration_ms);
    void ClearTemporary();

    void SetSpeaking(bool speaking);
    void OnAudioLevel(uint8_t level);

    void StopAll();

    const char* DebugCycleNext();
    void DebugSetState(ExpressionState state);

    const std::string& BaseEmotion() const { return base_emotion_; }
    bool IsTemporaryActive() const { return temporary_active_; }
    bool IsSpeaking() const { return speaking_; }
    bool UsesAvatarMouth() const;

private:
    void ApplyToView(const char* emotion);
    void ApplyToViewForced(const char* emotion);
    void StartMouthTimer();
    void StopMouthTimer();
    void StopTempTimer();
    static void OnTempTimer(lv_timer_t* t);
    static void OnMouthTimer(lv_timer_t* t);
    void TickMouthFromPlayback();
    int NextFallbackLevel(uint16_t env);
    void ResetFallbackEnvelope();

    ExpressionView* view_ = nullptr;
    AvatarCompositor* avatar_ = nullptr;
    std::string base_emotion_{"relaxed"};
    bool temporary_active_ = false;
    bool speaking_ = false;
    uint8_t debug_index_ = 0;
    uint8_t mouth_phase_ = 0;
    lv_timer_t* temp_timer_ = nullptr;
    lv_timer_t* mouth_timer_ = nullptr;
    uint32_t mouth_period_ms_ = 0;
    float env_smooth_ = 0.0f;
    float env_peak_ = 0.0f;
    int fallback_level_ = 0;
    uint32_t fallback_hold_ms_ = 0;
    bool fallback_active_ = false;
};

}  // namespace vocat
