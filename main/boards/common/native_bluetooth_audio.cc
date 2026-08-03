#include "native_bluetooth_audio.h"

#include "audio_codec.h"
#include "board.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
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
constexpr size_t kMinInternalFreeBeforeBtInit = 50000;
constexpr size_t kMinLargestBlockBeforeBtInit = 20000;

struct PcmBlock {
    uint8_t* data = nullptr;
    uint32_t len = 0;
};

struct BtState {
    bool initialized = false;
    bool output_active = false;
    bool avrc_initialized = false;
    bool a2dp_initialized = false;
    bool connected = false;
    bool playing = false;
    bool has_remote_bda = false;
    esp_bd_addr_t remote_bda = {0};
    int sample_rate = 48000;
    int channels = 2;
    uint8_t avrc_tl = 0;
    NativeBluetoothAudio::StateCallback state_callback = nullptr;
    NativeBluetoothAudio::MetadataCallback metadata_callback = nullptr;
    std::string title;
    std::string artist;
    std::string album;
    QueueHandle_t pcm_queue = nullptr;
    TaskHandle_t pcm_task = nullptr;
};

BtState s_state;

#if CONFIG_BT_ENABLED
extern "C" {
struct BtmPowerMode {
    uint16_t max;
    uint16_t min;
    uint16_t attempt;
    uint16_t timeout;
    uint8_t mode;
};

uint8_t BTM_SetLinkPolicy(uint8_t remote_bda[ESP_BD_ADDR_LEN],
                          uint16_t* settings);
uint8_t BTM_SetPowerMode(uint8_t pm_id, uint8_t remote_bda[ESP_BD_ADDR_LEN],
                         BtmPowerMode* mode);
}

constexpr uint16_t kBtLinkPolicyActiveOnly = 0x0000;
constexpr uint8_t kBtmPmSetOnlyId = 0x80;
constexpr uint8_t kBtmPmModeActive = 0x00;

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

void format_bda(const esp_bd_addr_t bda, char* out, size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return;
    }
    if (bda == nullptr) {
        std::snprintf(out, out_size, "(null)");
        return;
    }
    std::snprintf(out, out_size, "%02x:%02x:%02x:%02x:%02x:%02x",
                  bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

void keep_acl_active(const esp_bd_addr_t bda, const char* reason) {
    if (bda == nullptr) {
        return;
    }

    uint16_t policy = kBtLinkPolicyActiveOnly;
    const uint8_t policy_status = BTM_SetLinkPolicy(
        const_cast<uint8_t*>(bda), &policy);

    BtmPowerMode active_mode = {};
    active_mode.mode = kBtmPmModeActive;
    const uint8_t power_status = BTM_SetPowerMode(
        kBtmPmSetOnlyId, const_cast<uint8_t*>(bda), &active_mode);

    esp_err_t qos_err = esp_bt_gap_set_qos(
        const_cast<uint8_t*>(bda), ESP_BT_GAP_TPOLL_DFT);

    char bda_text[18];
    format_bda(bda, bda_text, sizeof(bda_text));
    ESP_LOGI(TAG,
             "Keep ACL active (%s): peer=%s policy=0x%04x status=%u power=%u qos=%s",
             reason != nullptr ? reason : "bt", bda_text, policy,
             policy_status, power_status, esp_err_to_name(qos_err));
}

void free_pcm_block(PcmBlock& block) {
    if (block.data != nullptr) {
        heap_caps_free(block.data);
        block.data = nullptr;
    }
    block.len = 0;
}

void delete_pcm_output() {
    if (s_state.pcm_task != nullptr) {
        vTaskDelete(s_state.pcm_task);
        s_state.pcm_task = nullptr;
    }
    if (s_state.pcm_queue != nullptr) {
        PcmBlock block;
        while (xQueueReceive(s_state.pcm_queue, &block, 0) == pdTRUE) {
            free_pcm_block(block);
        }
        vQueueDelete(s_state.pcm_queue);
        s_state.pcm_queue = nullptr;
    }
}

void drain_pcm_queue() {
    if (s_state.pcm_queue == nullptr) {
        return;
    }
    PcmBlock block;
    while (xQueueReceive(s_state.pcm_queue, &block, 0) == pdTRUE) {
        free_pcm_block(block);
    }
}

void cleanup_bt_stack() {
    delete_pcm_output();

    if (s_state.a2dp_initialized) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_a2d_sink_deinit());
        s_state.a2dp_initialized = false;
    }
    if (s_state.avrc_initialized) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_avrc_ct_deinit());
        s_state.avrc_initialized = false;
    }

    esp_bluedroid_status_t bluedroid_status = esp_bluedroid_get_status();
    if (bluedroid_status == ESP_BLUEDROID_STATUS_ENABLED) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_bluedroid_disable());
        bluedroid_status = esp_bluedroid_get_status();
    }
    if (bluedroid_status == ESP_BLUEDROID_STATUS_INITIALIZED) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_bluedroid_deinit());
    }

    esp_bt_controller_status_t controller_status = esp_bt_controller_get_status();
    if (controller_status == ESP_BT_CONTROLLER_STATUS_ENABLED) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_bt_controller_disable());
        controller_status = esp_bt_controller_get_status();
    }
    if (controller_status == ESP_BT_CONTROLLER_STATUS_INITED) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_bt_controller_deinit());
    }

    s_state.initialized = false;
    s_state.output_active = false;
    s_state.connected = false;
    s_state.playing = false;
    s_state.has_remote_bda = false;
    std::memset(s_state.remote_bda, 0, sizeof(s_state.remote_bda));
    s_state.title.clear();
    s_state.artist.clear();
    s_state.album.clear();
}

