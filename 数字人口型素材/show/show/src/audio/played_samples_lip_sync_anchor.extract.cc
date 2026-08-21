// EXTRACT from voiceshow/main/audio/audio_service.cc (+ .h 声明摘要)
// 喇叭播放时钟：MarkAnchorIfArmed + played_samples_ + playback_env_

/* --- audio_service.h (相关声明) --- */
Mouth fallback when no viseme timeline exists (e.g. AI singing); callers
     * normalize against their own running peak, so this stays unclipped.
     */
    uint16_t GetPlaybackEnvelope() const {
        return playback_env_.load(std::memory_order_relaxed);
    }

    /**
     * 真实播放采样数（24 kHz mono）。
     * 每次 OutputData 成功写入 DAC 后累加；ResetDecoder 时归零。
     * 供 LipSyncController 作唯一播放时钟。
     */
    int64_t GetPlayedSamples() const { return played_samples_.load(std::memory_order_relaxed); }

    /** 句首锚点：ResetDecoder 时归零 played_samples_。 */
    void ResetPlayedSamples() { played_samples_.store(0, std::memory_order_relaxed); }

std::atomic<int64_t> played_samples_{0};  // 已写入 DAC 的 PCM 采样总数（24 kHz）

/* --- audio_service.cc (DAC 输出路径) --- */
_volume);
        }
        */

        // 本句首包写入 DAC 前锚定口型时钟（played 为写入前计数）
        {
            const int64_t played_before =
                played_samples_.load(std::memory_order_relaxed);
            Application::GetInstance().GetLipSyncController().MarkAnchorIfArmed(
                played_before);
        }
        // Loudness of what is actually going to the DAC; drives the mouth when
        // no viseme timeline exists (AI singing) or before it arrives.
        {
            const size_t n = task->pcm.size();
            if (n == 0) {
                playback_env_.store(0, std::memory_order_relaxed);
            } else {
                int64_t sum = 0;
                for (size_t i = 0; i < n; ++i) {
                    const int32_t s = task->pcm[i];
                    sum += (s >= 0) ? s : -s;
                }
                playback_env_.store(
                    static_cast<uint16_t>(sum / static_cast<int64_t>(n)),
                    std::memory_order_relaxed);
            }
        }
        codec_->OutputData(task->pcm);
        played_samples_.fetch_add(static_cast<int64_t>(task->pcm.size()), std::memory_order_relaxed);

