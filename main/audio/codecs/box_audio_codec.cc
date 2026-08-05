#include "box_audio_codec.h"

#include <esp_log.h>
#include <esp_codec_dev.h>
#include <sdkconfig.h>
#include <driver/i2c_master.h>
#include <driver/i2s_tdm.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <array>
#include <cinttypes>
#include <vector>

#define TAG "BoxAudioCodec"

BoxAudioCodec::BoxAudioCodec(void* i2c_master_handle, int input_sample_rate, int output_sample_rate,
    gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
    gpio_num_t pa_pin, uint8_t es8311_addr, uint8_t es7210_addr, bool input_reference) {
    duplex_ = true; // 是否双工
    input_reference_ = input_reference; // 是否使用参考输入，实现回声消除
    input_channels_ = input_reference_ ? 2 : 1; // 输入通道数
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    input_gain_ = 30;

#if CONFIG_BOARD_TYPE_ESP_VOCAT
    i2c_master_handle_ = i2c_master_handle;
    mclk_ = mclk;
    bclk_ = bclk;
    ws_ = ws;
    dout_ = dout;
    din_ = din;
    pa_pin_ = pa_pin;
    es8311_addr_ = es8311_addr;
    es7210_addr_ = es7210_addr;

    if (!CreateCodecDevicesLocked()) {
        ESP_LOGE(TAG, "Failed to initialize BoxAudioDevice");
        abort();
    }
#else
    CreateDuplexChannels(mclk, bclk, ws, dout, din);

    // Do initialize of related interface: data_if, ctrl_if and gpio_if
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx_handle_,
        .tx_handle = tx_handle_,
    };
    data_if_ = audio_codec_new_i2s_data(&i2s_cfg);
    assert(data_if_ != NULL);

    // Output
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = (i2c_port_t)1,
        .addr = es8311_addr,
        .bus_handle = i2c_master_handle,
    };
    out_ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(out_ctrl_if_ != NULL);

    gpio_if_ = audio_codec_new_gpio();
    assert(gpio_if_ != NULL);

    es8311_codec_cfg_t es8311_cfg = {};
    es8311_cfg.ctrl_if = out_ctrl_if_;
    es8311_cfg.gpio_if = gpio_if_;
    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    es8311_cfg.pa_pin = pa_pin;
    es8311_cfg.use_mclk = true;
    es8311_cfg.hw_gain.pa_voltage = 5.0;
    es8311_cfg.hw_gain.codec_dac_voltage = 3.3;
    out_codec_if_ = es8311_codec_new(&es8311_cfg);
    assert(out_codec_if_ != NULL);

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = out_codec_if_,
        .data_if = data_if_,
    };
    output_dev_ = esp_codec_dev_new(&dev_cfg);
    assert(output_dev_ != NULL);

    // Input
    i2c_cfg.addr = es7210_addr;
    in_ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(in_ctrl_if_ != NULL);

    es7210_codec_cfg_t es7210_cfg = {};
    es7210_cfg.ctrl_if = in_ctrl_if_;
    es7210_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4;
    in_codec_if_ = es7210_codec_new(&es7210_cfg);
    assert(in_codec_if_ != NULL);

    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
    dev_cfg.codec_if = in_codec_if_;
    input_dev_ = esp_codec_dev_new(&dev_cfg);
    assert(input_dev_ != NULL);

    ESP_LOGI(TAG, "BoxAudioDevice initialized");
#endif
}

BoxAudioCodec::~BoxAudioCodec() {
#if CONFIG_BOARD_TYPE_ESP_VOCAT
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    DeleteCodecDevicesLocked();
#else
    ESP_ERROR_CHECK(esp_codec_dev_close(output_dev_));
    esp_codec_dev_delete(output_dev_);
    ESP_ERROR_CHECK(esp_codec_dev_close(input_dev_));
    esp_codec_dev_delete(input_dev_);

    audio_codec_delete_codec_if(in_codec_if_);
    audio_codec_delete_ctrl_if(in_ctrl_if_);
    audio_codec_delete_codec_if(out_codec_if_);
    audio_codec_delete_ctrl_if(out_ctrl_if_);
    audio_codec_delete_gpio_if(gpio_if_);
    audio_codec_delete_data_if(data_if_);
#endif
}