void notify_state_changed() {
    auto callback = s_state.state_callback;
    if (callback != nullptr) {
        callback(s_state.connected, s_state.playing);
    }
}

void notify_metadata_changed() {
    auto callback = s_state.metadata_callback;
    if (callback == nullptr) {
        return;
    }
    NativeBluetoothAudio::Metadata metadata = {
        s_state.title.c_str(),
        s_state.artist.c_str(),
        s_state.album.c_str(),
    };
    callback(metadata);
}

uint8_t next_avrc_tl() {
    return s_state.avrc_tl++ & 0x0f;
}

void request_metadata() {
    constexpr uint8_t kAttrMask = ESP_AVRC_MD_ATTR_TITLE |
                                  ESP_AVRC_MD_ATTR_ARTIST |
                                  ESP_AVRC_MD_ATTR_ALBUM;
    esp_err_t err = esp_avrc_ct_send_metadata_cmd(next_avrc_tl(), kAttrMask);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AVRCP metadata request failed: %s", esp_err_to_name(err));
    }
}

void register_track_notifications() {
    esp_err_t err = esp_avrc_ct_send_register_notification_cmd(
        next_avrc_tl(), ESP_AVRC_RN_TRACK_CHANGE, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AVRCP track notify register failed: %s", esp_err_to_name(err));
    }
    err = esp_avrc_ct_send_register_notification_cmd(
        next_avrc_tl(), ESP_AVRC_RN_PLAY_STATUS_CHANGE, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AVRCP play notify register failed: %s", esp_err_to_name(err));
    }
}

