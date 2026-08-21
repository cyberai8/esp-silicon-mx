#include "expression_controller.h"
#include "expression_debug.h"

#include "application.h"
#include "audio_service.h"
#include "display/avatar/avatar_compositor.h"
#include "display/lipsync/lip_sync_controller.h"
#include "display/lipsync/viseme_types.h"

#include <esp_log.h>

#include <cstring>
#include <inttypes.h>

#define TAG "ExprCtrl"

namespace vocat {

namespace {

MouthShape VisemeIdToMouthShape(int viseme_id)
{
    switch (viseme_id) {
        case 0:
            return MouthShape::SpeakingSmall;
        case 1:
        case 2:
            return MouthShape::SpeakingSmall;
        case 10:
        case 13:
            return MouthShape::SpeakingLarge;
        default:
            return MouthShape::SpeakingMedium;
    }
}

}  // namespace

void ExpressionController::Bind(ExpressionView* view)
{
    view_ = view;
}

void ExpressionController::BindAvatar(AvatarCompositor* avatar)
{
    avatar_ = avatar;
}

void ExpressionController::Unbind()
{
    StopAll();
    view_ = nullptr;
    avatar_ = nullptr;
}

bool ExpressionController::UsesAvatarMouth() const
{
    return avatar_ != nullptr && avatar_->IsReady();
}

void ExpressionController::ApplyToView(const char* emotion)
{
    if (UsesAvatarMouth()) {
        avatar_->SetEmotion(emotion);
    }
    if (view_ == nullptr || !view_->IsCreated()) {
        return;
    }
    view_->SetEmotion(emotion);
}

void ExpressionController::ApplyToViewForced(const char* emotion)
{
    if (UsesAvatarMouth()) {
        avatar_->SetEmotion(emotion);
    }
    if (view_ == nullptr || !view_->IsCreated()) {
        return;
    }
    view_->SetEmotionForced(emotion);
}

void ExpressionController::SetEmotion(const char* emotion)
{
    const char* key = (emotion != nullptr && emotion[0] != '\0') ? emotion : "relaxed";
    base_emotion_ = key;
    if (temporary_active_) {
        ESP_LOGI(TAG, "base emotion queued under temp: %s", key);
        return;
    }
    ApplyToView(key);
}

void ExpressionController::ForceRedraw()
{
    const char* key = base_emotion_.empty() ? "relaxed" : base_emotion_.c_str();
    ApplyToViewForced(key);
}

void ExpressionController::PlayTemporary(const char* emotion, uint32_t duration_ms)
{
    if (view_ == nullptr || emotion == nullptr || emotion[0] == '\0' || duration_ms == 0) {
        return;
    }
    temporary_active_ = true;
    if (strcmp(emotion, "happy") == 0 || strcmp(emotion, "relaxed") == 0 ||
        strcmp(emotion, "laughing") == 0 || strcmp(emotion, "loving") == 0) {
        if (UsesAvatarMouth()) {
            avatar_->SetEmotion(emotion);
        }
        if (view_ != nullptr) {
            view_->SetStateForced(ExpressionState::Happy);
        }
        duration_ms = duration_ms < 1500 ? 1500 : (duration_ms > 2500 ? 2500 : duration_ms);
        ESP_LOGI(TAG, "temporary emotion %s (Happy) for %" PRIu32 " ms", emotion, duration_ms);
    } else {
        ApplyToViewForced(emotion);
        ESP_LOGI(TAG, "temporary emotion %s for %" PRIu32 " ms", emotion, duration_ms);
    }
    StopTempTimer();
    temp_timer_ = lv_timer_create(OnTempTimer, duration_ms, this);
    lv_timer_set_repeat_count(temp_timer_, 1);
}

void ExpressionController::ClearTemporary()
{
    StopTempTimer();
    if (!temporary_active_) {
        return;
    }
    temporary_active_ = false;
    ESP_LOGI(TAG, "temporary cleared -> %s", base_emotion_.c_str());
    ApplyToViewForced(base_emotion_.c_str());
}

void ExpressionController::OnTempTimer(lv_timer_t* t)
{
    auto* self = static_cast<ExpressionController*>(lv_timer_get_user_data(t));
    if (self == nullptr) {
        return;
    }
    self->temp_timer_ = nullptr;
    self->temporary_active_ = false;
    self->ApplyToViewForced(self->base_emotion_.c_str());
    ESP_LOGI(TAG, "temporary ended -> %s", self->base_emotion_.c_str());
}

void ExpressionController::SetSpeaking(bool speaking)
{
    if (speaking_ == speaking) {
        if (speaking) {
            StartMouthTimer();
        }
        return;
    }
    speaking_ = speaking;
    ResetFallbackEnvelope();
    if (speaking_) {
        mouth_phase_ = 0;
        StartMouthTimer();
        if (UsesAvatarMouth()) {
            avatar_->SetSpeaking(true);
        } else if (view_ != nullptr) {
            view_->SetMouth(MouthShape::SpeakingSmall);
        }
    } else {
        mouth_phase_ = 0;
        StopMouthTimer();
        if (UsesAvatarMouth()) {
            avatar_->SetSpeaking(false);
        } else if (view_ != nullptr) {
            view_->SetMouth(MouthShape::None);
        }
    }
}

void ExpressionController::OnAudioLevel(uint8_t level)
{
    if (!speaking_ || UsesAvatarMouth() || view_ == nullptr) {
        return;
    }
    if (level < 25) {
        view_->SetMouth(MouthShape::SpeakingSmall);
    } else if (level < 70) {
        view_->SetMouth(MouthShape::SpeakingMedium);
    } else {
        view_->SetMouth(MouthShape::SpeakingLarge);
    }
}

void ExpressionController::StartMouthTimer()
{
    // 120ms is still under one syllable, so the geometry face can follow the
    // loudness envelope instead of opening on a fixed cadence.
    const uint32_t period_ms = UsesAvatarMouth() ? 40 : 120;
    mouth_period_ms_ = period_ms;
    if (mouth_timer_ != nullptr) {
        lv_timer_set_period(mouth_timer_, period_ms);
        return;
    }
    mouth_timer_ = lv_timer_create(OnMouthTimer, period_ms, this);
}

void ExpressionController::ResetFallbackEnvelope()
{
    env_smooth_ = 0.0f;
    env_peak_ = 0.0f;
    fallback_level_ = 0;
    fallback_hold_ms_ = 0;
    fallback_active_ = false;
}

void ExpressionController::StopMouthTimer()
{
    if (mouth_timer_ != nullptr) {
        lv_timer_delete(mouth_timer_);
        mouth_timer_ = nullptr;
    }
}

void ExpressionController::StopTempTimer()
{
    if (temp_timer_ != nullptr) {
        lv_timer_delete(temp_timer_);
        temp_timer_ = nullptr;
    }
}

void ExpressionController::StopAll()
{
    StopTempTimer();
    StopMouthTimer();
    temporary_active_ = false;
    speaking_ = false;
    if (UsesAvatarMouth()) {
        avatar_->SetSpeaking(false);
    }
    if (view_ != nullptr) {
        view_->SetMouth(MouthShape::None);
        view_->StopAnimation();
    }
}

const char* ExpressionController::DebugCycleNext()
{
    debug_index_ = static_cast<uint8_t>((debug_index_ + 1) % kExpressionStateCount);
    const ExpressionState st = ExpressionStateFromIndex(debug_index_);
    DebugSetState(st);
    return ExpressionStateName(st);
}

void ExpressionController::DebugSetState(ExpressionState state)
{
    ClearTemporary();
    base_emotion_ = ExpressionStateName(state);
    if (UsesAvatarMouth()) {
        avatar_->SetEmotion(base_emotion_.c_str());
    }
    if (view_ == nullptr) {
        return;
    }
    view_->SetStateForced(state);
    ESP_LOGI(TAG, "debug expression -> %s", base_emotion_.c_str());
}

void ExpressionController::OnMouthTimer(lv_timer_t* t)
{
    auto* self = static_cast<ExpressionController*>(lv_timer_get_user_data(t));
    if (self != nullptr) {
        self->TickMouthFromPlayback();
    }
}

int ExpressionController::NextFallbackLevel(uint16_t env)
{
    const uint32_t period_ms = mouth_period_ms_ > 0 ? mouth_period_ms_ : 40;
    const float x = static_cast<float>(env);

    // Follow singing syllables; still slower on close so it does not chatter.
    const float attack = 0.55f;
    const float release = 0.30f;
    env_smooth_ += (x > env_smooth_ ? attack : release) * (x - env_smooth_);

    constexpr float kMinPeak = 900.0f;
    env_peak_ = x > env_peak_ ? x : env_peak_ * 0.988f;
    if (env_peak_ < kMinPeak) {
        env_peak_ = kMinPeak;
    }

    int want_level = 0;
    constexpr float kSilenceAbs = 80.0f;
    if (env_smooth_ >= kSilenceAbs) {
        const float ratio = env_smooth_ / env_peak_;
        if (ratio < 0.32f) {
            want_level = 1;
        } else if (ratio < 0.62f) {
            want_level = 2;
        } else {
            want_level = 3;
        }
        // Light hysteresis: stay put unless we clearly left the band.
        const int cur = fallback_level_;
        if (cur == 1 && ratio < 0.40f) {
            want_level = 1;
        } else if (cur == 2 && ratio >= 0.24f && ratio < 0.72f) {
            want_level = 2;
        } else if (cur == 3 && ratio >= 0.52f) {
            want_level = 3;
        }
    }

    fallback_hold_ms_ += period_ms;
    if (want_level == fallback_level_) {
        return fallback_level_;
    }
    // Catching up several bands: one step per tick (~40ms).
    // Adjacent chatter still waits one extra tick.
    const int gap = want_level > fallback_level_ ? want_level - fallback_level_
                                                 : fallback_level_ - want_level;
    const uint32_t need_ms = (gap >= 2) ? period_ms : 80;
    if (fallback_hold_ms_ < need_ms) {
        return fallback_level_;
    }
    fallback_level_ += (want_level > fallback_level_) ? 1 : -1;
    fallback_hold_ms_ = 0;
    return fallback_level_;
}

void ExpressionController::TickMouthFromPlayback()
{
    if (!speaking_) {
        return;
    }

    auto& audio = Application::GetInstance().GetAudioService();
    const uint16_t env = audio.GetPlaybackEnvelope();

    auto& lip_sync = Application::GetInstance().GetLipSyncController();
    if (lip_sync.HasTimeline()) {
        const int64_t played = audio.GetPlayedSamples();
        int viseme_id = lip_sync.Tick(played);
        // Axis says SIL but DAC is still putting out speech: either the
        // timeline has not arrived yet relative to audio, or an early DAC
        // anchor burned through the axis during the previous sentence's
        // tail. Prefer the loudness fake mouth over a frozen closed lip.
        constexpr uint16_t kSpeechEnvFloor = 80;
        if (viseme_id == lipsync::kSil && env >= kSpeechEnvFloor) {
            if (!fallback_active_) {
                ResetFallbackEnvelope();
                fallback_active_ = true;
            }
            const int level = NextFallbackLevel(env);
            if (UsesAvatarMouth()) {
                avatar_->SetFallbackMouthLevel(level);
                return;
            }
            if (view_ != nullptr) {
                static const MouthShape kLevelMouth[] = {
                    MouthShape::None,
                    MouthShape::SpeakingSmall,
                    MouthShape::SpeakingMedium,
                    MouthShape::SpeakingLarge,
                };
                view_->SetMouth(kLevelMouth[level]);
            }
            return;
        }
        fallback_active_ = false;
        if (UsesAvatarMouth()) {
            avatar_->SetVisemeId(viseme_id);
            return;
        }
        if (view_ != nullptr) {
            view_->SetMouth(VisemeIdToMouthShape(viseme_id));
        }
        return;
    }

    // No viseme axis (AI singing) or it has not arrived yet: follow the PCM
    // loudness so the mouth stays locked to what is actually being played.
    if (!fallback_active_) {
        ResetFallbackEnvelope();
        fallback_active_ = true;
    }
    const int level = NextFallbackLevel(env);

    if (UsesAvatarMouth()) {
        avatar_->SetFallbackMouthLevel(level);
        return;
    }
    if (view_ == nullptr) {
        return;
    }
    static const MouthShape kLevelMouth[] = {
        MouthShape::None,
        MouthShape::SpeakingSmall,
        MouthShape::SpeakingMedium,
        MouthShape::SpeakingLarge,
    };
    view_->SetMouth(kLevelMouth[level]);
}

}  // namespace vocat
