#include "lip_sync_controller.h"

#include <algorithm>
#include <cinttypes>
#include <esp_log.h>

static const char* TAG = "LipSync";

LipSyncController::LipSyncController(OnVisemeChange cb)
    : cb_(std::move(cb)) {}

// ── 数据载入 ─────────────────────────────────────────────────────────────────

void LipSyncController::LoadTimeline(int utterance_index,
                                     std::vector<VisemeEvent> events) {
    std::lock_guard<std::mutex> lock(mu_);

    if (utterance_index < utterance_index_ && utterance_index != -1) {
        ESP_LOGW(TAG, "Stale viseme index=%d (current=%d), ignored",
                 utterance_index, utterance_index_);
        return;
    }

    // 保证时间轴末尾有 SIL 收嘴
    if (events.empty() || events.back().id != kVisemeSIL) {
        int last_t = events.empty() ? 0
                                    : events.back().time_ms + events.back().dur_ms;
        events.push_back({last_t, 120, kVisemeSIL, 0});
    }

    const bool same_utterance =
        utterance_index == utterance_index_ && utterance_index_ != -1;
    // 同句晚到/替换时间轴：若 DAC 已锚定则保留，避免嘴型整体漂移
    const bool keep_dac_anchor =
        same_utterance && anchor_set_ && !pending_dac_anchor_;

    utterance_index_ = utterance_index;
    events_          = std::move(events);
    if (!same_utterance) {
        anchor_samples_     = 0;
        anchor_set_         = false;
        pending_dac_anchor_ = false;
        current_id_         = kVisemeSIL;
        last_emit_id_       = kVisemeSIL;
    } else if (!keep_dac_anchor && !pending_dac_anchor_) {
        // 同句但尚未武装/锚定：交给后续 Arm / Auto
        anchor_samples_ = 0;
        anchor_set_     = false;
    }

    ESP_LOGI(TAG, "LoadTimeline index=%d events=%d replace=%d keep_dac=%d",
             utterance_index_, static_cast<int>(events_.size()),
             same_utterance ? 1 : 0, keep_dac_anchor ? 1 : 0);
}

void LipSyncController::ArmUtterance(int utterance_index) {
    std::lock_guard<std::mutex> lock(mu_);

    if (utterance_index != utterance_index_) {
        events_.clear();
        utterance_index_ = utterance_index;
        if (current_id_ != kVisemeSIL) {
            current_id_   = kVisemeSIL;
            last_emit_id_ = kVisemeSIL;
            if (cb_) {
                cb_(kVisemeSIL);
            }
        } else {
            last_emit_id_ = kVisemeSIL;
        }
    }

    pending_dac_anchor_ = true;
    anchor_set_         = false;
    anchor_samples_     = 0;
    ESP_LOGI(TAG, "ArmUtterance index=%d (wait DAC)", utterance_index_);
}

void LipSyncController::MarkAnchorIfArmed(int64_t played_samples_before_write) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!pending_dac_anchor_) {
        return;
    }
    pending_dac_anchor_ = false;
    anchor_samples_     = played_samples_before_write;
    anchor_set_         = true;
    ESP_LOGI(TAG, "MarkAnchorIfArmed samples=%" PRId64, anchor_samples_);
}

void LipSyncController::MarkAnchor(int64_t played_samples_at_first_audio) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!anchor_set_) {
        anchor_samples_ = played_samples_at_first_audio;
        anchor_set_     = true;
        ESP_LOGD(TAG, "MarkAnchor samples=%" PRId64, anchor_samples_);
    }
}

// ── 驱动 ─────────────────────────────────────────────────────────────────────

int LipSyncController::Tick(int64_t played_samples) {
    std::lock_guard<std::mutex> lock(mu_);

    if (events_.empty()) {
        return kVisemeSIL;
    }

    // 普通 TTS：Arm 后等首包 DAC，避免字幕晚到时 Auto-anchor 偏晚。
    // 演唱歌词轴：sentence_start 可能被跳过，或 Arm 之后 DAC 回调没赶上；
    // 已经有 events 就按当前播放采样锚定，否则嘴会一直停在 SIL。
    if (!anchor_set_) {
        if (pending_dac_anchor_ && events_.empty()) {
            return kVisemeSIL;
        }
        pending_dac_anchor_ = false;
        anchor_samples_ = played_samples;
        anchor_set_     = true;
        ESP_LOGD(TAG, "Auto MarkAnchor samples=%" PRId64, anchor_samples_);
    }

    // 计算相对句首的 ms
    const int64_t delta = played_samples - anchor_samples_;
    const int now_ms = (delta > 0)
                           ? static_cast<int>(delta * 1000 / kSampleRate)
                           : 0;

    current_id_ = LookupId(now_ms);

    if (current_id_ != last_emit_id_) {
        last_emit_id_ = current_id_;
        ESP_LOGD(TAG, "Viseme id=%d at %d ms", current_id_, now_ms);
        if (cb_) {
            cb_(current_id_);
        }
    }

    return current_id_;
}

// ── 控制 ─────────────────────────────────────────────────────────────────────

void LipSyncController::Reset() {
    std::lock_guard<std::mutex> lock(mu_);
    events_.clear();
    anchor_set_         = false;
    anchor_samples_     = 0;
    pending_dac_anchor_ = false;
    utterance_index_    = -1;

    if (current_id_ != kVisemeSIL) {
        current_id_   = kVisemeSIL;
        last_emit_id_ = kVisemeSIL;
        if (cb_) {
            cb_(kVisemeSIL);
        }
    }
    ESP_LOGD(TAG, "Reset -> SIL");
}

bool LipSyncController::HasTimeline() const {
    std::lock_guard<std::mutex> lock(mu_);
    return !events_.empty();
}

// ── 私有 ─────────────────────────────────────────────────────────────────────

int LipSyncController::LookupId(int now_ms) const {
    if (events_.empty()) return kVisemeSIL;

    // 找最后一个 time_ms <= now_ms 的事件
    int result_id = events_.front().id;
    for (const auto& ev : events_) {
        if (ev.time_ms > now_ms) break;
        result_id = ev.id;
    }
    return result_id;
}
