#pragma once

#include <cstdint>

namespace lipsync {

constexpr int kPlaybackSampleRateHz = 24000;
constexpr int kTickPeriodMs = 40;

/** Viseme ID set zh_15 (see show.md). */
enum VisemeId : int8_t {
    kSil = 0,
    kPp = 1,
    kFf = 2,
    kTh = 3,
    kDd = 4,
    kKk = 5,
    kCh = 6,
    kSs = 7,
    kNn = 8,
    kRr = 9,
    kAa = 10,
    kE = 11,
    kI = 12,
    kO = 13,
    kU = 14,
};

}  // namespace lipsync
