#pragma once

#include "lcd_display.h"
#include "vocat/vocat_display.h"
#include "vocat_gesture_router.h"
#include "vocat_ring_menu.h"
#include "top_status_bar.h"
#include "expression_view.h"
#include "expression_controller.h"
#include "display/avatar/avatar_compositor.h"
#include "standby_wipe_transition.h"
#include "boot_ui_state.h"
#include "lvgl_display/gif/lvgl_gif.h"
#include "lvgl_display/lvgl_image.h"

#include <esp_timer.h>

#include <memory>
#include <string>
#include <vector>

class VocatLvglDisplay : public SpiLcdDisplay, public vocat::IVocatDisplay {
public:
    VocatLvglDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                     int width, int height, int offset_x, int offset_y,
                     bool mirror_x, bool mirror_y, bool swap_xy);
    ~VocatLvglDisplay() override;

    void SetupUI() override;
    void SetStatus(const char* status) override;
    void SetChatMessage(const char* role, const char* content) override;
    void ClearChatMessages() override;
    void SetEmotion(const char* emotion) override;
    void PlayTemporaryHomeEmotion(const char* emotion, uint32_t duration_ms) override;

    bool IsRingMenuVisible() const override;
    void TriggerRingMenuFromBottom(bool animate_from_bottom = true) override;
    void HideRingMenu() override;
    void HideRingMenuImmediate() override;
    void ReturnToMainMenu() override;
    /** Unified feature back: nested step first, else ring menu. Button + left-edge share this. */
    bool HandleFeatureBack();
    bool TryConsumeFeatureBackGesture(int sdx, int sdy, int sx, int sy, int ex, int ey) override;
    void ForceShowRingMenuNow() override;
    void EnterHomeChatFromRing(bool enable_standby_carousel = true) override;
    void SetRingMenuPressedSegment(int segment) override;
    void ClearRingMenuPressedSegment() override;
    int HitTestRingMenu(int x, int y) override;
    bool HandleRingMenuPageSwipe(int sdx) override;
    bool HandleRingMenuPageDrag(int dx, int dy) override;
    bool FinishRingMenuPageDrag(int sdx) override;
    bool IsRingMenuPageGestureActive() const override;
    bool ShouldIgnoreHeadTouchForMenuUi() const override;
    bool IsHomeChatVisible() const override;
    void PresentBootInitializing() override;
    void MarkBootInitializingVisible() override;
    void SetBootInitializingText(const char* message) override;
    void SetBootUiState(uint8_t state) override;
    bool ShowBootAnimation() override;
    void CompleteBootSplash() override;
    void DismissBlockingBootOverlay() override;
    void NotifyBootSoundFinished() override;
    bool IsHourglassRunning() const override;
    bool HasEnabledAlarm() const override;
    bool IsSystemUiBlockingAutoPowerOff() const override;
    void ShowIdleAutoPowerOffWarning(int seconds_remaining) override;
    void HideIdleAutoPowerOffWarning() override;
    void ShowPowerOverlay(const char* message) override;
    void HidePowerOverlay() override;
    void RefreshAll() override;
    /** Re-apply theme text font to all VoCat widgets (call after Assets loads common CJK). */
    void RefreshFonts();
    void InvalidateFonts() { fonts_applied_ = nullptr; }

    void ShowSuccessScreen(const char* message) override;
    void ShowOtaProgress(int progress, size_t speed, const char* message = nullptr) override;
    void HideOtaProgress() override;
    void ShowQRCode(const char* url, const char* device_name = nullptr) override;
    void HideQRCode() override;
    void ShowBlufiProvisioningPage(const char* qr_url, const char* device_name = nullptr) override;
    void ShowSoftApProvisioningPage(const char* qr_url, const char* device_name,
                                    const char* ap_ssid, const char* web_url) override;
    void ReleaseWifiQrUi() override;
    /** Drop decoded boot brand bitmap after QR is on screen (BluFi RAM prep). */
    void ReleaseBootVisualResources() override;

    void ShowMusicPage(bool show) override;
    bool IsMusicPageVisible() const override;
    bool IsMusicPlaylistVisible() const override;
    bool IsMusicMoreMenuVisible() const override;
    bool IsMusicDownloadProgressVisible() const override;
    void SetMusicTracks(const std::vector<std::string>& tracks) override;
    bool TryStartLocalMusicPlayback() override;
    std::string GetCurrentMusicTrackName() const override;
    void SyncMusicPlayback(const std::string& song_name, int duration_seconds, bool is_playing) override;
    void SetMusicOpenedFromChat(bool from_chat) override;
    bool IsMusicOpenedFromChat() const override;
    void ReturnMusicToXiaozhiChat(bool start_listening = true) override;
    void SetMusicCoverRgb565(const std::string& song_title, uint8_t* rgb565, int size) override;
    void ToggleMusicPlaylist() override;
    void ToggleMusicMoreMenu() override;
    void MusicPrevious() override;
    void MusicNext() override;
    void MusicTogglePause() override;
    int MusicCycleVolumeLevel() override;
    bool HandleMusicPlaylistTouchRelease(int sdy, int sx, int sy, int ex, int ey) override;
    bool HandleMusicMoreMenuTouchRelease(int sx, int sy, int ex, int ey) override;
    bool HandleMusicDownloadProgressTouchRelease(int sx, int sy, int ex, int ey) override;

    void ShowPicturePage(bool show) override;
    bool IsPicturePageVisible() const override;
    bool HandlePictureTouchRelease(int sdx, int sdy, int sx, int sy, int ex, int ey) override;

    void ShowClockPage(bool show) override;
    bool IsClockPageVisible() const override;
    bool IsAlarmRinging() const override;
    void DismissAlarmRinging() override;
    bool AddAlarmAndShow(uint8_t hour, uint8_t minute, uint8_t weekday_mask = 0) override;
    void OpenAlarmApp() override;
    bool HandleClockTouchRelease(int sdx, int sdy, int sx, int sy, int ex, int ey) override;
    void HandleClockPickerFingerDown(int x, int y) override;
    bool HandleClockPickerFingerMove(int x, int y) override;

    void ShowHourglassPage(bool show) override;
    bool IsHourglassPageVisible() const override;
    bool IsHourglassSuspendedForRingMenu() const override;
    void LeaveHourglassForOtherRingModule() override;
    bool StartCountdownAndShow(int duration_seconds) override;
    void OpenCountdownApp() override;
    bool HandleHourglassTouchRelease(int sdx, int sdy, int sx, int sy, int ex, int ey) override;
    void HandleHourglassPickerFingerDown(int x, int y) override;
    bool HandleHourglassPickerFingerMove(int x, int y) override;

    bool LoadMusicLyricsFromFile(const std::string& lrc_path) override;
    void ClearMusicLyrics() override;
    void SetMusicLyricsFromTimestampedLines(
        const std::vector<std::pair<int, std::string>>& lines) override;
    bool InsertAnimDialog(const char* emoji_name, uint32_t duration_ms) override;
    bool StopAnimDialog() override;
    std::string DebugCycleHomeExpression() override;
    std::string DebugSetHomeExpression(const char* emotion) override;

    void ShowSettingsPage(bool show) override;
    bool IsSettingsPageVisible() const override;
    void HandleSettingsTouchPress(int x, int y) override;
    bool HandleSettingsTouchRelease(int sdx, int sdy, int sx, int sy, int ex, int ey) override;
    bool HandleSettingsTouchHold(int x, int y) override;
    bool IsSettingsSyncingSliders() const { return settings_syncing_sliders_; }

    void ShowIntimacyPage(bool show) override;
    bool IsIntimacyPageVisible() const override;
    void SetIntimacy(int value) override;
    void SetIntimacyTier(const char* tier) override;
    void SetIntimacyUpdatedAt(const char* iso_time) override;
    void SetPetEmotion(int show, const char* tier) override;
    void SetPetVitality(int value) override;
    void SetPetDeviceId(const char* device_id) override;
    bool HandleIntimacyTouchRelease(int sdx, int sdy, int sx, int sy, int ex, int ey) override;

    void ShowQuickSettings(bool show) override;
    bool IsQuickSettingsVisible() const override;
    bool AllowsQuickSettingsPullDown() const override;
    void HandleQuickSettingsTouchPress(int x, int y) override;
    bool HandleQuickSettingsTouchHold(int x, int y) override;
    bool HandleQuickSettingsTouchRelease(int sdx, int sdy, int sx, int sy, int ex, int ey) override;

    void ShowWeatherPage(bool show) override;
    bool IsWeatherPageVisible() const override;
    bool HandleStandbyHorizontalSwipe(int sdx) override;
    bool HandleStandbyDrag(int dx, int dy) override;
    bool FinishStandbyDrag(int sdx, int sdy) override;
    bool IsStandbyGestureActive() const override;
    bool IsHomeNavigationBlocked() const override;
    bool IsLiveVoiceChatActive() const override;
    bool IsStandbyIdleSurface() const override;
    void RefreshWeatherMock();
    void RequestWeatherRefresh();
    void RequestIntimacyRefresh();
    void ApplyWeatherData(const char* city, const char* wea, const char* wea_img, int temp,
                          int temp_high, int temp_low, int humidity, const char* air,
                          const char* alarm, const char* update_hhmm) override;
    bool HandleWeatherTouchRelease(int sdx, int sdy, int sx, int sy, int ex, int ey) override;

    void ShowFootballPage(bool show) override;
    bool IsFootballPageVisible() const override;
    bool HandleFootballTouchRelease(int sdx, int sdy, int sx, int sy, int ex, int ey) override;

    void ShowOpenClawPage(bool show, bool return_to_home = false) override;
    bool IsOpenClawPageVisible() const override;
    void RefreshOpenClawConversationUi(DeviceState state) override;
    void SetOpenClawBridgeStatus(bool connector_online, const char* pair_code) override;
    bool HandleOpenClawTouchRelease(int sdx, int sdy, int sx, int sy, int ex, int ey) override;

    void ShowTelnetDictationPage(bool show, bool return_to_home = false) override;
    bool IsTelnetDictationPageVisible() const override;
    void RefreshTelnetDictationTransmissionUi(DeviceState state) override;
    bool HandleTelnetDictationTouchRelease(int sdx, int sdy, int sx, int sy, int ex, int ey) override;

    void ShowMeetingPage(bool show, bool return_to_home = false) override;
    bool IsMeetingPageVisible() const override;
    void RefreshMeetingListFromServer();
    void AppendMeetingCaption(const char* text, bool is_final);
    /** Caller must hold DisplayLock. Committed lines + optional partial, scroll to bottom. */
    void FlushMeetingCaptionUiLocked(bool force);
    void OnMeetingHoldFailed(const char* message);
    /** Wi-Fi/MQTT drop while recording — keep session, stop live STT until reconnect. */
    void OnMeetingNetworkLost();
    /** Network back — resume meeting_live listen if still recording. */
    void OnMeetingNetworkRestored();
    bool HandleMeetingTouchRelease(int sdx, int sdy, int sx, int sy, int ex, int ey) override;

    void SetWifiConfigHint(const char* status, const char* hint) override;
    void ClearWifiConfigHint() override;
    bool IsWifiConfigHintActive() const override;
    bool IsWifiQrPageVisible() const override;

    /** Character-home mic / avatar entry → 赛搏对话 (xiaozhi route). */
    void OnHomeStartChat();

