#include "theme_manager.h"

#include "settings.h"

namespace ThemeManager {

namespace {

constexpr const char* kSettingsNs = "ui";
constexpr const char* kKeyThemeId = "theme_id";

// 缓存主题 ID，避免 LVGL（尤其 PSRAM 栈）路径反复读 NVS。
int s_cached_theme_id = -1;

}  // namespace

int GetCurrentThemeId() {
    if (s_cached_theme_id >= kMinThemeId && s_cached_theme_id <= kMaxThemeId) {
        return s_cached_theme_id;
    }
    Settings s(kSettingsNs, false);
    int id = s.GetInt(kKeyThemeId, kDefaultThemeId);
    if (id < kMinThemeId || id > kMaxThemeId) {
        id = kDefaultThemeId;
    }
    s_cached_theme_id = id;
    return id;
}

void SetCurrentThemeId(int id) {
    if (id < kMinThemeId || id > kMaxThemeId) {
        return;
    }
    s_cached_theme_id = id;
    Settings s(kSettingsNs, true);
    s.SetInt(kKeyThemeId, id);
}

}  // namespace ThemeManager