void BoxAudioCodec::CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din) {
    assert(input_sample_rate_ == output_sample_rate_);

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };

    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)input_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            .bclk_div = 8,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = i2s_tdm_slot_mask_t(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
            .ws_width = I2S_TDM_AUTO_WS_WIDTH,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = false,
            .big_endian = false,
            .bit_order_lsb = false,
            .skip_mask = false,
            .total_slot = I2S_TDM_AUTO_SLOT_NUM
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = I2S_GPIO_UNUSED,
            .din = din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(rx_handle_, &tdm_cfg));
    ESP_LOGI(TAG, "Duplex channels created");
}

#if CONFIG_BOARD_TYPE_ESP_VOCAT
bool BoxAudioCodec::CreateCodecDevicesLocked() {
    CreateDuplexChannels(mclk_, bclk_, ws_, dout_, din_);

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx_handle_,
        .tx_handle = tx_handle_,
    };
    data_if_ = audio_codec_new_i2s_data(&i2s_cfg);
    if (data_if_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create I2S data interface");
        return false;
    }

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = (i2c_port_t)1,
        .addr = es8311_addr_,
        .bus_handle = i2c_master_handle_,
    };
    out_ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (out_ctrl_if_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create ES8311 control interface");
        return false;
    }

    gpio_if_ = audio_codec_new_gpio();
    if (gpio_if_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create codec GPIO interface");
        return false;
    }

    es8311_codec_cfg_t es8311_cfg = {};
    es8311_cfg.ctrl_if = out_ctrl_if_;
    es8311_cfg.gpio_if = gpio_if_;
    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    es8311_cfg.pa_pin = pa_pin_;
    es8311_cfg.use_mclk = true;
    es8311_cfg.hw_gain.pa_voltage = 5.0;
    es8311_cfg.hw_gain.codec_dac_voltage = 3.3;
    out_codec_if_ = es8311_codec_new(&es8311_cfg);
    if (out_codec_if_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create ES8311 codec");
        return false;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = out_codec_if_,
        .data_if = data_if_,
    };
    output_dev_ = esp_codec_dev_new(&dev_cfg);
    if (output_dev_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create output codec device");
        return false;
    }

    i2c_cfg.addr = es7210_addr_;
    in_ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (in_ctrl_if_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create ES7210 control interface");
        return false;
    }

    es7210_codec_cfg_t es7210_cfg = {};
    es7210_cfg.ctrl_if = in_ctrl_if_;
    es7210_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 |
                              ES7210_SEL_MIC3 | ES7210_SEL_MIC4;
    in_codec_if_ = es7210_codec_new(&es7210_cfg);
    if (in_codec_if_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create ES7210 codec");
        return false;
    }

    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
    dev_cfg.codec_if = in_codec_if_;
    input_dev_ = esp_codec_dev_new(&dev_cfg);
    if (input_dev_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create input codec device");
        return false;
    }

    tdm_slot_map_ready_ = false;
    tdm_slot0_ = 0;
    tdm_slot1_ = 1;
    tdm_probe_reads_left_ = 12;
    tdm_probe_log_decimate_ = 0;
    input_read_fail_count_ = 0;
    last_input_recovery_tick_ = 0;
    input_recovery_in_progress_ = false;
    ESP_LOGI(TAG, "BoxAudioDevice initialized");
    return true;
}