private:
    void BuildVocatUi();
    void BuildCharacterHomeUi();
    void BindHomeIcons();
    void ApplyHomeEmotion(const char* emotion);
    void SyncHomeEmotionPresentation();
    void SetHomeSubtitle(const char* role, const char* content);
    void ApplyHomeSubtitleScrollStyle();
    void SyncHomeSubtitleVisibility(bool show_home);
    /** True when emotion is a real content cue (happy/sad/…), not state-machine filler. */
    static bool IsContentEmotion(const char* emotion);
    void UpdateHomeStatusBar();
    void BuildStandbyChrome();
    void SyncStandbyChrome();
    void RaiseStandbyChrome();
    bool IsUiMotionBusy() const;
    void BeginUiMotion();
    void EndUiMotion();
    void SchedulePendingUiUpdates();
    void FlushPendingUiUpdates();
    static void OnPendingUiUpdatesAsync(void* user);
    void ShowHomeLayer(bool show);
    void HideAllFeaturePages();
    bool AnyFeaturePageVisible() const;
    void OnRingSegmentSelected(int segment);
    static void OnClockTimer(void* arg);
    void UpdateRingClock();

    void BuildSystemOverlaysUi();
    /** Character home + ring + standby — skipped at boot when no SSID (BluFi RAM). */
    void EnsureHomeUiBuiltUnderLock();
    void ShowProvisionOverlayUnderLock(const char* title, const char* device_line,
                                       const char* hint, bool show_qr, const char* qr_url);
    /** Boot splash widgets only (safe/cheap during EspVocat ctor). */
    void EnsureBootSplashUiUnderLock();
    void PrepareExclusiveSystemOverlayUnderLock();
    void FinishBootAnimationUnderLock();
    /** Tear down boot splash on lv_layer_top (image, flags, gesture gate). */
    void DismissBootSplashUnderLock();
    void CancelBootSplashMotionUnderLock();
    void BeginBootSplashUnderLock();
    /** Swap initializing text → full splash image; call after backlight is on. */
    void RevealBootBrandUnderLock();
    /** Re-bind boot brand after VocatIcons::Invalidate (OTA/assets apply). */
    void RebindBootBrandImageUnderLock();
    bool TryPrepareBootImageUnderLock();
    bool StartBootSplashGifUnderLock();
    void StopBootSplashGifUnderLock();
    /** Post-brand stages (network/OTA/activate): black + status text, no GIF replay. */
    void ShowBootStatusOverlayUnderLock(vocat::BootUiState state);
    void StartEarlyDotsAnimUnderLock();
    void StopEarlyDotsAnimUnderLock();
    void SetBootUiStateUnderLock(vocat::BootUiState state);
    void StartBootIntroAnimsUnderLock();
    void StartBootHoldTimerUnderLock();
    void StartBootOutroAnimUnderLock();
    void TryFinishBootToHomeUnderLock();
    static void OnBootSplashTimer(void* arg);
    static void OnBootIntroAnimReady(lv_anim_t* a);
    static void OnBootOutroAnimReady(lv_anim_t* a);
    static void BootSplashScaleExec(void* obj, int32_t v);
    static void BootSplashOpaExec(void* obj, int32_t v);

    // Page builders (implemented in screens/screen_*.cc)
    void BuildMusicPageUi();
    void BuildClockPageUi();
    void BuildHourglassPageUi();
    void BuildSettingsPageUi();
    void BuildOpenClawPageUi();
    void BuildTelnetPageUi();
    void BuildMeetingPageUi();
    void BuildIntimacyPageUi();
    void BuildWeatherPageUi();
    void BuildPicturePageUi();
    void BuildFootballPageUi();
    void BuildQuickSettingsUi();
    void ApplyQuickSettingsSlider(int which, int x);
    void OnQuickSettingsNetworkTap();
    void OnQuickSettingsWifiConfigLongPress();
    void OnQuickSettingsPowerSaveTap();
    void UpdateQuickSettingsUi();
    void UpdateMusicPageUi();
    lv_obj_t* MusicCoverSpinTarget() const;
    void SyncMusicCoverSpinUnderLock();
    void StopMusicCoverSpinUnderLock(bool reset_angle);
    static void MusicCoverSpinExec(void* obj, int32_t v);
    void UpdateClockPageUi();
    void UpdateHourglassPageUi();
    void UpdateIntimacyPageUi();
    void UpdateWeatherPageUi();
    void UpdateOpenClawPageUi(DeviceState state);
    void UpdateTelnetPageUi(DeviceState state);
    void UpdateTelnetWaveform();
    void ShowMeetingView(uint8_t view);
    void HandleMeetingBack();
    void RebuildMeetingList();
    void SyncMeetingListScrollRange();
    void RefreshMeetingListVisibleRows();
    static void OnMeetingListScroll(lv_event_t* e);
    void PatchMeetingListStatus(uint32_t id, const char* status_label);
    void DeferRefreshMeetingListFromServer(uint32_t delay_ms);
    void StopMeetingTimer();
    void RefreshMeetingDetailUi();
    void DeleteSelectedMeetingRecord();
    void RetrySelectedMeetingUpload();
    void StartMeetingUpload(uint32_t id, int sec, std::string path);
    void RestorePendingMeetingUploads(bool rebuild_list = true);
    void PersistActiveMeetingPending(const char* phase);
    void ClearMeetingPendingMeta(uint32_t id);
    static void OnMeetingTick(void* arg);
    static void OnTelnetTick(void* arg);
    void ShowStandbyPage(int index);
    int CurrentStandbyIndex() const;
    lv_obj_t* StandbyLayer(int index) const;
    void CancelStandbyMotion(bool resume_expression = true);
    /** Phase-2: pages stay still; wipe overlay + page-dot progress follow the finger. */
    void ApplyStandbyDragProgress(int dx);
    void RequestStandbyDragUiPump();
    void PumpStandbyDragUi();
    void BeginStandbyDragUi();
    void CompleteStandbyDragUi();
    void CommitStandbyPageFast(int to);
    void MarkStandbySnapshotDirty(int page_index);
    void EnsureStandbySnapshotsForDrag(int from, int to);
    void ScheduleStandbySnapshotWarm(int page_index);
    void WarmStandbySnapshotNow(int page_index);
    static void OnStandbySnapshotWarmTick(lv_timer_t* t);
    void StartStandbyWipeSettle(bool commit, int to_index);
    void OnStandbyWipeSettleDone(bool committed);
    static void OnStandbyWipeRevealAnim(void* var, int32_t v);
    static void OnStandbyWipeSettleAnimReady(lv_anim_t* a);
    void ScheduleStandbyExpressionResume();
    void CancelStandbyExpressionResume();
    static void OnStandbyDragUiPumpAsync(void* user);
    static void OnStandbyExpressionResumeTick(lv_timer_t* t);
    /** Freeze home ExpressionView (visible last frame) during carousel drag. */
    void SetStandbyExpressionLite(bool lite);
    /** Delete in-flight X animations on standby layers (call under display lock). */
    void CancelStandbyLayerAnims();
    void StartStandbySlideAnim(lv_obj_t* from, lv_obj_t* to, int from_end_x, int to_start_x,
                               int commit_index);
    void OnStandbySlideDone(bool committed);
    static void OnStandbySlideAnimReady(lv_anim_t* a);
    static void OnMusicTick(void* arg);
    static void OnHourglassTick(void* arg);

    struct AlarmItem {
        uint8_t weekday_mask = 0x7F;  // 每天：周一~周日
        uint8_t hour = 7;
        uint8_t minute = 0;
        bool enabled = true;
    };

    void ShowClockHub(bool show, int tab);  // tab: 0=闹钟 1=倒计时
    void SetClockHubTab(int tab);
    void SyncClockHubTabUi();
    void ShowAlarmHubView(int view);  // 0 list, 1 edit, 2 picker
    void OpenAlarmEditor(int index);  // -1 = new
    bool AlarmNavigateBack();         // nested back; false = leave hub
    void RefreshAlarmEditUi();
    void CommitAlarmPicker();
    void SaveAlarmFromEditor();
    void DeleteEditingAlarm();
    void RebuildAlarmCards();
    void RefreshCountdownArc();
    void CheckAlarmsTick();
    void TriggerAlarm(const AlarmItem& alarm);
    void LoadAlarmsFromNvs();
    void SaveAlarmsToNvs();
    static void OnAlarmCardClick(lv_event_t* e);
    static void OnAlarmSwitchChanged(lv_event_t* e);

    /** UI manager: ring menu + gesture router + screen ownership. */
    vocat::VocatRingMenu ring_menu_;
    vocat::VocatGestureRouter gesture_router_;
    lv_obj_t* home_layer_ = nullptr;
    lv_obj_t* home_bg_glow_ = nullptr;
    lv_obj_t* home_bg_mid_ = nullptr;
    /** Shared Wi‑Fi / time / battery + page dots — outside sliding standby layers. */
    vocat::TopStatusBar standby_chrome_{};
    lv_obj_t* home_character_hit_ = nullptr;
    lv_obj_t* home_avatar_ring_ = nullptr;
    lv_obj_t* home_avatar_disc_ = nullptr;
    lv_obj_t* home_character_img_ = nullptr;
    lv_obj_t* home_emotion_label_ = nullptr;
    /** Pure-LVGL Xiaozhi-style face (replaces Emote GFX / emoji GIF on home). */
    vocat::ExpressionView home_expression_{};
    vocat::ExpressionController home_expr_ctrl_{};
    /** Phase-1 PNG face (base + viseme mouth). Preferred over geometry when ready. */
    vocat::AvatarCompositor home_avatar_{};
    /** Last server/content emotion — kept across listen/speak state-machine "neutral" resets. */
    std::string home_content_emotion_{"relaxed"};
    /** Home-only subtitle band directly below the top status bar. */
    lv_obj_t* home_subtitle_ = nullptr;
    /** Last full assistant line (for streaming suffix-only scroll). */
    std::string home_subtitle_last_full_;
    lv_obj_t* home_mic_btn_ = nullptr;
    lv_obj_t* home_mic_img_ = nullptr;
    lv_obj_t* home_mic_fallback_ = nullptr;
    lv_obj_t* home_mic_caption_ = nullptr;
    lv_obj_t* home_swipe_hint_ = nullptr;
    lv_obj_t* power_overlay_ = nullptr;
    lv_obj_t* power_overlay_label_ = nullptr;
    bool idle_auto_power_off_warning_visible_ = false;

    // System overlays: boot / QR / OTA / success
    lv_obj_t* boot_splash_ = nullptr;
    lv_obj_t* boot_splash_img_ = nullptr;
    lv_obj_t* boot_splash_brand_ = nullptr;  // white status: 正在初始化…
    lv_obj_t* boot_splash_sub_ = nullptr;    // unused (kept null)
    lv_obj_t* boot_splash_dots_ = nullptr;   // unused (kept null)
    esp_timer_handle_t boot_splash_timer_ = nullptr;
    std::unique_ptr<LvglGif> boot_gif_;
    const char* boot_splash_asset_name_ = nullptr;
    bool boot_splash_is_gif_ = false;
    bool boot_animation_running_ = false;
    bool boot_init_presented_ = false;
    bool boot_brand_revealed_ = false;
    bool boot_has_image_ = false;
    bool boot_image_ready_ = false;
    bool boot_brand_hold_done_ = false;
    bool boot_ready_for_home_ = false;
    int64_t boot_init_shown_us_ = 0;
    vocat::BootUiState boot_ui_state_ = vocat::BootUiState::PanelInitializing;
    const lv_font_t* fonts_applied_ = nullptr;

    lv_obj_t* wifi_qr_page_ = nullptr;
    lv_obj_t* wifi_qr_title_ = nullptr;
    lv_obj_t* wifi_qrcode_ = nullptr;
    lv_obj_t* wifi_qr_device_ = nullptr;
    lv_obj_t* wifi_qr_hint_ = nullptr;
    bool wifi_qr_page_visible_ = false;
    /** True when QR widget hidden (BluFi / SoftAP hint on wifi_qr_page_). */
    bool provision_qr_hidden_ = false;
    bool home_ui_deferred_ = false;

    lv_obj_t* ota_progress_page_ = nullptr;
    lv_obj_t* ota_progress_percent_ = nullptr;
    lv_obj_t* ota_progress_bar_ = nullptr;
    lv_obj_t* ota_progress_detail_ = nullptr;
    bool ota_progress_visible_ = false;

    lv_obj_t* success_screen_ = nullptr;
    lv_obj_t* success_screen_icon_ = nullptr;
    lv_obj_t* success_screen_label_ = nullptr;
    bool success_screen_visible_ = false;
    esp_timer_handle_t clock_timer_ = nullptr;
    esp_timer_handle_t music_timer_ = nullptr;
    esp_timer_handle_t hourglass_timer_ = nullptr;

    // Music — player / list / lyrics / empty
    lv_obj_t* music_page_ = nullptr;
    lv_obj_t* music_player_panel_ = nullptr;
    lv_obj_t* music_list_panel_ = nullptr;
    lv_obj_t* music_lyrics_panel_ = nullptr;
    lv_obj_t* music_empty_panel_ = nullptr;
    lv_obj_t* music_cover_ = nullptr;
    lv_obj_t* music_cover_art_ = nullptr;
    lv_obj_t* music_cover_note_ = nullptr;
    std::unique_ptr<LvglAllocatedImage> music_cover_image_;
    lv_obj_t* music_title_ = nullptr;
    lv_obj_t* music_subtitle_ = nullptr;
    lv_obj_t* music_lyric_prev2_ = nullptr;
    lv_obj_t* music_lyric_prev_ = nullptr;
    lv_obj_t* music_lyric_ = nullptr;
    lv_obj_t* music_lyric_next_ = nullptr;
    lv_obj_t* music_lyric_next2_ = nullptr;
    lv_obj_t* music_time_cur_ = nullptr;
    lv_obj_t* music_time_total_ = nullptr;
    lv_obj_t* music_progress_ = nullptr;
    lv_obj_t* music_play_btn_ = nullptr;
    lv_obj_t* music_lyrics_play_btn_ = nullptr;
    /** Visible when music was opened from Xiaozhi chat — returns to listening. */
    lv_obj_t* music_return_chat_btn_ = nullptr;
    lv_obj_t* music_playlist_list_ = nullptr;
    lv_obj_t* music_playlist_spacer_ = nullptr;
    lv_obj_t* music_playlist_page_label_ = nullptr;
    lv_obj_t* music_playlist_rows_[7] = {};
    lv_obj_t* music_rescan_btn_ = nullptr;
    std::vector<std::string> music_tracks_;
    std::string music_synced_title_;
    size_t music_track_index_ = 0;
    int music_playlist_first_visible_ = 0;
    int music_progress_seconds_ = 0;
    int music_duration_seconds_ = 0;
    bool music_playing_ = false;
    bool music_cover_spinning_ = false;
    uint8_t music_view_ = 0;  // 0=player 1=list 2=lyrics 3=empty
    bool music_opened_from_chat_ = false;
    std::vector<std::pair<int, std::string>> music_lyrics_;
    int music_lyric_index_ = -1;
    bool music_lyrics_loaded_ = false;
    void BuildMusicListPanelUi();
    void BuildMusicLyricsPanelUi();
    void BuildMusicEmptyPanelUi();
    void ShowMusicView(uint8_t view);
    void RefreshMusicEmptyState();
    /** Online stream / synced title may own the player UI even with no SD track list. */
    bool HasActiveOnlineMusicUi() const;
    void UpdateMusicLyricForProgress(int progress_ms);
    void RefreshMusicPlaylistVisibleRows();
    void SyncMusicPlaylistScrollRange();
    static void OnMusicPlaylistScroll(lv_event_t* e);

    // Clock hub（闹钟 + 倒计时）
    void StartClockRinging(const char* title, const char* time_text, bool countdown);
    void ClockRingingTick();
    void TriggerCountdownFinished();
    lv_obj_t* clock_page_ = nullptr;
    lv_obj_t* clock_back_btn_ = nullptr;
    lv_obj_t* clock_title_label_ = nullptr;
    lv_obj_t* clock_tab_bar_ = nullptr;
    lv_obj_t* clock_tab_alarm_btn_ = nullptr;
    lv_obj_t* clock_tab_timer_btn_ = nullptr;
    lv_obj_t* alarm_panel_ = nullptr;
    lv_obj_t* clock_list_ = nullptr;       // scroll container for cards
    lv_obj_t* clock_hint_ = nullptr;
    lv_obj_t* clock_fab_btn_ = nullptr;
    lv_obj_t* alarm_edit_panel_ = nullptr;
    lv_obj_t* alarm_edit_time_row_ = nullptr;
    lv_obj_t* alarm_edit_hour_label_ = nullptr;
    lv_obj_t* alarm_edit_minute_label_ = nullptr;
    lv_obj_t* alarm_edit_day_btns_[7] = {};
    lv_obj_t* alarm_edit_save_btn_ = nullptr;
    lv_obj_t* alarm_edit_delete_btn_ = nullptr;
    lv_obj_t* alarm_picker_panel_ = nullptr;
    lv_obj_t* alarm_picker_sel_bar_ = nullptr;
    lv_obj_t* alarm_hour_roller_ = nullptr;
    lv_obj_t* alarm_minute_roller_ = nullptr;
    lv_obj_t* clock_ring_overlay_ = nullptr;
    lv_obj_t* clock_ring_title_label_ = nullptr;
    lv_obj_t* clock_ring_time_label_ = nullptr;
    std::vector<AlarmItem> alarms_;
    std::vector<int> alarm_last_fired_keys_;
    bool alarm_ringing_ = false;
    bool countdown_ringing_ = false;
    int ringing_elapsed_sec_ = 0;
    int alarm_prev_output_volume_ = -1;
    static constexpr int kRingingMaxSeconds = 5 * 60;
    int clock_hub_tab_ = 0;       // 0 alarm, 1 countdown
    int alarm_hub_view_ = 0;      // 0 list, 1 edit, 2 picker
    int alarm_edit_index_ = -1;   // -1 new
    int clock_pick_hour_ = 7;
    int clock_pick_minute_ = 0;
    uint8_t alarm_edit_mask_ = 0x7F;
    static constexpr size_t kClockMaxAlarms = 5;

    // Countdown（挂在 hub 内；离页可后台继续）
    lv_obj_t* hourglass_page_ = nullptr;
    lv_obj_t* hourglass_arc_ = nullptr;
    lv_obj_t* hourglass_time_ = nullptr;
    lv_obj_t* hourglass_status_ = nullptr;
    lv_obj_t* hourglass_start_btn_ = nullptr;
    lv_obj_t* hourglass_cancel_btn_ = nullptr;
    int hourglass_remain_seconds_ = 600;  // default 10 min
    int hourglass_total_seconds_ = 600;
    bool hourglass_running_ = false;
    bool hourglass_suspended_for_ring_ = false;

    // Settings — miaoban 2×2 home + sub-pages
    lv_obj_t* settings_page_ = nullptr;
    lv_obj_t* settings_home_panel_ = nullptr;
    lv_obj_t* settings_interaction_panel_ = nullptr;
    lv_obj_t* settings_sound_panel_ = nullptr;
    lv_obj_t* settings_network_panel_ = nullptr;
    lv_obj_t* settings_general_panel_ = nullptr;
    lv_obj_t* settings_language_panel_ = nullptr;
    lv_obj_t* settings_about_panel_ = nullptr;
    lv_obj_t* settings_interaction_list_ = nullptr;
    lv_obj_t* settings_sound_list_ = nullptr;
    lv_obj_t* settings_network_list_ = nullptr;
    lv_obj_t* settings_general_list_ = nullptr;
    lv_obj_t* settings_language_list_ = nullptr;
    lv_obj_t* settings_about_list_ = nullptr;
    lv_obj_t* settings_network_status_ = nullptr;
    lv_obj_t* settings_wifi_row_ = nullptr;
    lv_obj_t* settings_4g_row_ = nullptr;
    lv_obj_t* settings_lang_zh_row_ = nullptr;
    lv_obj_t* settings_lang_en_row_ = nullptr;
    lv_obj_t* settings_lang_ru_row_ = nullptr;
    lv_obj_t* settings_about_value_labels_[7] = {};
    lv_obj_t* settings_vol_slider_ = nullptr;
    lv_obj_t* settings_bri_slider_ = nullptr;
    lv_obj_t* settings_sw_realtime_ = nullptr;
    lv_obj_t* settings_sw_head_ = nullptr;
    lv_obj_t* settings_sw_gyro_ = nullptr;
    lv_obj_t* settings_sw_doa_ = nullptr;
    lv_obj_t* settings_sw_mute_ = nullptr;
    lv_obj_t* settings_sw_power_ = nullptr;
    lv_obj_t* settings_sw_idle_off_ = nullptr;
    uint8_t settings_sub_page_ = 0;  // 0=home … 4=general 5=language 6=about
    bool settings_syncing_sliders_ = false;
    bool settings_confirm_enabling_doa_ = false;
    void ShowSettingsSubPage(uint8_t sub);
    void RefreshSettingsInteractionPanel();
    void RefreshSettingsSoundPanel();
    void RefreshSettingsNetworkPanel();
    void RefreshSettingsLanguagePanel();
    void RefreshSettingsAboutPanel();
    static void OnDeferredRefreshSoundPanel(void* user_data);
    bool SettingsNavigateBack();
    void DismissSettingsConfirmOverlay();
    void ConfirmDoaRealtimeConflict(bool enabling_doa);
    void ConfirmFactoryReset();
    void StartSettingsCheckUpdate();
    void ConfirmFirmwareUpgrade(const std::string& url, const std::string& version);
    lv_obj_t* settings_confirm_overlay_ = nullptr;
    std::string pending_upgrade_url_;
    std::string pending_upgrade_version_;

    // OpenClaw
    lv_obj_t* openclaw_page_ = nullptr;
    lv_obj_t* openclaw_pair_panel_ = nullptr;
    lv_obj_t* openclaw_offline_panel_ = nullptr;
    lv_obj_t* openclaw_ready_panel_ = nullptr;
    lv_obj_t* openclaw_active_panel_ = nullptr;
    lv_obj_t* openclaw_result_panel_ = nullptr;
    lv_obj_t* openclaw_pair_code_label_ = nullptr;
    lv_obj_t* openclaw_online_badge_ = nullptr;
    lv_obj_t* openclaw_mic_btn_ = nullptr;
    lv_obj_t* openclaw_interrupt_btn_ = nullptr;
    lv_obj_t* openclaw_active_dot_ = nullptr;
    lv_obj_t* openclaw_result_text_ = nullptr;
    bool openclaw_mic_active_ = false;
    bool openclaw_connector_online_ = false;
    std::string openclaw_pair_code_;
    std::string openclaw_hint_text_;

    // Telnet
    lv_obj_t* telnet_page_ = nullptr;
    lv_obj_t* telnet_pair_panel_ = nullptr;
    lv_obj_t* telnet_offline_panel_ = nullptr;
    lv_obj_t* telnet_ready_panel_ = nullptr;
    lv_obj_t* telnet_rec_panel_ = nullptr;
    lv_obj_t* telnet_result_panel_ = nullptr;
    lv_obj_t* telnet_pair_code_label_ = nullptr;
    lv_obj_t* telnet_start_btn_ = nullptr;
    lv_obj_t* telnet_pause_btn_ = nullptr;
    lv_obj_t* telnet_rec_time_ = nullptr;
    lv_obj_t* telnet_result_text_ = nullptr;
    lv_obj_t* telnet_wave_bars_[8] = {};
    esp_timer_handle_t telnet_timer_ = nullptr;
    bool telnet_listening_ = false;
    bool telnet_paused_ = false;
    bool telnet_has_content_ = false;
    int telnet_rec_seconds_ = 0;
    std::string telnet_dictation_text_;

    // Meeting
    struct MeetingEntry {
        uint32_t id = 0;
        std::string title;
        std::string date;
        std::string duration;
        int duration_sec = 0;
        std::string status;
        std::string summary;
        std::string local_path;
    };
    lv_obj_t* meeting_page_ = nullptr;
    lv_obj_t* meeting_page_title_ = nullptr;
    lv_obj_t* meeting_panel_list_ = nullptr;
    lv_obj_t* meeting_panel_rec_ = nullptr;
    lv_obj_t* meeting_panel_save_ = nullptr;
    lv_obj_t* meeting_panel_detail_ = nullptr;
    lv_obj_t* meeting_list_scroll_ = nullptr;
    lv_obj_t* meeting_list_empty_ = nullptr;
    lv_obj_t* meeting_new_btn_ = nullptr;
    lv_obj_t* meeting_rec_time_ = nullptr;
    lv_obj_t* meeting_rec_status_ = nullptr;
    lv_obj_t* meeting_rec_caption_scroll_ = nullptr;
    lv_obj_t* meeting_rec_caption_ = nullptr;
    lv_obj_t* meeting_save_time_ = nullptr;
    lv_obj_t* meeting_detail_meta_ = nullptr;
    lv_obj_t* meeting_detail_summary_ = nullptr;
    lv_obj_t* meeting_play_btn_ = nullptr;
    lv_obj_t* meeting_speak_btn_ = nullptr;
    lv_obj_t* meeting_delete_btn_ = nullptr;
    lv_obj_t* meeting_retry_btn_ = nullptr;
    esp_timer_handle_t meeting_timer_ = nullptr;
    uint8_t meeting_view_ = 0;
    int meeting_rec_seconds_ = 0;
    int meeting_selected_ = -1;
    uint32_t meeting_active_id_ = 0;
    std::string meeting_active_path_;
    std::string meeting_caption_committed_;
    std::string meeting_caption_partial_;
    bool meeting_recording_ = false;
    bool meeting_paused_ = false;
    /** No local WAV (SD missing); cloud STT preview only. */
    bool meeting_cloud_only_ = false;
    bool meeting_network_lost_ = false;
    bool meeting_audio_playing_ = false;
    bool meeting_uploading_ = false;
    uint32_t meeting_list_refresh_gen_ = 0;
    int64_t meeting_caption_last_ui_us_ = 0;
    bool meeting_caption_dirty_ = false;
    lv_obj_t* meeting_list_spacer_ = nullptr;
    lv_obj_t* meeting_list_rows_[6] = {};
    int meeting_list_first_visible_ = 0;
    bool meeting_page_visible_ = false;
    std::vector<MeetingEntry> meeting_entries_;

    // Intimacy — design: arc score + mood/vitality cards
    lv_obj_t* intimacy_page_ = nullptr;
    lv_obj_t* intimacy_arc_ = nullptr;
    lv_obj_t* intimacy_score_label_ = nullptr;
    lv_obj_t* intimacy_tier_label_ = nullptr;
    lv_obj_t* intimacy_row_icon_[3] = {};
    lv_obj_t* intimacy_row_title_[3] = {};
    lv_obj_t* intimacy_row_pct_[3] = {};
    lv_obj_t* intimacy_row_bar_[3] = {};
    lv_obj_t* intimacy_row_sub_[3] = {};
    lv_obj_t* intimacy_mood_card_ = nullptr;
    lv_obj_t* intimacy_vital_card_ = nullptr;
    lv_obj_t* intimacy_mood_label_ = nullptr;
    lv_obj_t* intimacy_vital_label_ = nullptr;
    lv_obj_t* intimacy_mood_base_img_ = nullptr;
    lv_obj_t* intimacy_mood_face_img_ = nullptr;
    lv_obj_t* intimacy_center_mood_base_ = nullptr;
    lv_obj_t* intimacy_center_mood_face_ = nullptr;
    lv_obj_t* intimacy_vital_img_ = nullptr;
    lv_obj_t* intimacy_refresh_btn_ = nullptr;
    lv_obj_t* intimacy_refresh_img_ = nullptr;
    // Same NVS keys / sentinels as Emote (pet_em_show / pet_vitality: -1 = unknown)
    int intimacy_value_ = 0;
    int pet_emotion_ = -1;
    int pet_vitality_ = -1;
    std::string intimacy_tier_;
    std::string intimacy_updated_at_;
    std::string pet_emotion_tier_;
    std::string pet_device_id_;

    // Weather (standby left)
    lv_obj_t* weather_page_ = nullptr;
    lv_obj_t* weather_city_label_ = nullptr;
    lv_obj_t* weather_update_label_ = nullptr;
    lv_obj_t* weather_alarm_label_ = nullptr;
    lv_obj_t* weather_icon_img_ = nullptr;
    lv_obj_t* weather_wea_label_ = nullptr;
    lv_obj_t* weather_temp_label_ = nullptr;
    lv_obj_t* weather_hl_label_ = nullptr;
    lv_obj_t* weather_extra_label_ = nullptr;
    lv_obj_t* weather_hum_card_ = nullptr;
    lv_obj_t* weather_air_card_ = nullptr;
    lv_obj_t* weather_hum_label_ = nullptr;
    lv_obj_t* weather_air_label_ = nullptr;
    lv_obj_t* weather_refresh_btn_ = nullptr;
    lv_obj_t* weather_refresh_img_ = nullptr;
    std::string weather_city_;
    std::string weather_wea_;
    std::string weather_wea_img_;
    std::string weather_update_;
    std::string weather_air_;
    std::string weather_alarm_;
    int weather_temp_ = 0;
    int weather_temp_high_ = 0;
    int weather_temp_low_ = 0;
    int weather_humidity_ = 0;
    bool weather_has_data_ = false;

    // Picture / media gallery
    lv_obj_t* picture_page_ = nullptr;
    lv_obj_t* picture_list_panel_ = nullptr;
    lv_obj_t* picture_viewer_panel_ = nullptr;
    lv_obj_t* picture_empty_panel_ = nullptr;
    lv_obj_t* picture_status_ = nullptr;
    lv_obj_t* picture_list_label_ = nullptr;
    lv_obj_t* picture_list_scroll_ = nullptr;
    lv_obj_t* picture_list_spacer_ = nullptr;
    lv_obj_t* picture_list_rows_[7] = {};
    lv_obj_t* picture_view_img_ = nullptr;
    lv_obj_t* picture_view_name_ = nullptr;
    std::vector<std::string> picture_files_;
    size_t picture_index_ = 0;
    int picture_list_first_visible_ = 0;
    uint8_t picture_view_ = 0;  // 0=list 1=viewer 2=empty
    uint8_t* picture_rgb565_ = nullptr;
    size_t picture_rgb565_size_ = 0;
    lv_image_dsc_t picture_img_dsc_{};
    void ShowPictureView(uint8_t view);
    void RefreshPictureList();
    void RefreshPictureListVisibleRows();
    void SyncPictureListScrollRange();
    void FreePictureFramebuffer();
    bool LoadPictureAt(size_t index);
    static void OnPictureListScroll(lv_event_t* e);

    // Football
    lv_obj_t* football_page_ = nullptr;
    lv_obj_t* football_hint_ = nullptr;

    // Quick settings (half-screen overlay) — mirrors Emote 系统设置 high-freq rows
    lv_obj_t* qs_root_ = nullptr;
    lv_obj_t* qs_panel_ = nullptr;
    lv_obj_t* qs_net_icon_ = nullptr;
    lv_obj_t* qs_net_label_ = nullptr;
    lv_obj_t* qs_net_value_ = nullptr;
    lv_obj_t* qs_vol_label_ = nullptr;
    lv_obj_t* qs_bri_label_ = nullptr;
    lv_obj_t* qs_vol_fill_ = nullptr;
    lv_obj_t* qs_bri_fill_ = nullptr;
    lv_obj_t* qs_vol_slider_ = nullptr;
    lv_obj_t* qs_bri_slider_ = nullptr;
    lv_obj_t* qs_net_hit_ = nullptr;
    lv_obj_t* qs_power_hit_ = nullptr;
    lv_obj_t* qs_power_label_ = nullptr;
    lv_obj_t* qs_power_knob_ = nullptr;
    lv_obj_t* qs_power_track_ = nullptr;
    int qs_panel_h_ = 236;
    int qs_slider_drag_ = 0;  // 0 none, 1 volume, 2 brightness
    bool qs_power_save_ = false;
    bool qs_net_press_ = false;
    int64_t qs_net_press_us_ = 0;
    bool quick_settings_visible_ = false;

    bool home_chat_visible_ = true;
    bool music_page_visible_ = false;
    bool picture_page_visible_ = false;
    bool clock_page_visible_ = false;
    bool hourglass_page_visible_ = false;
    bool settings_page_visible_ = false;
    bool intimacy_page_visible_ = false;
    bool weather_page_visible_ = false;
    bool football_page_visible_ = false;
    bool openclaw_page_visible_ = false;
    bool telnet_page_visible_ = false;
    bool wifi_config_hint_active_ = false;
    // wifi_qr_page_visible_ / ota_progress_visible_ / success_screen_visible_ declared above
    /** Standby carousel: -1 weather, 0 home, 1 intimacy */
    int standby_index_ = 0;
    /** False after menu→赛搏对话; true for idle standby / weather / intimacy. */
    bool standby_carousel_enabled_ = true;
    bool standby_dragging_ = false;
    bool standby_animating_ = false;
    int standby_drag_from_ = 0;
    int standby_drag_to_ = 0;
    int standby_drag_dx_ = 0;
    /** +1 = finger right (prev page), -1 = finger left (next page); 0 = unlocked. */
    int standby_drag_direction_ = 0;
    int standby_pending_index_ = 0;
    bool standby_pending_commit_ = false;
    int64_t standby_last_apply_us_ = 0;
    int standby_last_dx_ = 0;
    int64_t standby_last_dx_us_ = 0;
    float standby_velocity_px_s_ = 0.f;
    /** Latest finger dx from gesture task; UI pump consumes this. */
    int standby_desired_dx_ = 0;
    bool standby_drag_pending_ = false;
    bool standby_begin_pending_ = false;
    bool standby_finish_pending_ = false;
    int standby_finish_sdx_ = 0;
    bool standby_ui_pump_scheduled_ = false;
    /** Last applied page-dot progress (0..256) to coalesce tiny updates. */
    int standby_progress_t256_ = -1;
    int standby_progress_neighbor_ = 99;
    bool standby_wipe_active_ = false;
    int standby_wipe_settle_to_ = 0;
    int standby_warm_page_ = 0;
    lv_timer_t* standby_warm_timer_ = nullptr;
    vocat::StandbyWipeTransition standby_wipe_;
    lv_timer_t* standby_expr_resume_timer_ = nullptr;
    /** Freeze home ExpressionView (keep last frame visible) for standby drag. */
    bool standby_expression_lite_ = false;
    /** Nested motion gate (standby / menu / quick-settings animations). */
    int ui_motion_count_ = 0;
    bool pending_ui_scheduled_ = false;
    bool qs_ui_motion_ = false;
    /** Defer status / weather / intimacy redraws while dragging or animating. */
    bool pending_status_bar_ = false;
    bool pending_weather_ui_ = false;
    bool pending_intimacy_ui_ = false;
};
