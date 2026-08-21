#pragma once

#include <cstdint>
#include <vector>
#include <mutex>
#include <functional>

/**
 * LipSyncController
 *
 * 职责：
 *   1. 存储服务端下发的 viseme 时间轴（每句一份）。
 *   2. 在每帧 Tick（25 FPS）时，依据 played_samples_ 播放时钟
 *      查出当前应显示的 Viseme ID。
 *   3. 句末强制 SIL；打断时立即切 SIL。
 *
 * 使用方式：
 *   - tts.sentence_start → ArmUtterance(index)
 *   - 收到 type:viseme JSON → LoadTimeline()
 *   - AudioService 本句首帧 OutputData 前 → MarkAnchorIfArmed(played_before)
 *   - 渲染定时器每 40ms → Tick(played_samples)
 *   - AbortSpeaking / tts.stop → Reset()
 */

constexpr int kVisemeSIL = 0;
constexpr int kSampleRate = 24000;   // 设备采样率（Hz）

struct VisemeEvent {
    int time_ms  = 0;
    int dur_ms   = 120;
    int id       = kVisemeSIL;
    int blend_ms = 0;
};

class LipSyncController {
public:
    /** 回调：当 viseme id 改变时通知渲染层。参数为新的 viseme id（0=SIL）。 */
    using OnVisemeChange = std::function<void(int viseme_id)>;

    explicit LipSyncController(OnVisemeChange cb = nullptr);

    // ── 数据载入 ───────────────────────────────────────────────────────────────
    /**
     * 收到服务端 type:viseme JSON 时调用。
     * utterance_index：与服务端 index 字段对应，用于防止旧包覆盖新包。
     */
    void LoadTimeline(int utterance_index, std::vector<VisemeEvent> events);

    /**
     * 新句开始（tts.sentence_start）：武装 DAC 锚点。
     * 同 index 已有时间轴时保留 events，只重置锚点等待首包 DAC。
     */
    void ArmUtterance(int utterance_index);

    /**
     * 本句首帧写入 DAC 前调用（played_samples 为写入前计数）。
     * 仅在 ArmUtterance 后生效；用于避免字幕晚到时 Auto-anchor 系统性偏晚。
     */
    void MarkAnchorIfArmed(int64_t played_samples_before_write);

    /**
     * 手动锚点（兼容）；仅在尚未锚定时生效。
     */
    void MarkAnchor(int64_t played_samples_at_first_audio);

    // ── 驱动 ──────────────────────────────────────────────────────────────────
    /**
     * 每帧（~40ms）由渲染定时器调用。
     * played_samples：来自 AudioService::GetPlayedSamples()。
     * 返回当前 viseme id（0=SIL）。
     */
    int Tick(int64_t played_samples);

    // ── 控制 ──────────────────────────────────────────────────────────────────
    /** 打断 / tts.stop / tts.start 新句前调用；立即切 SIL。 */
    void Reset();

    bool HasTimeline() const;
    int CurrentVisemeId() const { return current_id_; }

private:
    int LookupId(int now_ms) const;  // 二分查 viseme 轴，返回 id

    OnVisemeChange cb_;

    mutable std::mutex mu_;
    int                 utterance_index_ = -1;
    std::vector<VisemeEvent> events_;
    int64_t             anchor_samples_  = 0;   // 本句首帧 OutputData 前的 played_samples
    bool                anchor_set_      = false;
    bool                pending_dac_anchor_ = false;  // Arm 后等待首包 DAC

    int current_id_   = kVisemeSIL;
    int last_emit_id_ = kVisemeSIL;
};
