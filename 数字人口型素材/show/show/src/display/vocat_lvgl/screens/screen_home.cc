#include "vocat_lvgl_display.h"

#include "application.h"
#include "board.h"
#include "dictation_forward.h"
#include "expression_view.h"
#include "top_status_bar.h"
#include "vocat_styles.h"

#if CONFIG_VOCAT_USE_EMOTE_GEN_PLAYER
#include "emote_mapping.h"
#include "emote_service.h"
#endif

#include <cstring>
#include <esp_log.h>
#include <lvgl.h>
#include <string>

#define TAG "VocatHome"

namespace {

/** Transparent hit target over the mouth / lower face — sized for finger tap. */
constexpr int kMouthHitW = 180;
constexpr int kMouthHitH = 100;
constexpr int kMouthHitY = 196;
/** Face lives on home_layer_ (full round), not inside the hit box — avoids LVGL clip. */
constexpr int kFaceSize = 340;
/** Speak subtitle: fixed-width band under the status chrome, inside the round chord. */
constexpr int kSubtitleH = 30;
constexpr int kSubtitleTopY = 52;
constexpr int kSubtitleSideMargin = 16;

int HomeSubtitleWidth()
{
    const int top = vocat::MaxWidthAtY(kSubtitleTopY, kSubtitleSideMargin);
    const int bot = vocat::MaxWidthAtY(kSubtitleTopY + kSubtitleH - 1, kSubtitleSideMargin);
    const int w = top < bot ? top : bot;
    return w > 160 ? w : 160;
}

void ApplyHomeSubtitleBand(lv_obj_t* label)
{
    if (label == nullptr) {
        return;
    }
    lv_obj_set_size(label, HomeSubtitleWidth(), kSubtitleH);
    lv_obj_set_style_max_width(label, HomeSubtitleWidth(), 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, kSubtitleTopY);
}

/** Flatten to one line for horizontal scroll. */
std::string FlattenSubtitleLine(const char* content)
{
    if (content == nullptr || content[0] == '\0') {
        return {};
    }

    std::string out;
    out.reserve(strlen(content));
    bool previous_space = false;
    const auto* p = reinterpret_cast<const unsigned char*>(content);
    while (*p != '\0') {
        if (*p == '\r' || *p == '\n' || *p == '\t' || *p == ' ') {
            if (!previous_space && !out.empty()) {
                out.push_back(' ');
                previous_space = true;
            }
            ++p;
            continue;
        }
        size_t bytes = 1;
        if ((*p & 0xE0) == 0xC0) {
            bytes = 2;
        } else if ((*p & 0xF0) == 0xE0) {
            bytes = 3;
        } else if ((*p & 0xF8) == 0xF0) {
            bytes = 4;
        }
        size_t valid_bytes = 1;
        while (valid_bytes < bytes && p[valid_bytes] != '\0' &&
               (p[valid_bytes] & 0xC0) == 0x80) {
            ++valid_bytes;
        }
        out.append(reinterpret_cast<const char*>(p), valid_bytes);
        p += valid_bytes;
        previous_space = false;
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

void TrimLeadingSubtitleSpace(std::string* text)
{
    if (text == nullptr) {
        return;
    }
    while (!text->empty()) {
        const unsigned char c = static_cast<unsigned char>((*text)[0]);
        if (c == ' ' || c == '\t' || c == ',' || c == '，' || c == '。' || c == '、') {
            text->erase(text->begin());
            continue;
        }
        if (text->size() >= 3 && c == 0xEF && static_cast<unsigned char>((*text)[1]) == 0xBC &&
            static_cast<unsigned char>((*text)[2]) == 0x8C) {
            text->erase(0, 3);
            continue;
        }
        break;
    }
}

bool IsAssistantSubtitleRole(const char* role)
{
    return role != nullptr && strcmp(role, "assistant") == 0;
}

bool IsGenericNeutralEmotion(const char* emotion)
{
    // Only treat state-machine filler as ignorable during chat.
    // Cloud content emotion may intentionally be "relaxed"; that must update the face.
    if (emotion == nullptr || emotion[0] == '\0') {
        return true;
    }
    return strcmp(emotion, "neutral") == 0 || strcmp(emotion, "microchip_ai") == 0 ||
           strcmp(emotion, "idle") == 0;
}

const char* ChatFallbackEmotion(DeviceState state)
{
    return state == kDeviceStateSpeaking ? "speaking" : "listening";
}

#if CONFIG_VOCAT_USE_EMOTE_GEN_PLAYER
void PlayMappedEmote(const char* emotion)
{
    auto& emote = vocat::emote::EmoteService::Instance();
    if (!emote.IsReady()) {
        return;
    }
    const char* clip = vocat::emote::ResolveClipName(emotion);
    ESP_LOGI(TAG, "emotion '%s' -> clip '%s'", emotion != nullptr ? emotion : "", clip);
    emote.EnqueueShow();
    emote.EnqueuePlayNow(clip, vocat::emote::ClipShouldRepeat(clip));
}
#endif

}  // namespace

void VocatLvglDisplay::ApplyHomeSubtitleScrollStyle()
{
    if (home_subtitle_ == nullptr) {
        return;
    }
    static lv_anim_t scroll_anim;
    lv_anim_init(&scroll_anim);
    lv_anim_set_delay(&scroll_anim, 800);
    lv_anim_set_repeat_count(&scroll_anim, LV_ANIM_REPEAT_INFINITE);
    lv_obj_set_style_anim(home_subtitle_, &scroll_anim, LV_PART_MAIN);
    lv_obj_set_style_anim_duration(home_subtitle_, lv_anim_speed_clamped(60, 300, 60000),
                                  LV_PART_MAIN);
}

void VocatLvglDisplay::SetHomeSubtitle(const char* role, const char* content)
{
    if (home_subtitle_ == nullptr) {
        return;
    }
    const std::string flat = FlattenSubtitleLine(content);
    if (flat.empty()) {
        home_subtitle_last_full_.clear();
        lv_label_set_text(home_subtitle_, "");
        SyncHomeSubtitleVisibility(home_chat_visible_ && !IsRingMenuVisible() &&
                                   !AnyFeaturePageVisible());
        return;
    }

    std::string display = flat;
    if (IsAssistantSubtitleRole(role)) {
        if (!home_subtitle_last_full_.empty() && flat == home_subtitle_last_full_) {
            return;
        }
        if (!home_subtitle_last_full_.empty() && flat.size() > home_subtitle_last_full_.size() &&
            flat.compare(0, home_subtitle_last_full_.size(), home_subtitle_last_full_) == 0) {
            display = flat.substr(home_subtitle_last_full_.size());
            TrimLeadingSubtitleSpace(&display);
            if (display.empty()) {
                home_subtitle_last_full_ = flat;
                return;
            }
        }
        home_subtitle_last_full_ = flat;
    } else {
        home_subtitle_last_full_.clear();
    }

    lv_label_set_long_mode(home_subtitle_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    ApplyHomeSubtitleScrollStyle();
    lv_label_set_text(home_subtitle_, display.c_str());
    ApplyHomeSubtitleBand(home_subtitle_);
    SyncHomeSubtitleVisibility(home_chat_visible_ && !IsRingMenuVisible() &&
                               !AnyFeaturePageVisible());
}

bool VocatLvglDisplay::IsContentEmotion(const char* emotion)
{
    return !IsGenericNeutralEmotion(emotion);
}

void OnHomeStartChatEvent(lv_event_t* e)
{
    auto* self = static_cast<VocatLvglDisplay*>(lv_event_get_user_data(e));
    if (self != nullptr) {
        self->OnHomeStartChat();
    }
}

void VocatLvglDisplay::BuildCharacterHomeUi()
{
    lv_obj_set_style_bg_color(home_layer_, lv_color_hex(vocat::kColBg), 0);
    lv_obj_add_flag(home_layer_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    // Keep legacy emoji/GIF layer hidden — home uses pure LVGL ExpressionView.
    if (emoji_box_ != nullptr) {
        lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    }
    if (emoji_image_ != nullptr) {
        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    }
    if (emoji_label_ != nullptr) {
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
    // Character home uses home_subtitle_; never show inherited LcdDisplay bottom_bar_.
    if (bottom_bar_ != nullptr) {
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }

    // Face on home_layer_ so wide eyes are not clipped by the hit container.
    home_expression_.Create(home_layer_, kFaceSize);
    home_expr_ctrl_.Bind(&home_expression_);

    // Phase-1 PNG avatar (base + mouth). Prefer over geometry / emote_gen when embeds load.
    if (home_avatar_.Create(home_layer_)) {
        home_expr_ctrl_.BindAvatar(&home_avatar_);
        home_expression_.SetVisible(false);
        home_avatar_.SetVisible(true);
        home_avatar_.SetEmotion("relaxed");
        home_avatar_.SetIdleMouth();
        ESP_LOGI(TAG, "Home face: AvatarCompositor (full layered PNG pack)");
    } else {
        ESP_LOGW(TAG, "AvatarCompositor unavailable; keeping geometric ExpressionView");
    }

#if CONFIG_VOCAT_USE_EMOTE_GEN_PLAYER
    // Only use emote_gen when avatar PNG path is not active.
    if (!home_avatar_.IsReady()) {
        auto& emote = vocat::emote::EmoteService::Instance();
        if (emote.Init() == ESP_OK && emote.AttachLvglImage(home_layer_) == ESP_OK) {
            ESP_LOGI(TAG, "Home face: emote_gen_player (PSRAM→LVGL canvas)");
            home_expression_.SetVisible(false);
        } else {
            ESP_LOGW(TAG, "emote_gen unavailable; keeping geometric ExpressionView");
        }
    }
#endif

    home_character_hit_ = lv_obj_create(home_layer_);
    lv_obj_remove_style_all(home_character_hit_);
    lv_obj_set_size(home_character_hit_, kMouthHitW, kMouthHitH);
    lv_obj_align(home_character_hit_, LV_ALIGN_TOP_MID, 0, kMouthHitY);
    lv_obj_set_style_bg_opa(home_character_hit_, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(home_character_hit_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(home_character_hit_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    // The visual mouth is owned by ExpressionView; this larger transparent target makes it usable.
    lv_obj_add_event_cb(home_character_hit_, OnHomeStartChatEvent, LV_EVENT_CLICKED, this);

    home_subtitle_ = lv_label_create(home_layer_);
    lv_label_set_long_mode(home_subtitle_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(home_subtitle_, "");
    vocat::ApplyTextFont(home_subtitle_);
    lv_obj_set_style_text_color(home_subtitle_, lv_color_hex(vocat::kColTextPri), 0);
    lv_obj_set_style_text_align(home_subtitle_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_opa(home_subtitle_, LV_OPA_90, 0);
    lv_obj_set_style_bg_opa(home_subtitle_, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(home_subtitle_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(home_subtitle_, LV_OBJ_FLAG_HIDDEN);
    ApplyHomeSubtitleBand(home_subtitle_);
    ApplyHomeSubtitleScrollStyle();

    home_character_img_ = nullptr;
    home_emotion_label_ = nullptr;
    home_bg_glow_ = nullptr;
    home_avatar_ring_ = nullptr;
    home_avatar_disc_ = nullptr;
    home_bg_mid_ = nullptr;
    home_mic_btn_ = nullptr;
    home_mic_img_ = nullptr;
    home_mic_fallback_ = nullptr;
    home_mic_caption_ = nullptr;
    home_swipe_hint_ = nullptr;

    BindHomeIcons();
    UpdateHomeStatusBar();
}

void VocatLvglDisplay::SyncHomeSubtitleVisibility(bool show_home)
{
    if (home_subtitle_ == nullptr) {
        return;
    }
    const char* text = lv_label_get_text(home_subtitle_);
    const bool has_text = text != nullptr && text[0] != '\0';
    if (show_home && has_text) {
        lv_obj_clear_flag(home_subtitle_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(home_subtitle_);
        ApplyHomeSubtitleBand(home_subtitle_);
    } else {
        lv_obj_add_flag(home_subtitle_, LV_OBJ_FLAG_HIDDEN);
    }
}

void VocatLvglDisplay::BindHomeIcons()
{
    home_content_emotion_ = "relaxed";
    ApplyHomeEmotion("relaxed");
}

void VocatLvglDisplay::ApplyHomeEmotion(const char* emotion)
{
    const DeviceState state = Application::GetInstance().GetDeviceState();
    const bool in_chat = (state == kDeviceStateConnecting || state == kDeviceStateListening ||
                          state == kDeviceStateSpeaking);

    // State-machine neutral/relaxed is only a cue during a live conversation.
    // Keep the last LLM face; if we never got one, use listening/speaking.
    if (IsGenericNeutralEmotion(emotion) && in_chat) {
        const char* keep = home_content_emotion_.c_str();
        const char* shown = IsGenericNeutralEmotion(keep) ? ChatFallbackEmotion(state) : keep;
        home_expr_ctrl_.SetEmotion(shown);
#if CONFIG_VOCAT_USE_EMOTE_GEN_PLAYER
        PlayMappedEmote(shown);
#endif
        SyncHomeEmotionPresentation();
        return;
    }

    if (IsContentEmotion(emotion)) {
        home_content_emotion_ = emotion;
    } else if (state == kDeviceStateIdle) {
        home_content_emotion_ = "relaxed";
        emotion = "relaxed";
    }

    home_expr_ctrl_.SetEmotion(emotion);
#if CONFIG_VOCAT_USE_EMOTE_GEN_PLAYER
    PlayMappedEmote(emotion);
#endif
    SyncHomeEmotionPresentation();
}

void VocatLvglDisplay::SyncHomeEmotionPresentation()
{
    const DeviceState state = Application::GetInstance().GetDeviceState();
    const bool in_live_chat = (state == kDeviceStateConnecting || state == kDeviceStateListening ||
                               state == kDeviceStateSpeaking);

    // Face stays up for the whole connected session. The mouth itself replaces the old
    // listening-wave overlay and is also the only chat tap target.
    const bool face_allowed = home_chat_visible_ && !IsRingMenuVisible() && !AnyFeaturePageVisible() &&
                              !wifi_qr_page_visible_ && !ota_progress_visible_ &&
                              !success_screen_visible_ && !boot_animation_running_;
    // During standby drag, keep the frozen last frame visible (do not hide for SPI).
    const bool show_face = face_allowed && !standby_expression_lite_;
    const bool speaking = show_face && (state == kDeviceStateSpeaking);

    if (emoji_box_ != nullptr) {
        lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    }
    if (gif_controller_) {
        gif_controller_->Stop();
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }

    const bool use_avatar = home_avatar_.IsReady();
#if CONFIG_VOCAT_USE_EMOTE_GEN_PLAYER
    const bool use_gen =
        !use_avatar && vocat::emote::EmoteService::Instance().IsReady();
#else
    const bool use_gen = false;
#endif

    if (use_avatar) {
        if (standby_expression_lite_ && face_allowed) {
            return;
        }
        home_avatar_.SetVisible(show_face);
        home_expression_.SetVisible(false);
        if (!speaking && show_face) {
            home_avatar_.SetIdleMouth();
        }
    } else if (home_expression_.IsCreated()) {
        if (standby_expression_lite_ && face_allowed) {
            home_expression_.FreezeCurrentPose();
            return;
        }
        home_expression_.SetBlinkEnabled(!(show_face && in_live_chat));
        home_expression_.SetVisible(show_face && !use_gen);
#if CONFIG_VOCAT_USE_EMOTE_GEN_PLAYER
        if (use_gen) {
            if (show_face) {
                vocat::emote::EmoteService::Instance().EnqueueShow();
            } else {
                vocat::emote::EmoteService::Instance().EnqueueHide();
            }
        }
#endif
    }
    home_expr_ctrl_.SetSpeaking(speaking);
    if (show_face && !speaking && !home_expr_ctrl_.IsTemporaryActive()) {
        if (!use_avatar &&
            (state == kDeviceStateListening || state == kDeviceStateConnecting)) {
            home_expression_.SetMouth(vocat::MouthShape::SmallO);
        }
    }
    if (!show_face) {
        home_expr_ctrl_.ClearTemporary();
    }
    if (home_character_hit_ != nullptr) {
        if (show_face) {
            lv_obj_clear_flag(home_character_hit_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(home_character_hit_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (!show_face && home_subtitle_ != nullptr) {
        home_subtitle_last_full_.clear();
        lv_label_set_text(home_subtitle_, "");
    }

    // Minimal z-order inside home: face → mouth hit → subtitle; chrome remains topmost.
    if (show_face) {
#if CONFIG_VOCAT_USE_EMOTE_GEN_PLAYER
        if (auto* emote_img = vocat::emote::EmoteService::Instance().Image()) {
            lv_obj_move_foreground(emote_img);
        }
#endif
        if (home_avatar_.IsReady() && home_avatar_.Root() != nullptr) {
            lv_obj_move_foreground(home_avatar_.Root());
        } else if (home_expression_.Root() != nullptr) {
            lv_obj_move_foreground(home_expression_.Root());
        }
        if (home_character_hit_ != nullptr) {
            lv_obj_move_foreground(home_character_hit_);
        }
        SyncHomeSubtitleVisibility(true);
        if (home_subtitle_ != nullptr && !lv_obj_has_flag(home_subtitle_, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_move_foreground(home_subtitle_);
        }
        if (standby_chrome_.root != nullptr) {
            lv_obj_move_foreground(standby_chrome_.root);
        }
        if (standby_chrome_.dots_row != nullptr) {
            lv_obj_move_foreground(standby_chrome_.dots_row);
        }

        ESP_LOGD(TAG, "home face+mouth: face=1 state=%s emotion=%s",
                 DeviceStateMachine::GetStateName(state), home_content_emotion_.c_str());
    } else {
        SyncHomeSubtitleVisibility(false);
    }
}

void VocatLvglDisplay::UpdateHomeStatusBar()
{
    if (standby_chrome_.root == nullptr) {
        return;
    }
    if (IsUiMotionBusy()) {
        pending_status_bar_ = true;
        return;
    }

    vocat::UpdateTopStatusBar(standby_chrome_);
    vocat::SetTopStatusBarPage(standby_chrome_, CurrentStandbyIndex());
}

void VocatLvglDisplay::OnHomeStartChat()
{
    ESP_LOGI(TAG, "home mouth tap");
    auto& app = Application::GetInstance();
    const DeviceState state = app.GetDeviceState();

    // The face can already be visible while Wi-Fi/OTA/MQTT activation is still
    // completing. Do not enqueue a chat toggle that will be discarded because
    // protocol_ is null; give the user immediate, explicit feedback instead.
    if (!app.IsProtocolReady() ||
        state == kDeviceStateStarting ||
        state == kDeviceStateActivating ||
        state == kDeviceStateWifiConfiguring) {
        ESP_LOGI(TAG, "home chat blocked: network/protocol still initializing (state=%s)",
                 DeviceStateMachine::GetStateName(state));
        ShowNotification("网络初始化中，请稍候", 1800);
        return;
    }

    // Xiaozhi ToggleChat semantics:
    //   speaking  → interrupt TTS, stay in session (listening)
    //   listening / connecting → exit session → idle
    //   idle → start session
    if (state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "tap mouth -> interrupt speaking");
        app.ToggleChatState();
        return;
    }
    if (state == kDeviceStateListening || state == kDeviceStateConnecting) {
        ESP_LOGI(TAG, "tap mouth -> exit chat (was %s)", DeviceStateMachine::GetStateName(state));
        app.ToggleChatState();
        return;
    }

    if (AnyFeaturePageVisible() || IsRingMenuVisible()) {
        ESP_LOGI(TAG, "tap mouth: leaving feature/ring UI first");
        HideAllFeaturePages();
        HideRingMenuImmediate();
    }

    app.EndDictationHold(false);
    DictationForwardSetEnabled(false);

    EnterHomeChatFromRing(standby_carousel_enabled_);
    app.SetChatRouteMode("xiaozhi");
    app.SendChatRouteUpdate();

    // The mouth is an explicit conversation control, independent of wake-word settings.
    ESP_LOGI(TAG, "tap mouth -> start chat");
    app.ToggleChatState();
}
