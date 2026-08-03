#include "native_bluetooth_audio.h"

#include "audio_codec.h"
#include "board.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_BT_ENABLED
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#endif

namespace {

constexpr const char* TAG = "NativeBTAudio";
constexpr const char* kDeviceName = "Xingzhi-S31";
constexpr uint32_t kPcmQueueDepth = 8;

struct PcmBlock {
    uint8_t* data = nullptr;
    uint32_t len = 0;
};

struct BtState {
    bool initialized = false;
    bool connected = false;
    bool playing = false;
    int sample_rate = 48000;
    int channels = 2;
    uint8_t avrc_tl = 0;
    NativeBluetoothAudio::StateCallback state_callback = nullptr;
    QueueHandle_t pcm_queue = nullptr;
    TaskHandle_t pcm_task = nullptr;
};

BtState s_state;

#if CONFIG_BT_ENABLED
int sample_rate_from_sbc(uint8_t samp_freq) {
    if (samp_freq & ESP_A2D_SBC_CIE_SF_48K) {
        return 48000;
    }
    if (samp_freq & ESP_A2D_SBC_CIE_SF_32K) {
        return 32000;
    }
    if (samp_freq & ESP_A2D_SBC_CIE_SF_16K) {
        return 16000;
    }
    if (samp_freq & ESP_A2D_SBC_CIE_SF_44K) {
        return 44100;
    }
    return 48000;
}

int channels_from_sbc(uint8_t ch_mode) {
    return (ch_mode & ESP_A2D_SBC_CIE_CH_MODE_MONO) ? 1 : 2;
}

void free_pcm_block(PcmBlock& block) {
    if (block.data != nullptr) {
        heap_caps_free(block.data);
        block.data = nullptr;
    }
    block.len = 0;
}

void notify_state_changed() {
    auto callback = s_state.state_callback;
    if (callback != nullptr) {
        callback(s_state.connected, s_state.playing);
    }
}

std::vector<int16_t> resample_mono_linear(const std::vector<int16_t>& input,
                                          int input_rate, int output_rate) {
    if (input.empty() || input_rate <= 0 || output_rate <= 0 || input_rate == output_rate) {
        return input;
    }

    const size_t output_samples =
        std::max<size_t>(1, (static_cast<uint64_t>(input.size()) * output_rate) / input_rate);
    std::vector<int16_t> output(output_samples);
    for (size_t i = 0; i < output_samples; ++i) {
        const uint64_t pos_q16 =
            (static_cast<uint64_t>(i) * static_cast<uint64_t>(input_rate) << 16) /
            static_cast<uint64_t>(output_rate);
        const size_t idx = std::min<size_t>(pos_q16 >> 16, input.size() - 1);
        const size_t next = std::min<size_t>(idx + 1, input.size() - 1);
        const int32_t frac = static_cast<int32_t>(pos_q16 & 0xffff);
        const int32_t a = input[idx];
        const int32_t b = input[next];
        output[i] = static_cast<int16_t>(a + (((b - a) * frac) >> 16));
    }
    return output;
}

void pcm_output_task(void* /*arg*/) {
    PcmBlock block;
    while (true) {
        if (xQueueReceive(s_state.pcm_queue, &block, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        AudioCodec* codec = Board::GetInstance().GetAudioCodec();
        if (codec == nullptr || block.data == nullptr || block.len == 0) {
            free_pcm_block(block);
            continue;
        }

        if (!codec->output_enabled()) {
            codec->EnableOutput(true);
        }

        const int16_t* in = reinterpret_cast<const int16_t*>(block.data);
        const int input_samples = block.len / sizeof(int16_t);
        const int frames = input_samples / std::max(1, s_state.channels);
        std::vector<int16_t> mono(frames);
        if (s_state.channels == 1) {
            std::memcpy(mono.data(), in, frames * sizeof(int16_t));
        } else {
            for (int i = 0; i < frames; ++i) {
                int32_t mixed = static_cast<int32_t>(in[i * 2]) + static_cast<int32_t>(in[i * 2 + 1]);
                mono[i] = static_cast<int16_t>(mixed / 2);
            }
        }

        auto output = resample_mono_linear(mono, s_state.sample_rate, codec->output_sample_rate());
        codec->OutputData(output);

        free_pcm_block(block);
    }
}

void a2d_data_cb(const uint8_t* data, uint32_t len) {
    if (s_state.pcm_queue == nullptr || data == nullptr || len == 0) {
        return;
    }
    PcmBlock block;
    block.data = static_cast<uint8_t*>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (block.data == nullptr) {
        block.data = static_cast<uint8_t*>(heap_caps_malloc(len, MALLOC_CAP_8BIT));
    }
    if (block.data == nullptr) {
        ESP_LOGW(TAG, "Drop BT PCM, no memory (%u bytes)", (unsigned)len);
        return;
    }
    std::memcpy(block.data, data, len);
    block.len = len;
    if (xQueueSend(s_state.pcm_queue, &block, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Drop BT PCM, output queue full");
        free_pcm_block(block);
    }
}

void a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t* param) {
    switch (event) {
        case ESP_A2D_CONNECTION_STATE_EVT:
            s_state.connected = param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED;
            if (!s_state.connected) {
                s_state.playing = false;
            }
            ESP_LOGI(TAG, "A2DP %s", s_state.connected ? "connected" : "disconnected");
            notify_state_changed();
            break;
        case ESP_A2D_AUDIO_STATE_EVT:
            s_state.playing = param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED;
            ESP_LOGI(TAG, "A2DP audio %s", s_state.playing ? "started" : "suspended");
            notify_state_changed();
            break;
        case ESP_A2D_AUDIO_CFG_EVT:
            if (param->audio_cfg.mcc.type == ESP_A2D_MCT_SBC) {
                s_state.sample_rate = sample_rate_from_sbc(param->audio_cfg.mcc.cie.sbc_info.samp_freq);
                s_state.channels = channels_from_sbc(param->audio_cfg.mcc.cie.sbc_info.ch_mode);
                ESP_LOGI(TAG, "A2DP SBC cfg: %d Hz, %d ch", s_state.sample_rate, s_state.channels);
            }
            break;
        case ESP_A2D_PROF_STATE_EVT:
            ESP_LOGI(TAG, "A2DP profile state=%d", param->a2d_prof_stat.init_state);
            break;
        default:
            break;
    }
}

void avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t* param) {
    if (event == ESP_AVRC_CT_CONNECTION_STATE_EVT) {
        ESP_LOGI(TAG, "AVRCP CT %s", param->conn_stat.connected ? "connected" : "disconnected");
    }
}
#endif

}  // namespace

NativeBluetoothAudio& NativeBluetoothAudio::GetInstance() {
    static NativeBluetoothAudio instance;
    return instance;
}

bool NativeBluetoothAudio::IsSupported() const {
#if CONFIG_BT_ENABLED
    return true;
#else
    return false;
#endif
}

const char* NativeBluetoothAudio::DeviceName() const {
    return kDeviceName;
}

bool NativeBluetoothAudio::Initialize() {
#if CONFIG_BT_ENABLED
    if (s_state.initialized) {
        return true;
    }

    s_state.pcm_queue = xQueueCreate(kPcmQueueDepth, sizeof(PcmBlock));
    if (s_state.pcm_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create PCM queue");
        return false;
    }
    xTaskCreate(pcm_output_task, "bt_pcm_out", 4096, nullptr, 5, &s_state.pcm_task);

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_bt_controller_init failed: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_bt_controller_enable failed: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_bluedroid_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_bluedroid_init failed: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_bluedroid_enable();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_bluedroid_enable failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_bt_gap_set_device_name(kDeviceName));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_avrc_ct_register_callback(avrc_ct_cb));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_avrc_ct_init());
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_a2d_register_callback(a2d_cb));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_a2d_sink_init());
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_a2d_sink_register_data_callback(a2d_data_cb));

    s_state.initialized = true;
    ESP_LOGI(TAG, "Native Bluetooth A2DP sink initialized as %s", kDeviceName);
    return true;
