#include "sim_screens.h"

#include <cstring>

#include "album_screen/album_screen.h"
#include "clock_screen/clock_screen.h"
#include "settings_screen/settings_screen.h"
#include "sim_ramimg.h"

namespace {

void AlbumLifecycle(screen_lifecycle_event_t event) {
    AlbumScreen::LifecycleCallback(event);
}

void ClockLifecycle(screen_lifecycle_event_t event) {
    ClockScreen::LifecycleCallback(event);
}

void SettingsLifecycle(screen_lifecycle_event_t event) {
    SettingsScreen::LifecycleCallback(event);
}

// 想仿真新的屏：把它的 .cc 加进 sim/CMakeLists.txt 的 SCREEN_SOURCES，
// 缺的 ESP-IDF 头在 sim/stubs 里补上，然后在这里加一行。
constexpr SimScreenEntry kScreens[] = {
    {"album", AlbumScreen::Create, AlbumLifecycle},
    {"clock", ClockScreen::Create, ClockLifecycle},
    {"settings", SettingsScreen::Create, SettingsLifecycle},
    {"ramimg", SimRamImgCreate, nullptr},
};

}  // namespace

const SimScreenEntry* SimScreens() {
    return kScreens;
}

int SimScreenCount() {
    return static_cast<int>(sizeof(kScreens) / sizeof(kScreens[0]));
}

const SimScreenEntry* SimFindScreen(const char* name) {
    if (name == nullptr) {
        return nullptr;
    }
    for (const SimScreenEntry& e : kScreens) {
        if (strcmp(e.name, name) == 0) {
            return &e;
        }
    }
    return nullptr;
}
