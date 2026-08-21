#pragma once

#include "lvgl_display/lvgl_image.h"

#include <lvgl.h>

#include <cstdint>
#include <memory>
#include <string>

namespace vocat {

/**
 * V5 / V5.1 full-layer 360×360 home face:
 *   base → upper_face → mouth (viseme | emotion) → overlay
 * upper_face and mouth are opaque, adjacent at Y=209/210.
 */
class AvatarCompositor {
public:
    bool Create(lv_obj_t* parent);
    void Destroy();

    void SetVisible(bool visible);
    /** Cloud / UI emotion key → upper face + idle mouth + overlay. */
    void SetEmotion(const char* emotion);
    void SetSpeaking(bool speaking);
    void SetVisemeId(int viseme_id);
    /** Energy fallback (singing / late axis): never insert via frames. */
    void SetVisemeIdDirect(int viseme_id);
    /** Singing/envelope mouth: 0=SIL, 1=aa_small, 2=aa, 3=aa_open. */
    void SetFallbackMouthLevel(int level);
    /** Not speaking: show current emotion mouth (not forced SIL). */
    void SetIdleMouth();

    bool IsReady() const { return ready_; }
    bool IsCreated() const { return root_ != nullptr; }
    lv_obj_t* Root() const { return root_; }

private:
    struct Slot {
        std::unique_ptr<LvglRawImage> image;
        const lv_img_dsc_t* dsc = nullptr;
    };

    enum class FaceKind : uint8_t {
        Neutral,
        Happy,
        Sad,
        Angry,
        Surprised,
        Sleepy,
        Thinking,
        Focused,
        Playful,
        // V5.1 extension pack.
        Shy,
        Crying,
        Silly,
        Loving,
        Cool,
        Count
    };
    enum class OverlayKind : uint8_t {
        None = 0,
        Blush,
        Tear,
        Sweat,
        Question,
        Sparkle,
        Heart,
    };

    struct Pose {
        FaceKind face = FaceKind::Neutral;
        OverlayKind overlay = OverlayKind::None;
        bool hide_overlay_when_speaking = false;
    };

    bool LoadAssetPng(const char* filename, Slot* out, const char* tag);
    bool LoadAllAssets();
    void BuildLayers(lv_obj_t* parent);
    void ApplyPoseLayers();
    void ApplyMouthDsc(const lv_img_dsc_t* dsc);
    void ApplyOverlay(OverlayKind kind, bool force_hide);
    const lv_img_dsc_t* MouthDscForViseme(int viseme_id) const;
    const lv_img_dsc_t* TransitionDsc(int from_id, int to_id) const;
    void ApplyViseme(int viseme_id, bool allow_transition);
    const lv_img_dsc_t* Upper(FaceKind k) const;
    const lv_img_dsc_t* EmoMouth(FaceKind k) const;
    static Pose PoseForEmotion(const char* emotion);
    void StartBlinkTimer();
    void StopBlinkTimer();
    static void OnBlinkTimer(lv_timer_t* t);
    void TickBlink();

    enum class TransitionEdge : uint8_t {
        SilToAa = 0,
        AaToSil,
        PpToAa,
        AaToPp,
        IToAa,
        AaToI,
        SilToO,
        OToSil,
        Count
    };

    lv_obj_t* root_ = nullptr;
    lv_obj_t* base_img_ = nullptr;
    lv_obj_t* upper_img_ = nullptr;
    lv_obj_t* mouth_img_ = nullptr;
    lv_obj_t* overlay_img_ = nullptr;

    Slot base_{};
    Slot uppers_[static_cast<int>(FaceKind::Count)]{};
    Slot upper_blink_35_{};
    Slot upper_blink_80_{};
    Slot upper_closed_{};
    Slot emo_mouths_[static_cast<int>(FaceKind::Count)]{};
    Slot visemes_[15]{};  // zh_15 ids 0..14
    Slot viseme_aa_small_{};
    Slot viseme_aa_open_{};
    Slot transitions_[static_cast<int>(TransitionEdge::Count)]{};
    Slot overlay_blush_{};
    Slot overlay_tear_{};
    Slot overlay_sweat_{};
    Slot overlay_question_{};
    Slot overlay_sparkle_{};
    Slot overlay_heart_{};

    bool ready_ = false;
    bool speaking_ = false;
    int last_viseme_id_ = -1;
    int last_fallback_level_ = -1;
    bool transition_active_ = false;
    int transition_target_id_ = -1;
    uint32_t transition_start_ms_ = 0;
    Pose pose_{};
    std::string emotion_key_{"relaxed"};
    uint8_t blink_phase_ = 0;  // 0 idle, 1 35%, 2 80%, 3 closed, 4 80%, 5 35%
    lv_timer_t* blink_timer_ = nullptr;
    uint32_t blink_idle_ticks_ = 0;

    // V5 install coordinates (360 canvas, top-left origin).
    static constexpr int kCanvas = 360;
    static constexpr int kUpperX = 70;
    static constexpr int kUpperY = 105;
    // V5 viseme and emotion mouths: 160×100 opaque @ (100,210).
    static constexpr int kMouthX = 100;
    static constexpr int kMouthY = 210;
    // V5.2 transition hold; one 40ms mouth tick is enough to kill pop.
    static constexpr uint32_t kTransitionHoldMs = 32;
};

}  // namespace vocat