#else
    ESP_LOGW(TAG, "Native Bluetooth is not enabled in sdkconfig");
    return false;
#endif
}

bool NativeBluetoothAudio::SetMode(Mode mode) {
#if CONFIG_BT_ENABLED
    if (!Initialize()) {
        return false;
    }
    if (mode == Mode::kSpeakerSink) {
        ESP_LOGI(TAG, "Set native BT speaker/sink mode");
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE));
        return true;
    }

    ESP_LOGW(TAG, "Native BT audio-source mode UI is available, A2DP source stream is not wired yet");
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE));
    return true;
#else
    (void)mode;
    return false;
#endif
}

bool NativeBluetoothAudio::SendCommand(Command command) {
#if CONFIG_BT_ENABLED
    if (!s_state.initialized) {
        return false;
    }
    uint8_t key = ESP_AVRC_PT_CMD_PLAY;
    switch (command) {
        case Command::kPlay:
            key = ESP_AVRC_PT_CMD_PLAY;
            break;
        case Command::kPause:
            key = ESP_AVRC_PT_CMD_PAUSE;
            break;
        case Command::kPrevious:
            key = ESP_AVRC_PT_CMD_BACKWARD;
            break;
        case Command::kNext:
            key = ESP_AVRC_PT_CMD_FORWARD;
            break;
        case Command::kVolumeDown:
            key = ESP_AVRC_PT_CMD_VOL_DOWN;
            break;
        case Command::kVolumeUp:
            key = ESP_AVRC_PT_CMD_VOL_UP;
            break;
    }

    uint8_t tl = s_state.avrc_tl++ & 0x0f;
    esp_err_t err = esp_avrc_ct_send_passthrough_cmd(tl, key, ESP_AVRC_PT_CMD_STATE_PRESSED);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(80));
        err = esp_avrc_ct_send_passthrough_cmd(tl, key, ESP_AVRC_PT_CMD_STATE_RELEASED);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AVRCP command %u failed: %s", key, esp_err_to_name(err));
        return false;
    }
    return true;
#else
    (void)command;
    return false;
#endif
}

void NativeBluetoothAudio::SetStateCallback(StateCallback callback) {
    s_state.state_callback = callback;
}

bool NativeBluetoothAudio::IsConnected() const {
    return s_state.connected;
}

bool NativeBluetoothAudio::IsPlaying() const {
    return s_state.playing;
}