std::string metadata_text_to_string(const uint8_t* text, int length) {
    if (text == nullptr || length <= 0) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(text),
                       static_cast<size_t>(length));
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
    if (!s_state.output_active || s_state.pcm_queue == nullptr ||
        data == nullptr || len == 0) {
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
            if (s_state.connected) {
                std::memcpy(s_state.remote_bda, param->conn_stat.remote_bda,
                            sizeof(s_state.remote_bda));
                s_state.has_remote_bda = true;
                keep_acl_active(s_state.remote_bda, "a2dp connected");
            } else {
                s_state.playing = false;
                s_state.has_remote_bda = false;
                std::memset(s_state.remote_bda, 0, sizeof(s_state.remote_bda));
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
    switch (event) {
        case ESP_AVRC_CT_CONNECTION_STATE_EVT:
            ESP_LOGI(TAG, "AVRCP CT %s",
                     param->conn_stat.connected ? "connected" : "disconnected");
            if (param->conn_stat.connected) {
                request_metadata();
                register_track_notifications();
            } else {
                s_state.title.clear();
                s_state.artist.clear();
                s_state.album.clear();
                notify_metadata_changed();
            }
            break;
        case ESP_AVRC_CT_METADATA_RSP_EVT: {
            const std::string text = metadata_text_to_string(
                param->meta_rsp.attr_text, param->meta_rsp.attr_length);
            switch (param->meta_rsp.attr_id) {
                case ESP_AVRC_MD_ATTR_TITLE:
                    s_state.title = text;
                    break;
                case ESP_AVRC_MD_ATTR_ARTIST:
                    s_state.artist = text;
                    break;
                case ESP_AVRC_MD_ATTR_ALBUM:
                    s_state.album = text;
                    break;
                default:
                    break;
            }
            ESP_LOGI(TAG, "AVRCP metadata: attr=0x%02x text=%s",
                     param->meta_rsp.attr_id, text.c_str());
            notify_metadata_changed();
            break;
        }
        case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
            ESP_LOGI(TAG, "AVRCP notify: event=0x%02x",
                     param->change_ntf.event_id);
            if (param->change_ntf.event_id == ESP_AVRC_RN_TRACK_CHANGE ||
                param->change_ntf.event_id == ESP_AVRC_RN_NOW_PLAYING_CHANGE) {
                request_metadata();
                esp_avrc_ct_send_register_notification_cmd(
                    next_avrc_tl(), param->change_ntf.event_id, 0);
            } else if (param->change_ntf.event_id == ESP_AVRC_RN_PLAY_STATUS_CHANGE) {
                s_state.playing =
                    param->change_ntf.event_parameter.playback == ESP_AVRC_PLAYBACK_PLAYING;
                notify_state_changed();
                esp_avrc_ct_send_register_notification_cmd(
                    next_avrc_tl(), ESP_AVRC_RN_PLAY_STATUS_CHANGE, 0);
            }
            break;
        case ESP_AVRC_CT_PLAY_STATUS_RSP_EVT:
            s_state.playing =
                param->play_status_rsp.play_status == ESP_AVRC_PLAYBACK_PLAYING;
            notify_state_changed();
            break;
        default:
            break;
    }
}

void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
    switch (event) {
        case ESP_BT_GAP_MODE_CHG_EVT:
            ESP_LOGI(TAG, "GAP mode change: mode=%u interval=%u",
                     param->mode_chg.mode, param->mode_chg.interval);
            if (param->mode_chg.mode != ESP_BT_PM_MD_ACTIVE) {
                keep_acl_active(param->mode_chg.bda, "gap mode change");
            }
            break;
        case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT:
            ESP_LOGW(TAG, "GAP ACL disconnected: reason=0x%02x handle=0x%04x",
                     param->acl_disconn_cmpl_stat.reason,
                     param->acl_disconn_cmpl_stat.handle);
            break;
        default:
            break;
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

bool NativeBluetoothAudio::IsInitialized() const {
    return s_state.initialized;
}

const char* NativeBluetoothAudio::DeviceName() const {
    return kDeviceName;
}

bool NativeBluetoothAudio::Initialize() {
#if CONFIG_BT_ENABLED
    if (s_state.initialized) {
        return true;
    }

    ESP_LOGI(TAG, "BT init heap: internal free=%u largest=%u spiram free=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));

    const size_t internal_free =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (internal_free < kMinInternalFreeBeforeBtInit ||
        largest < kMinLargestBlockBeforeBtInit) {
        ESP_LOGE(TAG,
                 "Not enough internal heap for BT init: free=%u/%u largest=%u/%u",
                 static_cast<unsigned>(internal_free),
                 static_cast<unsigned>(kMinInternalFreeBeforeBtInit),
                 static_cast<unsigned>(largest),
                 static_cast<unsigned>(kMinLargestBlockBeforeBtInit));
        return false;
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_bt_controller_init failed: %s", esp_err_to_name(err));
        cleanup_bt_stack();
        return false;
    }
    err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_bt_controller_enable failed: %s", esp_err_to_name(err));
        cleanup_bt_stack();
        return false;
    }
    err = esp_bluedroid_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_bluedroid_init failed: %s", esp_err_to_name(err));
        cleanup_bt_stack();
        return false;
    }
    err = esp_bluedroid_enable();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_bluedroid_enable failed: %s", esp_err_to_name(err));
        cleanup_bt_stack();
        return false;
    }

    err = esp_bt_gap_set_device_name(kDeviceName);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_gap_set_device_name failed: %s", esp_err_to_name(err));
        cleanup_bt_stack();
        return false;
    }
    err = esp_bt_gap_register_callback(gap_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_gap_register_callback failed: %s", esp_err_to_name(err));
        cleanup_bt_stack();
        return false;
    }
    esp_bt_cod_t cod = {};
    cod.major = ESP_BT_COD_MAJOR_DEV_AV;
    cod.minor = 0x04;
    cod.service = ESP_BT_COD_SRVC_RENDERING | ESP_BT_COD_SRVC_AUDIO;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_bt_gap_set_cod(cod, ESP_BT_INIT_COD));

    err = esp_avrc_ct_register_callback(avrc_ct_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_avrc_ct_register_callback failed: %s", esp_err_to_name(err));
        cleanup_bt_stack();
        return false;
    }
    err = esp_avrc_ct_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_avrc_ct_init failed: %s", esp_err_to_name(err));
        cleanup_bt_stack();
        return false;
    }
    s_state.avrc_initialized = true;
    err = esp_a2d_register_callback(a2d_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_a2d_register_callback failed: %s", esp_err_to_name(err));
        cleanup_bt_stack();
        return false;
    }
    err = esp_a2d_sink_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_a2d_sink_init failed: %s", esp_err_to_name(err));
        cleanup_bt_stack();
        return false;
    }
    s_state.a2dp_initialized = true;
    err = esp_a2d_sink_register_data_callback(a2d_data_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_a2d_sink_register_data_callback failed: %s", esp_err_to_name(err));
        cleanup_bt_stack();
        return false;
    }

    s_state.pcm_queue = xQueueCreate(kPcmQueueDepth, sizeof(PcmBlock));
    if (s_state.pcm_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create PCM queue");
        cleanup_bt_stack();
        return false;
    }
    if (xTaskCreate(pcm_output_task, "bt_pcm_out", 4096, nullptr, 5,
                    &s_state.pcm_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create PCM output task");
        cleanup_bt_stack();
        return false;
    }

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
        s_state.output_active = true;
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE));
        return true;
    }

    ESP_LOGW(TAG, "Native BT audio-source mode UI is available, A2DP source stream is not wired yet");
    s_state.output_active = false;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE));
    return true;
