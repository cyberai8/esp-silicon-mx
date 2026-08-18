#pragma once

#include "backlight.h"

class AudioCodec;

class Board {
public:
    static Board& GetInstance();

    Backlight* GetBacklight() { return &backlight_; }
    AudioCodec* GetAudioCodec() { return &codec_; }

private:
    Board() = default;

    Backlight backlight_;
    AudioCodec codec_;
};