void BoxAudioCodec::DeleteCodecDevicesLocked() {
    AudioCodec::EnableInput(false);
    AudioCodec::EnableOutput(false);

    if (input_dev_ != nullptr) {
        esp_codec_dev_close(input_dev_);
        esp_codec_dev_delete(input_dev_);
        input_dev_ = nullptr;
    }
    if (output_dev_ != nullptr) {
        esp_codec_dev_close(output_dev_);
        esp_codec_dev_delete(output_dev_);
        output_dev_ = nullptr;
    }
    if (in_codec_if_ != nullptr) {
        audio_codec_delete_codec_if(in_codec_if_);
        in_codec_if_ = nullptr;
    }
    if (out_codec_if_ != nullptr) {
        audio_codec_delete_codec_if(out_codec_if_);
        out_codec_if_ = nullptr;
    }
    if (in_ctrl_if_ != nullptr) {
        audio_codec_delete_ctrl_if(in_ctrl_if_);
        in_ctrl_if_ = nullptr;
    }
    if (out_ctrl_if_ != nullptr) {
        audio_codec_delete_ctrl_if(out_ctrl_if_);
        out_ctrl_if_ = nullptr;
    }
    if (gpio_if_ != nullptr) {
        audio_codec_delete_gpio_if(gpio_if_);
        gpio_if_ = nullptr;
    }
    if (data_if_ != nullptr) {
        audio_codec_delete_data_if(data_if_);
        data_if_ = nullptr;
    }
    if (rx_handle_ != nullptr) {
        esp_err_t ret = i2s_channel_disable(rx_handle_);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGD(TAG, "I2S rx disable during delete failed: %s",
                     esp_err_to_name(ret));
        }
        ESP_ERROR_CHECK_WITHOUT_ABORT(i2s_del_channel(rx_handle_));
        rx_handle_ = nullptr;
    }
    if (tx_handle_ != nullptr) {
        esp_err_t ret = i2s_channel_disable(tx_handle_);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGD(TAG, "I2S tx disable during delete failed: %s",
                     esp_err_to_name(ret));
        }
        ESP_ERROR_CHECK_WITHOUT_ABORT(i2s_del_channel(tx_handle_));
        tx_handle_ = nullptr;
    }
}
#endif

void BoxAudioCodec::SetOutputVolume(int volume) {
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(output_dev_, volume));
    AudioCodec::SetOutputVolume(volume);
}

bool BoxAudioCodec::OpenOutputDeviceLocked() {
    if (output_enabled_) {
        return true;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = (uint32_t)output_sample_rate_,
        .mclk_multiple = 0,
    };
    if (esp_codec_dev_open(output_dev_, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Failed to open output device");
        return false;
    }
    if (esp_codec_dev_set_out_vol(output_dev_, output_volume_) !=
        ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Failed to set output volume");
        esp_codec_dev_close(output_dev_);
        return false;
    }
    AudioCodec::EnableOutput(true);
    return true;
}

bool BoxAudioCodec::OpenInputDeviceLocked() {
    if (input_enabled_) {
        return true;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 4,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
        .sample_rate = (uint32_t)output_sample_rate_,
        .mclk_multiple = 0,
    };
#if CONFIG_BOARD_TYPE_ESP_VOCAT
    if (input_reference_) {
        fs.channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) |
                          ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
    } else {
        fs.channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) |
                          ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1) |
                          ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2) |
                          ESP_CODEC_DEV_MAKE_CHANNEL_MASK(3);
    }
#else
    if (input_reference_) {
        fs.channel_mask |= ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
    }
#endif

    if (esp_codec_dev_open(input_dev_, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Failed to open input device");
        return false;
    }
#if CONFIG_BOARD_TYPE_ESP_VOCAT
    if (esp_codec_dev_set_in_gain(input_dev_, input_gain_) !=
        ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Failed to set input gain");
        esp_codec_dev_close(input_dev_);
        return false;
    }
    input_read_fail_count_ = 0;
    ESP_LOGI(TAG, "Input device opened: ch=%d mask=0x%x rate=%" PRIu32
                  " app_ch=%d ref=%d",
             fs.channel, fs.channel_mask, fs.sample_rate, input_channels_,
             input_reference_ ? 1 : 0);
#else
    if (esp_codec_dev_set_in_channel_gain(input_dev_,
                                          ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
                                          input_gain_) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Failed to set input channel gain");
        esp_codec_dev_close(input_dev_);
        return false;
    }
#endif
    AudioCodec::EnableInput(true);
    return true;
}

void BoxAudioCodec::CloseInputDeviceLocked() {
    if (input_dev_ != nullptr &&
        esp_codec_dev_close(input_dev_) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "Failed to close input device");
    }
}