#else
    (void)mode;
    return false;
#endif
}

void NativeBluetoothAudio::Suspend() {
#if CONFIG_BT_ENABLED
    if (!s_state.initialized) {
        return;
    }

    ESP_LOGI(TAG, "Suspending native Bluetooth audio");
    s_state.output_active = false;
    s_state.playing = false;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE,
                                                           ESP_BT_NON_DISCOVERABLE));
    drain_pcm_queue();
#endif
}

void NativeBluetoothAudio::Shutdown() {
#if CONFIG_BT_ENABLED
    if (!s_state.initialized) {
        cleanup_bt_stack();
        return;
    }

    ESP_LOGI(TAG, "Shutting down native Bluetooth audio");
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE,
                                                           ESP_BT_NON_DISCOVERABLE));
    cleanup_bt_stack();
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

    uint8_t tl = next_avrc_tl();
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

void NativeBluetoothAudio::SetMetadataCallback(MetadataCallback callback) {
    s_state.metadata_callback = callback;
#if CONFIG_BT_ENABLED
    if (callback != nullptr) {
        notify_metadata_changed();
    }
#endif
}

bool NativeBluetoothAudio::IsConnected() const {
    return s_state.connected;
}

bool NativeBluetoothAudio::IsPlaying() const {
    return s_state.playing;
}
