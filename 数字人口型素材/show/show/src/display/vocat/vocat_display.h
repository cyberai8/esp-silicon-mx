#pragma once

#include "device_state.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class Display;

namespace vocat {

/**
 * VoCat device UI contract. Application / board / music call this — not a concrete UI class.
 * Implementations: VocatLvglDisplay (current, CONFIG_USE_LVGL_VOCAT_UI) or EmoteDisplay (legacy).
 */
class IVocatDisplay {
public:
    virtual ~IVocatDisplay() = default;

    // --- Ring menu ---
    virtual bool IsRingMenuVisible() const = 0;
    virtual void TriggerRingMenuFromBottom(bool animate_from_bottom = true) = 0;
    virtual void HideRingMenu() = 0;
    virtual void HideRingMenuImmediate() = 0;
    virtual void ReturnToMainMenu() = 0;
    virtual void ForceShowRingMenuNow() = 0;
    virtual void EnterHomeChatFromRing(bool enable_standby_carousel = true) = 0;
    virtual void SetRingMenuPressedSegment(int segment) = 0;
    virtual void ClearRingMenuPressedSegment() = 0;
    /** Screen hit-test for 2×2 menu → dispatcher segment, or -1. */
    virtual int HitTestRingMenu(int x, int y) { (void)x; (void)y; return -1; }
    /** Menu page swipe: sdx>0 previous, sdx<0 next. */
    virtual bool HandleRingMenuPageSwipe(int sdx) { (void)sdx; return false; }
    /** Follow-finger menu page drag (same motion policy as standby). */
    virtual bool HandleRingMenuPageDrag(int dx, int dy)
    {
        (void)dx;
        (void)dy;
        return false;
    }
    virtual bool FinishRingMenuPageDrag(int sdx) { (void)sdx; return false; }
    virtual bool IsRingMenuPageGestureActive() const { return false; }
    virtual bool ShouldIgnoreHeadTouchForMenuUi() const = 0;

    // --- Home chat ---
    virtual bool IsHomeChatVisible() const = 0;
    /** Keep a transient face visible while state-machine neutral updates are queued underneath. */
    virtual void PlayTemporaryHomeEmotion(const char* emotion, uint32_t duration_ms)
    {
        (void)emotion;
        (void)duration_ms;
    }

    // --- Music ---
    virtual void ShowMusicPage(bool show) = 0;
    virtual bool IsMusicPageVisible() const = 0;
    virtual bool IsMusicPlaylistVisible() const = 0;
    virtual bool IsMusicMoreMenuVisible() const = 0;
    virtual bool IsMusicDownloadProgressVisible() const = 0;
    virtual void SetMusicTracks(const std::vector<std::string>& tracks) = 0;
    virtual bool TryStartLocalMusicPlayback() = 0;
    virtual std::string GetCurrentMusicTrackName() const = 0;
    virtual void SyncMusicPlayback(const std::string& song_name, int duration_seconds, bool is_playing) = 0;
    /** Mark whether this music session was opened from Xiaozhi chat (vs ring menu). */
    virtual void SetMusicOpenedFromChat(bool from_chat) { (void)from_chat; }
    virtual bool IsMusicOpenedFromChat() const { return false; }
    /** Leave music → home face + xiaozhi; optionally start listening immediately. */
    virtual void ReturnMusicToXiaozhiChat(bool start_listening = true)
    {
        (void)start_listening;
    }
    virtual void ToggleMusicPlaylist() = 0;
    virtual void ToggleMusicMoreMenu() = 0;
    virtual void MusicPrevious() = 0;
    virtual void MusicNext() = 0;
    virtual void MusicTogglePause() = 0;
    virtual int MusicCycleVolumeLevel() = 0;
    virtual bool HandleMusicPlaylistTouchRelease(int sdy, int sx, int sy, int ex, int ey) = 0;
    virtual bool HandleMusicMoreMenuTouchRelease(int sx, int sy, int ex, int ey) = 0;
    virtual bool HandleMusicDownloadProgressTouchRelease(int sx, int sy, int ex, int ey) = 0;

    // --- Picture ---
    virtual void ShowPicturePage(bool show) = 0;
    virtual bool IsPicturePageVisible() const = 0;
    virtual bool HandlePictureTouchRelease(int sdx, int sdy, int sx, int sy, int ex, int ey) = 0;

    // --- Hourglass ---
    virtual void ShowHourglassPage(bool show) = 0;
    virtual bool IsHourglassPageVisible() const = 0;
    virtual bool IsHourglassSuspendedForRingMenu() const = 0;
    virtual void LeaveHourglassForOtherRingModule() = 0;
    virtual bool StartCountdownAndShow(int duration_seconds) = 0;
    virtual void OpenCountdownApp() = 0;
    virtual bool HandleHourglassTouchRelease(int sdx, int sdy, int sx, int sy, int ex, int ey) = 0;
    virtual void HandleHourglassPickerFingerDown(int x, int y) = 0;
    virtual bool HandleHourglassPickerFingerMove(int x, int y) = 0;

