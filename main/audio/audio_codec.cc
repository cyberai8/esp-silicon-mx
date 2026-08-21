#include "audio_codec.h"
#include "board.h"
#include "settings.h"
#include "avatar_compositor.h"

#include <esp_log.h>
#include <cstring>
#include <driver/i2s_common.h>

#define TAG "AudioCodec"

AudioCodec::AudioCodec() {
}

AudioCodec::~AudioCodec() {
}

void AudioCodec::OutputData(std::vector<int16_t>& data) {
    std::lock_guard<std::mutex> lock(output_mutex_);
    const uint64_t pcm_start =
        played_samples_.load(std::memory_order_relaxed);
    Write(data.data(), data.size());
    const int channels = output_channels_ > 0 ? output_channels_ : 1;
    const size_t frames = data.size() / static_cast<size_t>(channels);
    if (frames > 0) {
        played_samples_.fetch_add(static_cast<uint64_t>(frames),
                                  std::memory_order_relaxed);
    }
    uint32_t peak = 0;
    for (int16_t sample : data) {
        int32_t v = sample;
        if (v < 0) {
            v = -v;
        }
        if (static_cast<uint32_t>(v) > peak) {
            peak = static_cast<uint32_t>(v);
        }
    }
    last_output_peak_.store(peak, std::memory_order_relaxed);
    if (frames > 0) {
        // 口型锚点：首包 PCM（含句首静音）写入 DAC 的时刻。
        AvatarCompositor::NotifyPcmOutput(pcm_start, peak);
    }
}

bool AudioCodec::InputData(std::vector<int16_t>& data) {
    std::lock_guard<std::mutex> lock(input_mutex_);
    int samples = Read(data.data(), data.size());
    if (samples > 0) {
        return true;
    }
    return false;
}

void AudioCodec::Start() {
    Settings settings("audio", false);
    output_volume_ = settings.GetInt("output_volume", output_volume_);
    if (output_volume_ <= 0) {
        ESP_LOGW(TAG, "Output volume value (%d) is too small, setting to default (10)", output_volume_);
        output_volume_ = 10;
    }

    if (tx_handle_ != nullptr) {
        ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    }

    if (rx_handle_ != nullptr) {
        ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
    }

    EnableInput(true);
    EnableOutput(true);
    ESP_LOGI(TAG, "Audio codec started");
}

void AudioCodec::SetOutputVolume(int volume) {
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }
    if (volume == output_volume_) {
        return;
    }
    output_volume_ = volume;
    ESP_LOGI(TAG, "Set output volume to %d", output_volume_);

    Settings settings("audio", true);
    settings.SetInt("output_volume", output_volume_);
}

void AudioCodec::SetInputGain(float gain) {
    input_gain_ = gain;
    ESP_LOGI(TAG, "Set input gain to %.1f", input_gain_);
}

void AudioCodec::EnableInput(bool enable) {
    if (enable == input_enabled_) {
        return;
    }
    input_enabled_ = enable;
    ESP_LOGI(TAG, "Set input enable to %s", enable ? "true" : "false");
}

void AudioCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) {
        return;
    }
    output_enabled_ = enable;
    ESP_LOGI(TAG, "Set output enable to %s", enable ? "true" : "false");
}
