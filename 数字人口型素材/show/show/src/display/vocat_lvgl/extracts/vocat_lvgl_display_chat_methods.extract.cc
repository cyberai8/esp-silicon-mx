// EXTRACT from voiceshow/main/display/vocat_lvgl/vocat_lvgl_display.cc
// Home 对话页：状态 / 字幕 / 表情入口（完整实现见原文件）

void VocatLvglDisplay::SetStatus(const char* status)
{
    LvglDisplay::SetStatus(status);
    // Idle clock refresh must not rebuild the face — that was a visible standby hitch
    // every ~10s (SetStatus("HH:MM") → SyncHomeEmotionPresentation + diag logs).
    const DeviceState state = Application::GetInstance().GetDeviceState();
    if (state == kDeviceStateIdle || state == kDeviceStateStarting) {
        return;
    }
    // Speaking does not always SetEmotion; sync listen mic on chat status transitions.
    DisplayLockGuard lock(this);
    SyncHomeEmotionPresentation();
}

void VocatLvglDisplay::ClearChatMessages()
{
    {
        DisplayLockGuard lock(this);
        if (home_subtitle_ != nullptr) {
            lv_label_set_text(home_subtitle_, "");
            lv_obj_add_flag(home_subtitle_, LV_OBJ_FLAG_HIDDEN);
            home_subtitle_last_full_.clear();
        }
        if (bottom_bar_ != nullptr) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    LcdDisplay::ClearChatMessages();
}

void VocatLvglDisplay::SetChatMessage(const char* role, const char* content)
{
    bool refresh_openclaw = false;
    bool refresh_telnet = false;
    {
        DisplayLockGuard lock(this);
        if (wifi_qr_page_visible_ || ota_progress_visible_ || success_screen_visible_ ||
            boot_animation_running_) {
            return;
        }
        // Character home: dedicated subtitle band — never fill LcdDisplay bottom_bar_.
        if (home_chat_visible_ && !IsRingMenuVisible() && !AnyFeaturePageVisible()) {
            if (role != nullptr && content != nullptr &&
                (strcmp(role, "assistant") == 0 || strcmp(role, "user") == 0)) {
                SetHomeSubtitle(role, content);
            }
            if (bottom_bar_ != nullptr) {
                lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (openclaw_page_visible_ && openclaw_result_text_ != nullptr && content != nullptr &&
            content[0] != '\0') {
            if (role != nullptr && (strcmp(role, "assistant") == 0 || strcmp(role, "system") == 0)) {
                openclaw_hint_text_ = content;
                refresh_openclaw = true;
            }
        }
        if (telnet_page_visible_ && content != nullptr && content[0] != '\0') {
            if (role != nullptr && strcmp(role, "user") == 0) {
                telnet_dictation_text_ = content;
                telnet_has_content_ = true;
                refresh_telnet = true;
            }
        }
        // Meeting live captions: Application STT handler calls AppendMeetingCaption(text, is_final).
    }
    if (refresh_openclaw) {
        UpdateOpenClawPageUi(Application::GetInstance().GetDeviceState());
    }
    if (refresh_telnet) {
        UpdateTelnetPageUi(Application::GetInstance().GetDeviceState());
    }
}

void VocatLvglDisplay::SetEmotion(const char* emotion)
{
    if (wifi_qr_page_visible_ || ota_progress_visible_ || success_screen_visible_ ||
        boot_animation_running_) {
        return;
    }
    // Meeting page / hold: home face is off-screen; skip expression work under load.
    if (meeting_page_visible_ || Application::GetInstance().IsMeetingHoldActive()) {
        return;
    }
    DisplayLockGuard lock(this);
    ApplyHomeEmotion(emotion);
}

void VocatLvglDisplay::PlayTemporaryHomeEmotion(const char* emotion, uint32_t duration_ms)
{
    if (wifi_qr_page_visible_ || ota_progress_visible_ || success_screen_visible_ ||
        boot_animation_running_ || emotion == nullptr || emotion[0] == '\0') {
        return;
    }
    DisplayLockGuard lock(this);
    home_expr_ctrl_.PlayTemporary(emotion, duration_ms);
#if CONFIG_VOCAT_USE_EMOTE_GEN_PLAYER
    if (vocat::emote::EmoteService::Instance().IsReady()) {
        const char* clip = vocat::emote::ResolveClipName(emotion);
        ESP_LOGI("VocatLvgl", "temp emotion '%s' -> clip '%s'", emotion, clip);
        vocat::emote::EmoteService::Instance().EnqueueShow();
        vocat::emote::EmoteService::Instance().EnqueuePlayNow(
            clip, vocat::emote::ClipShouldRepeat(clip));
    }
#endif
    SyncHomeEmotionPresentation();
}