    // --- Clock / alarm ---
    virtual void ShowClockPage(bool show) = 0;
    virtual bool IsClockPageVisible() const = 0;
    virtual bool IsAlarmRinging() const = 0;
    virtual void DismissAlarmRinging() = 0;
    virtual bool AddAlarmAndShow(uint8_t hour, uint8_t minute, uint8_t weekday_mask = 0) = 0;
    virtual void OpenAlarmApp() = 0;
    virtual bool HandleClockTouchRelease(int sdx, int sdy, int sx, int sy, int ex, int ey) = 0;
    virtual void HandleClockPickerFingerDown(int x, int y) = 0;
    virtual bool HandleClockPickerFingerMove(int x, int y) = 0;

    /** Debug: cycle through 21 ExpressionState looks; returns name. */
    virtual std::string DebugCycleHomeExpression() { return ""; }
    /** Debug: set face by emotion name (e.g. sad/cool/eat). "wink" → Winking state. */
    virtual std::string DebugSetHomeExpression(const char* emotion)
    {
        (void)emotion;
        return "";
    }

    // --- Music lyrics (optional; Emote full / LVGL stub OK) ---
    virtual bool LoadMusicLyricsFromFile(const std::string& lrc_path) = 0;
    virtual void ClearMusicLyrics() = 0;
    virtual void SetMusicLyricsFromTimestampedLines(
        const std::vector<std::pair<int, std::string>>& lines) = 0;
    /**
     * Album cover for a specific track. Takes ownership of RGB565 buffer
     * (size×size×2, usually PSRAM). Ignored if song_title is not the current
     * synced track (stale cover after fast skip).
     */
    virtual void SetMusicCoverRgb565(const std::string& song_title, uint8_t* rgb565, int size);
    virtual bool InsertAnimDialog(const char* emoji_name, uint32_t duration_ms) = 0;
    virtual bool StopAnimDialog() = 0;

    // --- Settings ---
    virtual void ShowSettingsPage(bool show) = 0;
    virtual bool IsSettingsPageVisible() const = 0;
    virtual void HandleSettingsTouchPress(int x, int y) = 0;
    virtual bool HandleSettingsTouchRelease(int sdx, int sdy, int sx, int sy, int ex, int ey) = 0;
    virtual bool HandleSettingsTouchHold(int x, int y) = 0;

    // --- Intimacy / pet metrics ---
    virtual void ShowIntimacyPage(bool show) = 0;
    virtual bool IsIntimacyPageVisible() const = 0;
    virtual void SetIntimacy(int value) = 0;
    virtual void SetIntimacyTier(const char* tier) = 0;
    virtual void SetIntimacyUpdatedAt(const char* iso_time) = 0;
    virtual void SetPetEmotion(int show, const char* tier) = 0;
    virtual void SetPetVitality(int value) = 0;
    virtual void SetPetDeviceId(const char* device_id) = 0;
    virtual bool HandleIntimacyTouchRelease(int sdx, int sdy, int sx, int sy, int ex, int ey) = 0;

    // --- Quick settings (LVGL; Emote no-op) ---
    virtual void ShowQuickSettings(bool show) { (void)show; }
    virtual bool IsQuickSettingsVisible() const { return false; }
    /** Top-edge pull-down allowed on standby + feature pages (not ring/QS itself). */
    virtual bool AllowsQuickSettingsPullDown() const { return false; }
    virtual void HandleQuickSettingsTouchPress(int x, int y) { (void)x; (void)y; }
    virtual bool HandleQuickSettingsTouchHold(int x, int y) { (void)x; (void)y; return false; }
    virtual bool HandleQuickSettingsTouchRelease(int sdx, int sdy, int sx, int sy, int ex, int ey)
    {
        (void)sdx;
        (void)sdy;
        (void)sx;
        (void)sy;
        (void)ex;
        (void)ey;
        return false;
    }

    // --- Standby carousel: 天气 ← 角色 → 亲密度 (LVGL; Emote no-op) ---
    virtual void ShowWeatherPage(bool show) { (void)show; }
    virtual bool IsWeatherPageVisible() const { return false; }
    virtual bool HandleWeatherTouchRelease(int sdx, int sdy, int sx, int sy, int ex, int ey)
    {
        (void)sdx;
        (void)sdy;
        (void)sx;
        (void)sy;
        (void)ex;
        (void)ey;
        return false;
    }
    /** Apply fetched weather; implementations persist last-good snapshot for offline. */
    virtual void ApplyWeatherData(const char* city, const char* wea, const char* wea_img, int temp,
                                  int temp_high, int temp_low, int humidity, const char* air,
                                  const char* alarm, const char* update_hhmm)
    {
        (void)city;
        (void)wea;
        (void)wea_img;
        (void)temp;
        (void)temp_high;
        (void)temp_low;
        (void)humidity;
        (void)air;
        (void)alarm;
        (void)update_hhmm;
    }
    /** sdx<0 → page on the right; sdx>0 → page on the left. Returns true if handled. */
    virtual bool HandleStandbyHorizontalSwipe(int sdx) { (void)sdx; return false; }
    /** Follow-finger while pressed. dx/dy = finger delta from press start. */
    virtual bool HandleStandbyDrag(int dx, int dy) { (void)dx; (void)dy; return false; }
    /** End drag: commit page change or snap back. Returns true if a page was committed. */
    virtual bool FinishStandbyDrag(int sdx, int sdy) { (void)sdx; (void)sdy; return false; }
    virtual bool IsStandbyGestureActive() const { return false; }
    /** Block ring menu / standby carousel while bind QR, boot splash, or home gate closed. */
    virtual bool IsHomeNavigationBlocked() const { return false; }
    /** True while device is connecting/listening/speaking — block carousel & ring steal. */
    virtual bool IsLiveVoiceChatActive() const { return false; }
    /**
     * True on home / weather / intimacy with no ring, feature pages, quick settings,
     * alarm ringing, or boot/OTA overlays — eligible for idle auto power-off.
     */
    virtual bool IsStandbyIdleSurface() const { return false; }

