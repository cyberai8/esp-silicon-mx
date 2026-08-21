#pragma once

#include "audio_codec.h"
#include "backlight.h"

class Board {
public:
    static Board& GetInstance();

    Backlight* GetBacklight() { return &backlight_; }
    AudioCodec* GetAudioCodec() { return &codec_; }

    bool GetBatteryLevel(int& level, bool& charging, bool& discharging) {
        level = 78;
        charging = false;
        discharging = true;
        return true;
    }

private:
    Board() = default;

    Backlight backlight_;
    AudioCodec codec_;
};
