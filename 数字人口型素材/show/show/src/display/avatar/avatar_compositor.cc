#include "avatar_compositor.h"

#include "assets.h"
#include "display/lipsync/viseme_types.h"

#include <esp_log.h>

#include <cstring>
#include <initializer_list>

#define TAG "AvatarComp"

namespace vocat {

namespace {

lv_obj_t* MakeImg(lv_obj_t* parent, int x, int y)
{
    lv_obj_t* img = lv_image_create(parent);
    lv_obj_align(img, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    return img;
}

void SetImgSrc(lv_obj_t* img, const lv_img_dsc_t* dsc, int x, int y)
{
    if (img == nullptr || dsc == nullptr) {
        return;
    }
    lv_image_set_src(img, dsc);
    lv_obj_align(img, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_HIDDEN);
}

}  // namespace

bool AvatarCompositor::LoadAssetPng(const char* filename, Slot* out, const char* tag)
{
    if (out == nullptr || filename == nullptr) {
        return false;
    }
    void* ptr = nullptr;
    size_t size = 0;
    if (!Assets::GetInstance().GetAssetData(filename, ptr, size) || ptr == nullptr || size <= 8) {
        ESP_LOGW(TAG, "asset missing: %s (%s)", filename, tag ? tag : "?");
        return false;
    }
    // Assets mmap stays valid while partition is applied; point LVGL at flash directly.
    out->image = std::make_unique<LvglRawImage>(ptr, size);
    out->dsc = out->image->image_dsc();
    if (out->dsc == nullptr) {
        ESP_LOGW(TAG, "asset decode fail: %s", filename);
        return false;
    }
    return true;
}

bool AvatarCompositor::LoadAllAssets()
{
    if (!Assets::GetInstance().partition_valid()) {
        ESP_LOGW(TAG, "assets partition not ready");
        return false;
    }

#define LOAD(file, slot)                                     \
    do {                                                     \
        if (!LoadAssetPng(#file ".png", &(slot), #file)) {   \
            return false;                                    \
        }                                                    \
    } while (0)

    LOAD(base_360, base_);

    LOAD(upper_neutral, uppers_[static_cast<int>(FaceKind::Neutral)]);
    LOAD(upper_happy, uppers_[static_cast<int>(FaceKind::Happy)]);
    LOAD(upper_sad, uppers_[static_cast<int>(FaceKind::Sad)]);
    LOAD(upper_angry, uppers_[static_cast<int>(FaceKind::Angry)]);
    LOAD(upper_surprised, uppers_[static_cast<int>(FaceKind::Surprised)]);
    LOAD(upper_sleepy, uppers_[static_cast<int>(FaceKind::Sleepy)]);
    LOAD(upper_thinking, uppers_[static_cast<int>(FaceKind::Thinking)]);
    LOAD(upper_focused, uppers_[static_cast<int>(FaceKind::Focused)]);
    LOAD(upper_playful, uppers_[static_cast<int>(FaceKind::Playful)]);
    LOAD(upper_shy, uppers_[static_cast<int>(FaceKind::Shy)]);
    LOAD(upper_crying, uppers_[static_cast<int>(FaceKind::Crying)]);
    LOAD(upper_silly, uppers_[static_cast<int>(FaceKind::Silly)]);
    LOAD(upper_loving, uppers_[static_cast<int>(FaceKind::Loving)]);
    LOAD(upper_cool, uppers_[static_cast<int>(FaceKind::Cool)]);
    LOAD(upper_blink_35, upper_blink_35_);
    LOAD(upper_blink_80, upper_blink_80_);
    LOAD(upper_closed, upper_closed_);

    LOAD(mouth_neutral, emo_mouths_[static_cast<int>(FaceKind::Neutral)]);
    LOAD(mouth_happy, emo_mouths_[static_cast<int>(FaceKind::Happy)]);
    LOAD(mouth_sad, emo_mouths_[static_cast<int>(FaceKind::Sad)]);
    LOAD(mouth_angry, emo_mouths_[static_cast<int>(FaceKind::Angry)]);
    LOAD(mouth_surprised, emo_mouths_[static_cast<int>(FaceKind::Surprised)]);
    LOAD(mouth_sleepy, emo_mouths_[static_cast<int>(FaceKind::Sleepy)]);
    LOAD(mouth_thinking, emo_mouths_[static_cast<int>(FaceKind::Thinking)]);
    LOAD(mouth_focused, emo_mouths_[static_cast<int>(FaceKind::Focused)]);
    LOAD(mouth_playful, emo_mouths_[static_cast<int>(FaceKind::Playful)]);
    LOAD(mouth_shy, emo_mouths_[static_cast<int>(FaceKind::Shy)]);
    LOAD(mouth_crying, emo_mouths_[static_cast<int>(FaceKind::Crying)]);
    LOAD(mouth_silly, emo_mouths_[static_cast<int>(FaceKind::Silly)]);
    LOAD(mouth_loving, emo_mouths_[static_cast<int>(FaceKind::Loving)]);
    LOAD(mouth_cool, emo_mouths_[static_cast<int>(FaceKind::Cool)]);

    LOAD(viseme_00_SIL, visemes_[0]);
    LOAD(viseme_01_PP, visemes_[1]);
    LOAD(viseme_02_FF, visemes_[2]);
    LOAD(viseme_03_TH, visemes_[3]);
    LOAD(viseme_04_DD, visemes_[4]);
    LOAD(viseme_05_kk, visemes_[5]);
    LOAD(viseme_06_CH, visemes_[6]);
    LOAD(viseme_07_SS, visemes_[7]);
    LOAD(viseme_08_nn, visemes_[8]);
    LOAD(viseme_09_RR, visemes_[9]);
    LOAD(viseme_10_aa, visemes_[10]);
    LOAD(viseme_11_E, visemes_[11]);
    LOAD(viseme_12_I, visemes_[12]);
    LOAD(viseme_13_O, visemes_[13]);
    LOAD(viseme_14_U, visemes_[14]);
    LOAD(viseme_10_aa_small, viseme_aa_small_);
    LOAD(viseme_10_aa_open, viseme_aa_open_);

    LOAD(transition_SIL_to_aa, transitions_[static_cast<int>(TransitionEdge::SilToAa)]);
    LOAD(transition_aa_to_SIL, transitions_[static_cast<int>(TransitionEdge::AaToSil)]);
    LOAD(transition_PP_to_aa, transitions_[static_cast<int>(TransitionEdge::PpToAa)]);
    LOAD(transition_aa_to_PP, transitions_[static_cast<int>(TransitionEdge::AaToPp)]);
    LOAD(transition_I_to_aa, transitions_[static_cast<int>(TransitionEdge::IToAa)]);
    LOAD(transition_aa_to_I, transitions_[static_cast<int>(TransitionEdge::AaToI)]);
    LOAD(transition_SIL_to_O, transitions_[static_cast<int>(TransitionEdge::SilToO)]);
    LOAD(transition_O_to_SIL, transitions_[static_cast<int>(TransitionEdge::OToSil)]);

    LOAD(overlay_blush, overlay_blush_);
    LOAD(overlay_tear, overlay_tear_);
    LOAD(overlay_sweat, overlay_sweat_);
    LOAD(overlay_question, overlay_question_);
    LOAD(overlay_sparkle, overlay_sparkle_);
    LOAD(overlay_heart, overlay_heart_);

#undef LOAD
    return true;
}

const lv_img_dsc_t* AvatarCompositor::Upper(FaceKind k) const
{
    return uppers_[static_cast<int>(k)].dsc;
}

const lv_img_dsc_t* AvatarCompositor::EmoMouth(FaceKind k) const
{
    return emo_mouths_[static_cast<int>(k)].dsc;
}

AvatarCompositor::Pose AvatarCompositor::PoseForEmotion(const char* emotion)
{
    Pose p;
    const char* key = (emotion != nullptr && emotion[0] != '\0') ? emotion : "relaxed";

    auto is = [key](const char* a) { return strcmp(key, a) == 0; };
    auto one_of = [&](std::initializer_list<const char*> xs) {
        for (const char* x : xs) {
            if (is(x)) {
                return true;
            }
        }
        return false;
    };

    // V5 ships 9 face pairs, V5.1 adds shy / crying / silly / loving / cool.
    if (one_of({"happy", "laughing", "funny", "paishou"})) {
        p = {FaceKind::Happy, OverlayKind::None, false};
    } else if (one_of({"loving", "kissy", "love"})) {
        p = {FaceKind::Loving, OverlayKind::Heart, false};
    } else if (one_of({"embarrassed", "shy"})) {
        p = {FaceKind::Shy, OverlayKind::Blush, false};
    } else if (one_of({"crying", "cry"})) {
        p = {FaceKind::Crying, OverlayKind::Tear, false};
    } else if (one_of({"sad"})) {
        p = {FaceKind::Sad, OverlayKind::None, false};
    } else if (one_of({"angry"})) {
        p = {FaceKind::Angry, OverlayKind::None, false};
    } else if (one_of({"surprised", "shocked", "surprise", "insert"})) {
        p = {FaceKind::Surprised, OverlayKind::None, false};
    } else if (one_of({"thinking", "think", "question", "book"})) {
        p = {FaceKind::Thinking, OverlayKind::Question, true};
    } else if (one_of({"confused", "dizzy", "nauseated"})) {
        p = {FaceKind::Thinking, OverlayKind::Sweat, false};
    } else if (one_of({"silly", "playful"})) {
        p = {FaceKind::Silly, OverlayKind::None, false};
    } else if (one_of({"winking", "wink"})) {
        p = {FaceKind::Playful, OverlayKind::Sparkle, false};
    } else if (one_of({"delicious", "eat"})) {
        p = {FaceKind::Silly, OverlayKind::Sparkle, false};
    } else if (one_of({"sleepy", "sleep", "tired", "tried"})) {
        p = {FaceKind::Sleepy, OverlayKind::None, false};
    } else if (one_of({"cool", "confident"})) {
        p = {FaceKind::Cool, is("confident") ? OverlayKind::Sparkle : OverlayKind::None, false};
    } else if (one_of({"listening", "focused", "look_left", "look_right", "look_around"})) {
        // V5 没有独立左右看；用 focused 表达专注/侧目。
        p = {FaceKind::Focused, OverlayKind::None, false};
    } else {
        // idle / relaxed / neutral / speaking / default
        p = {FaceKind::Neutral, OverlayKind::None, false};
    }
    return p;
}

void AvatarCompositor::BuildLayers(lv_obj_t* parent)
{
    root_ = lv_obj_create(parent);
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, kCanvas, kCanvas);
    lv_obj_align(root_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    base_img_ = MakeImg(root_, 0, 0);
    lv_image_set_src(base_img_, base_.dsc);

    upper_img_ = MakeImg(root_, kUpperX, kUpperY);
    mouth_img_ = MakeImg(root_, kMouthX, kMouthY);
    overlay_img_ = MakeImg(root_, 0, 0);
    lv_obj_add_flag(overlay_img_, LV_OBJ_FLAG_HIDDEN);
}

void AvatarCompositor::ApplyMouthDsc(const lv_img_dsc_t* dsc)
{
    SetImgSrc(mouth_img_, dsc, kMouthX, kMouthY);
}

void AvatarCompositor::ApplyOverlay(OverlayKind kind, bool force_hide)
{
    if (overlay_img_ == nullptr) {
        return;
    }
    if (force_hide || kind == OverlayKind::None) {
        lv_obj_add_flag(overlay_img_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    const lv_img_dsc_t* dsc = nullptr;
    int x = 0;
    int y = 0;
    switch (kind) {
        case OverlayKind::Blush:
            dsc = overlay_blush_.dsc;
            x = 70;
            y = 184;
            break;
        case OverlayKind::Tear:
            dsc = overlay_tear_.dsc;
            x = 224;
            y = 179;
            break;
        case OverlayKind::Sweat:
            dsc = overlay_sweat_.dsc;
            x = 269;
            y = 92;
            break;
        case OverlayKind::Question:
            dsc = overlay_question_.dsc;
            x = 273;
            y = 63;
            break;
        case OverlayKind::Sparkle:
            dsc = overlay_sparkle_.dsc;
            x = 267;
            y = 139;
            break;
        case OverlayKind::Heart:
            dsc = overlay_heart_.dsc;
            x = 270;
            y = 125;
            break;
        default:
            break;
    }
    if (dsc == nullptr) {
        lv_obj_add_flag(overlay_img_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    SetImgSrc(overlay_img_, dsc, x, y);
}

void AvatarCompositor::ApplyPoseLayers()
{
    if (!ready_) {
        return;
    }
    // Keep the active emotion's eyes/brows while visemes drive only the mouth.
    const lv_img_dsc_t* upper = Upper(pose_.face);
    if (blink_phase_ == 1 || blink_phase_ == 5) {
        upper = upper_blink_35_.dsc;
    } else if (blink_phase_ == 2 || blink_phase_ == 4) {
        upper = upper_blink_80_.dsc;
    } else if (blink_phase_ == 3) {
        upper = upper_closed_.dsc;
    }
    SetImgSrc(upper_img_, upper, kUpperX, kUpperY);

    const bool hide_ov =
        speaking_ && pose_.hide_overlay_when_speaking;
    ApplyOverlay(pose_.overlay, hide_ov);

    if (!speaking_) {
        SetImgSrc(mouth_img_, EmoMouth(pose_.face), kMouthX, kMouthY);
        last_viseme_id_ = -1;
        last_fallback_level_ = -1;
    }
}

bool AvatarCompositor::Create(lv_obj_t* parent)
{
    Destroy();
    if (parent == nullptr) {
        return false;
    }
    if (!LoadAllAssets()) {
        Destroy();
        return false;
    }
    BuildLayers(parent);
    ready_ = true;
    speaking_ = false;
    pose_ = PoseForEmotion("relaxed");
    emotion_key_ = "relaxed";
    ApplyPoseLayers();
    StartBlinkTimer();
    ESP_LOGI(TAG, "AvatarCompositor ready (assets pack: V5.1 + V5.2 transitions + zh_15)");
    return true;
}

void AvatarCompositor::Destroy()
{
    StopBlinkTimer();
    ready_ = false;
    speaking_ = false;
    last_viseme_id_ = -1;
    last_fallback_level_ = -1;
    transition_active_ = false;
    transition_target_id_ = -1;
    transition_start_ms_ = 0;
    blink_phase_ = 0;
    if (root_ != nullptr) {
        lv_obj_delete(root_);
        root_ = nullptr;
    }
    base_img_ = upper_img_ = mouth_img_ = overlay_img_ = nullptr;
    base_ = {};
    for (auto& s : uppers_) {
        s = {};
    }
    upper_blink_35_ = {};
    upper_blink_80_ = {};
    upper_closed_ = {};
    for (auto& s : emo_mouths_) {
        s = {};
    }
    for (auto& s : visemes_) {
        s = {};
    }
    viseme_aa_small_ = {};
    viseme_aa_open_ = {};
    for (auto& s : transitions_) {
        s = {};
    }
    overlay_blush_ = {};
    overlay_tear_ = {};
    overlay_sweat_ = {};
    overlay_question_ = {};
    overlay_sparkle_ = {};
    overlay_heart_ = {};
}

void AvatarCompositor::SetVisible(bool visible)
{
    if (root_ == nullptr) {
        return;
    }
    if (visible) {
        lv_obj_clear_flag(root_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(root_);
    } else {
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    }
}

void AvatarCompositor::SetEmotion(const char* emotion)
{
    if (!ready_) {
        return;
    }
    const char* key = (emotion != nullptr && emotion[0] != '\0') ? emotion : "relaxed";
    // Keep the prior mood when the state machine sends speaking/neutral noise mid-utterance.
    // Cloud may send "relaxed" as a real content emotion; allow that through.
    if (speaking_ && (strcmp(key, "speaking") == 0 || strcmp(key, "neutral") == 0 ||
                      strcmp(key, "idle") == 0)) {
        return;
    }
    emotion_key_ = key;
    pose_ = PoseForEmotion(key);
    ApplyPoseLayers();
    ESP_LOGI(TAG, "emotion -> %s", key);
}

void AvatarCompositor::SetSpeaking(bool speaking)
{
    if (!ready_) {
        return;
    }
    if (speaking_ == speaking) {
        if (!speaking) {
            SetIdleMouth();
        }
        return;
    }
    speaking_ = speaking;
    if (speaking_) {
        transition_active_ = false;
        last_fallback_level_ = -1;
        ApplyPoseLayers();  // may hide question overlay
        ApplyMouthDsc(visemes_[0].dsc);
        last_viseme_id_ = lipsync::kSil;
    } else {
        transition_active_ = false;
        last_viseme_id_ = -1;
        last_fallback_level_ = -1;
        ApplyPoseLayers();
    }
}

const lv_img_dsc_t* AvatarCompositor::MouthDscForViseme(int viseme_id) const
{
    if (viseme_id < 0 || viseme_id > 14) {
        return visemes_[0].dsc;
    }
    if (viseme_id == lipsync::kAa) {
        // Prefer mid aa; open/small available if callers specialize later.
        return visemes_[10].dsc != nullptr ? visemes_[10].dsc : viseme_aa_small_.dsc;
    }
    return visemes_[viseme_id].dsc != nullptr ? visemes_[viseme_id].dsc : visemes_[0].dsc;
}

const lv_img_dsc_t* AvatarCompositor::TransitionDsc(int from_id, int to_id) const
{
    using lipsync::kAa;
    using lipsync::kI;
    using lipsync::kO;
    using lipsync::kPp;
    using lipsync::kSil;

    TransitionEdge edge = TransitionEdge::Count;
    if (from_id == kSil && to_id == kAa) {
        edge = TransitionEdge::SilToAa;
    } else if (from_id == kAa && to_id == kSil) {
        edge = TransitionEdge::AaToSil;
    } else if (from_id == kPp && to_id == kAa) {
        edge = TransitionEdge::PpToAa;
    } else if (from_id == kAa && to_id == kPp) {
        edge = TransitionEdge::AaToPp;
    } else if (from_id == kI && to_id == kAa) {
        edge = TransitionEdge::IToAa;
    } else if (from_id == kAa && to_id == kI) {
        edge = TransitionEdge::AaToI;
    } else if (from_id == kSil && to_id == kO) {
        edge = TransitionEdge::SilToO;
    } else if (from_id == kO && to_id == kSil) {
        edge = TransitionEdge::OToSil;
    } else {
        return nullptr;
    }
    return transitions_[static_cast<int>(edge)].dsc;
}

void AvatarCompositor::SetVisemeId(int viseme_id)
{
    ApplyViseme(viseme_id, true);
}

void AvatarCompositor::SetVisemeIdDirect(int viseme_id)
{
    ApplyViseme(viseme_id, false);
}

void AvatarCompositor::SetFallbackMouthLevel(int level)
{
    if (!ready_) {
        return;
    }
    if (level < 0) {
        level = 0;
    } else if (level > 3) {
        level = 3;
    }
    speaking_ = true;
    transition_active_ = false;
    if (level == last_fallback_level_) {
        return;
    }
    last_fallback_level_ = level;

    const lv_img_dsc_t* dsc = visemes_[0].dsc;
    int vid = lipsync::kSil;
    if (level == 1) {
        dsc = viseme_aa_small_.dsc != nullptr ? viseme_aa_small_.dsc : MouthDscForViseme(lipsync::kAa);
        vid = lipsync::kAa;
    } else if (level == 2) {
        dsc = MouthDscForViseme(lipsync::kAa);
        vid = lipsync::kAa;
    } else if (level >= 3) {
        dsc = viseme_aa_open_.dsc != nullptr ? viseme_aa_open_.dsc : MouthDscForViseme(lipsync::kAa);
        vid = lipsync::kAa;
    }
    last_viseme_id_ = vid;
    ApplyMouthDsc(dsc);
    ApplyOverlay(pose_.overlay, pose_.hide_overlay_when_speaking);
}

void AvatarCompositor::ApplyViseme(int viseme_id, bool allow_transition)
{
    if (!ready_) {
        return;
    }
    speaking_ = true;
    last_fallback_level_ = -1;

    if (!allow_transition && transition_active_) {
        transition_active_ = false;
    }

    if (transition_active_) {
        if (viseme_id == transition_target_id_) {
            if (lv_tick_elaps(transition_start_ms_) >= kTransitionHoldMs) {
                transition_active_ = false;
                last_viseme_id_ = transition_target_id_;
                ApplyMouthDsc(MouthDscForViseme(last_viseme_id_));
                ApplyOverlay(pose_.overlay, pose_.hide_overlay_when_speaking);
            }
            return;
        }
        // New viseme while a 32ms blend is showing — jump, don't freeze the mouth.
        transition_active_ = false;
    }

    if (viseme_id == last_viseme_id_) {
        return;
    }

    const lv_img_dsc_t* via = nullptr;
    if (allow_transition && last_viseme_id_ >= 0) {
        via = TransitionDsc(last_viseme_id_, viseme_id);
    }
    if (via != nullptr) {
        transition_active_ = true;
        transition_target_id_ = viseme_id;
        transition_start_ms_ = lv_tick_get();
        ApplyMouthDsc(via);
        ApplyOverlay(pose_.overlay, pose_.hide_overlay_when_speaking);
        return;
    }

    last_viseme_id_ = viseme_id;
    ApplyMouthDsc(MouthDscForViseme(viseme_id));
    ApplyOverlay(pose_.overlay, pose_.hide_overlay_when_speaking);
}

void AvatarCompositor::SetIdleMouth()
{
    if (!ready_) {
        return;
    }
    speaking_ = false;
    last_viseme_id_ = -1;
    last_fallback_level_ = -1;
    transition_active_ = false;
    ApplyPoseLayers();
}

void AvatarCompositor::StartBlinkTimer()
{
    StopBlinkTimer();
    blink_idle_ticks_ = 0;
    blink_phase_ = 0;
    // 40ms tick: idle ~2.0–3.2s then ~200ms blink.
    blink_timer_ = lv_timer_create(OnBlinkTimer, 40, this);
}

void AvatarCompositor::StopBlinkTimer()
{
    if (blink_timer_ != nullptr) {
        lv_timer_delete(blink_timer_);
        blink_timer_ = nullptr;
    }
}

void AvatarCompositor::OnBlinkTimer(lv_timer_t* t)
{
    auto* self = static_cast<AvatarCompositor*>(lv_timer_get_user_data(t));
    if (self != nullptr) {
        self->TickBlink();
    }
}

void AvatarCompositor::TickBlink()
{
    if (!ready_ || root_ == nullptr || lv_obj_has_flag(root_, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    if (blink_phase_ == 0) {
        blink_idle_ticks_++;
        // ~2.0–3.2s, with a light variation by face state (more lively than real-human 3–5s).
        const uint32_t wait = 50 + (static_cast<uint32_t>(pose_.face) % 5) * 8;
        if (blink_idle_ticks_ < wait) {
            return;
        }
        blink_idle_ticks_ = 0;
        blink_phase_ = 1;
        ApplyPoseLayers();
        return;
    }

    // 35% -> 80% -> closed -> 80% -> 35% -> current upper face.
    blink_phase_ = (blink_phase_ >= 5) ? 0 : blink_phase_ + 1;
    ApplyPoseLayers();
}

}  // namespace vocat