    /**
     * Feature-page system back: left-edge swipe (back-pill taps use LVGL indev).
     */
    virtual bool TryConsumeFeatureBackGesture(int sdx, int sdy, int sx, int sy, int ex, int ey)
    {
        (void)sdx;
        (void)sdy;
        (void)sx;
        (void)sy;
        (void)ex;
        (void)ey;
        return false;
    }

    // --- Football ---
    virtual void ShowFootballPage(bool show) = 0;
    virtual bool IsFootballPageVisible() const = 0;
    virtual bool HandleFootballTouchRelease(int sdx, int sdy, int sx, int sy, int ex, int ey) = 0;

    // --- OpenClaw ---
    virtual void ShowOpenClawPage(bool show, bool return_to_home = false) = 0;
    virtual bool IsOpenClawPageVisible() const = 0;
    virtual void RefreshOpenClawConversationUi(DeviceState state) = 0;
    virtual void SetOpenClawBridgeStatus(bool connector_online, const char* pair_code) = 0;
    virtual bool HandleOpenClawTouchRelease(int sdx, int sdy, int sx, int sy, int ex, int ey) = 0;

    // --- Telnet dictation ---
    virtual void ShowTelnetDictationPage(bool show, bool return_to_home = false) = 0;
    virtual bool IsTelnetDictationPageVisible() const = 0;
    virtual void RefreshTelnetDictationTransmissionUi(DeviceState state) = 0;
    virtual bool HandleTelnetDictationTouchRelease(int sdx, int sdy, int sx, int sy, int ex, int ey) = 0;

    // --- Meeting notes ---
    virtual void ShowMeetingPage(bool show, bool return_to_home = false) = 0;
    virtual bool IsMeetingPageVisible() const = 0;
    virtual bool HandleMeetingTouchRelease(int sdx, int sdy, int sx, int sy, int ex, int ey) = 0;

    // --- WiFi / boot overlays ---
    virtual void SetWifiConfigHint(const char* status, const char* hint) = 0;
    virtual void ClearWifiConfigHint() = 0;
    virtual bool IsWifiConfigHintActive() const = 0;
    virtual bool IsWifiQrPageVisible() const = 0;
    /**
     * Early boot: opaque black + CYBER TECH + "正在初始化" using App basic font.
     * Does not touch backlight — caller opens light only after first flush.
     */
    virtual void PresentBootInitializing() {}
    /** Record light-on time for early-page min hold (call right after RestoreBrightness). */
    virtual void MarkBootInitializingVisible() {}
    /** Update init-screen status line; no-op after brand reveal. */
    virtual void SetBootInitializingText(const char* message) { (void)message; }
    virtual void SetBootUiState(uint8_t /*BootUiState*/) {}
    virtual bool ShowBootAnimation() = 0;
    /** Allow boot splash to finish to home after brand hold (call when activation/network ready). */
    virtual void CompleteBootSplash() {}
    /** Drop reconnect/status boot overlay without forcing home (restores touch + ring/settings). */
    virtual void DismissBlockingBootOverlay() {}
    /** OGG_BOOT drained — retry splash→home gate without marking activation ready. */
    virtual void NotifyBootSoundFinished() {}
    /** Drop decoded boot brand after QR handoff (optional). */
    virtual void ReleaseBootVisualResources() {}
    virtual bool IsHourglassRunning() const { return false; }
    /** Any scheduled/enabled alarm — keep device on so it can still ring (idle auto-off policy A). */
    virtual bool HasEnabledAlarm() const { return false; }
    /** Boot splash, OTA, WiFi QR, success/reboot screens — block auto power-off timer. */
    virtual bool IsSystemUiBlockingAutoPowerOff() const { return false; }
    virtual void ShowIdleAutoPowerOffWarning(int seconds_remaining) { (void)seconds_remaining; }
    virtual void HideIdleAutoPowerOffWarning() {}

    virtual void ShowPowerOverlay(const char* message) = 0;
    virtual void HidePowerOverlay() = 0;
    virtual void RefreshAll() = 0;
};

IVocatDisplay* GetVocatDisplay(Display* display);

/** Show music page on the main/UI thread (safe from MCP or audio tasks). */
void RequestShowMusicPage(IVocatDisplay* display);

}  // namespace vocat
