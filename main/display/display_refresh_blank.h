#pragma once

#include <cstdint>

#include "board.h"

// QSPI 360 用 PARTIAL 条带刷屏时，整页 invalidate 会从上往下露出来。
// 作用域内立刻关背光，析构时恢复；配合 lv_refr_now 可把扫屏藏在黑屏里。
class DisplayRefreshBlank {
public:
    DisplayRefreshBlank() {
        backlight_ = Board::GetInstance().GetBacklight();
        if (backlight_ == nullptr) {
            return;
        }
        previous_ = backlight_->brightness();
        if (previous_ > 0) {
            backlight_->SetBrightnessImmediately(0);
        }
    }

    ~DisplayRefreshBlank() {
        if (backlight_ != nullptr && previous_ > 0) {
            backlight_->SetBrightnessImmediately(previous_);
        }
    }

    DisplayRefreshBlank(const DisplayRefreshBlank&) = delete;
    DisplayRefreshBlank& operator=(const DisplayRefreshBlank&) = delete;

private:
    Backlight* backlight_ = nullptr;
    uint8_t previous_ = 0;
};
