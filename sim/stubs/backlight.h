#pragma once

#include <cstdint>

constexpr uint8_t kBacklightDefaultPercent = 75;
constexpr uint8_t kBacklightMinPercent = 5;

class Backlight {
public:
    void SetBrightness(uint8_t brightness, bool permanent = false);
    uint8_t brightness() const { return brightness_; }

private:
    uint8_t brightness_ = kBacklightDefaultPercent;
};