void BoxAudioCodec::CloseOutputDeviceLocked() {
    if (output_dev_ != nullptr &&
        esp_codec_dev_close(output_dev_) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "Failed to close output device");
    }
}

#if CONFIG_BOARD_TYPE_ESP_VOCAT
void BoxAudioCodec::HandleReadFailureLocked(esp_err_t ret, int samples) {
    input_read_fail_count_++;
    ESP_LOGW(TAG, "esp_codec_dev_read failed: %s (0x%x) samples=%d fail_count=%d",
             esp_err_to_name(ret), ret, samples, input_read_fail_count_);
    if (input_read_fail_count_ < 3) {
        return;
    }

    const TickType_t now = xTaskGetTickCount();
    if (last_input_recovery_tick_ != 0 &&
        now - last_input_recovery_tick_ < pdMS_TO_TICKS(5000)) {
        return;
    }
    last_input_recovery_tick_ = now;
    input_read_fail_count_ = 0;
    RecoverInputStreamLocked();
}

bool BoxAudioCodec::RecoverInputStreamLocked() {
    if (input_recovery_in_progress_) {
        return false;
    }
    input_recovery_in_progress_ = true;
    ESP_LOGW(TAG, "Recover ES7210 input stream after repeated read timeouts");

    const bool reopen_output = output_enabled_;
    if (input_dev_ != nullptr) {
        esp_codec_dev_close(input_dev_);
    }
    if (reopen_output && output_dev_ != nullptr) {
        esp_codec_dev_close(output_dev_);
        AudioCodec::EnableOutput(false);
    }
    AudioCodec::EnableInput(false);

    if (rx_handle_ != nullptr) {
        esp_err_t err = i2s_channel_disable(rx_handle_);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "Disable RX during recovery failed: %s",
                     esp_err_to_name(err));
        }
    }
    if (tx_handle_ != nullptr) {
        esp_err_t err = i2s_channel_disable(tx_handle_);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "Disable TX during recovery failed: %s",
                     esp_err_to_name(err));
        }
    }
    vTaskDelay(pdMS_TO_TICKS(60));
    if (tx_handle_ != nullptr) {
        esp_err_t err = i2s_channel_enable(tx_handle_);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "Enable TX during recovery failed: %s",
                     esp_err_to_name(err));
        }
    }
    if (rx_handle_ != nullptr) {
        esp_err_t err = i2s_channel_enable(rx_handle_);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "Enable RX during recovery failed: %s",
                     esp_err_to_name(err));
        }
    }
    vTaskDelay(pdMS_TO_TICKS(60));

    bool ok = OpenInputDeviceLocked();
    if (ok && reopen_output) {
        ok = OpenOutputDeviceLocked();
    }
    input_recovery_in_progress_ = false;
    ESP_LOGW(TAG, "Recover ES7210 input stream %s", ok ? "done" : "failed");
    return ok;
}
#endif

void BoxAudioCodec::EnableInput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == input_enabled_) {
        return;
    }
    if (enable) {
        if (!OpenInputDeviceLocked()) {
            return;
        }
    } else {
        CloseInputDeviceLocked();
        AudioCodec::EnableInput(false);
    }
}

void BoxAudioCodec::EnableOutput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == output_enabled_) {
        return;
    }
    if (enable) {
        if (!OpenOutputDeviceLocked()) {
            return;
        }
    } else {
        CloseOutputDeviceLocked();
        AudioCodec::EnableOutput(false);
    }
}

