#include "sim_screens.h"

#include <cstring>

#include "album_screen/album_screen.h"
#include "clock_screen/clock_screen.h"
#include "settings_screen/settings_screen.h"
#include "standby_screen/standby_screen.h"
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

void StandbyLifecycle(screen_lifecycle_event_t event) {
    StandbyScreen::LifecycleCallback(event);
}

constexpr SimScreenEntry kScreens[] = {
    {"album", AlbumScreen::Create, AlbumLifecycle},
    {"clock", ClockScreen::Create, ClockLifecycle},
    {"settings", SettingsScreen::Create, SettingsLifecycle},
    {"standby", StandbyScreen::Create, StandbyLifecycle},
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