int BoxAudioCodec::Read(int16_t* dest, int samples) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (input_enabled_ && input_dev_ != nullptr) {
#if CONFIG_BOARD_TYPE_ESP_VOCAT
        if (!input_reference_ && (input_channels_ == 1 || input_channels_ == 2)) {
            const int want_ch = input_channels_;
            if (samples % want_ch != 0) {
                ESP_LOGW(TAG, "Read samples not divisible: samples=%d ch=%d",
                         samples, want_ch);
                return 0;
            }

            const int per_ch = samples / want_ch;
            std::vector<int16_t> tdm(static_cast<size_t>(per_ch) * 4);
            const esp_err_t ret = esp_codec_dev_read(input_dev_, tdm.data(),
                                                     tdm.size() * sizeof(int16_t));
            if (ret != ESP_OK) {
                HandleReadFailureLocked(ret, samples);
                return 0;
            }
            input_read_fail_count_ = 0;

            if (!tdm_slot_map_ready_ && tdm_probe_reads_left_ > 0) {
                std::array<uint64_t, 4> energy = {0, 0, 0, 0};
                for (int i = 0; i < per_ch; ++i) {
                    for (int ch = 0; ch < 4; ++ch) {
                        int16_t sample = tdm[static_cast<size_t>(i) * 4 + ch];
                        energy[ch] += sample >= 0 ? sample : -sample;
                    }
                }

                uint8_t best0 = 0;
                uint8_t best1 = 1;
                for (uint8_t ch = 1; ch < 4; ++ch) {
                    if (energy[ch] > energy[best0]) {
                        best0 = ch;
                    }
                }
                best1 = best0 == 0 ? 1 : 0;
                for (uint8_t ch = 0; ch < 4; ++ch) {
                    if (ch != best0 && energy[ch] > energy[best1]) {
                        best1 = ch;
                    }
                }

                tdm_slot0_ = best0;
                tdm_slot1_ = best1;
                tdm_probe_reads_left_--;
                tdm_probe_log_decimate_++;
                if ((tdm_probe_log_decimate_ % 4) == 0 ||
                    tdm_probe_reads_left_ <= 0) {
                    ESP_LOGI(TAG,
                             "TDM probe: left=%d pick=%u/%u e=%" PRIu64
                             ",%" PRIu64 ",%" PRIu64 ",%" PRIu64,
                             tdm_probe_reads_left_, (unsigned)tdm_slot0_,
                             (unsigned)tdm_slot1_, energy[0], energy[1],
                             energy[2], energy[3]);
                }
                if (tdm_probe_reads_left_ <= 0) {
                    tdm_slot_map_ready_ = true;
                    ESP_LOGI(TAG,
                             "TDM slot map locked: slot0=%u slot1=%u",
                             (unsigned)tdm_slot0_, (unsigned)tdm_slot1_);
                }
            }

            const uint8_t slot0 = tdm_slot_map_ready_ ? tdm_slot0_ : 0;
            const uint8_t slot1 = tdm_slot_map_ready_ ? tdm_slot1_ : 1;
            for (int i = 0; i < per_ch; ++i) {
                if (want_ch == 1) {
                    dest[i] = tdm[static_cast<size_t>(i) * 4 + slot0];
                } else {
                    dest[static_cast<size_t>(i) * 2 + 0] =
                        tdm[static_cast<size_t>(i) * 4 + slot0];
                    dest[static_cast<size_t>(i) * 2 + 1] =
                        tdm[static_cast<size_t>(i) * 4 + slot1];
                }
            }
            return samples;
        }
#endif
        const esp_err_t ret = esp_codec_dev_read(input_dev_, dest,
                                                 samples * sizeof(int16_t));
        if (ret != ESP_OK) {
#if CONFIG_BOARD_TYPE_ESP_VOCAT
            HandleReadFailureLocked(ret, samples);
#else
            ESP_ERROR_CHECK_WITHOUT_ABORT(ret);
#endif
            return 0;
        }
#if CONFIG_BOARD_TYPE_ESP_VOCAT
        input_read_fail_count_ = 0;
#endif
    }
    return samples;
}

int BoxAudioCodec::Write(const int16_t* data, int samples) {
    if (!output_enabled_ || output_dev_ == nullptr) {
        return samples;
    }
    if (data == nullptr || samples <= 0) {
        ESP_LOGW(TAG, "Invalid write args: data=%p samples=%d", data, samples);
        return samples;
    }

    const esp_err_t ret = esp_codec_dev_write(output_dev_, (void*)data,
                                             samples * sizeof(int16_t));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_codec_dev_write failed: %s (0x%x)",
                 esp_err_to_name(ret), ret);
    }
    return samples;
}
