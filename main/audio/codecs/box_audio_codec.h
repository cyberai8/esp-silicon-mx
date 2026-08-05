#ifndef _BOX_AUDIO_CODEC_H
#define _BOX_AUDIO_CODEC_H

#include "audio_codec.h"

#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>
#include <sdkconfig.h>
#include <freertos/FreeRTOS.h>
#include <mutex>


class BoxAudioCodec : public AudioCodec {
private:
    const audio_codec_data_if_t* data_if_ = nullptr;
    const audio_codec_ctrl_if_t* out_ctrl_if_ = nullptr;
    const audio_codec_if_t* out_codec_if_ = nullptr;
    const audio_codec_ctrl_if_t* in_ctrl_if_ = nullptr;
    const audio_codec_if_t* in_codec_if_ = nullptr;
    const audio_codec_gpio_if_t* gpio_if_ = nullptr;

    esp_codec_dev_handle_t output_dev_ = nullptr;
    esp_codec_dev_handle_t input_dev_ = nullptr;
    std::mutex data_if_mutex_;

#if CONFIG_BOARD_TYPE_ESP_VOCAT
    void* i2c_master_handle_ = nullptr;
    gpio_num_t mclk_ = GPIO_NUM_NC;
    gpio_num_t bclk_ = GPIO_NUM_NC;
    gpio_num_t ws_ = GPIO_NUM_NC;
    gpio_num_t dout_ = GPIO_NUM_NC;
    gpio_num_t din_ = GPIO_NUM_NC;
    gpio_num_t pa_pin_ = GPIO_NUM_NC;
    uint8_t es8311_addr_ = 0;
    uint8_t es7210_addr_ = 0;

    bool tdm_slot_map_ready_ = false;
    uint8_t tdm_slot0_ = 0;
    uint8_t tdm_slot1_ = 1;
    int tdm_probe_reads_left_ = 12;
    uint32_t tdm_probe_log_decimate_ = 0;
    int input_read_fail_count_ = 0;
    TickType_t last_input_recovery_tick_ = 0;
    bool input_recovery_in_progress_ = false;
#endif

    void CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din);

    bool OpenOutputDeviceLocked();
    bool OpenInputDeviceLocked();
    void CloseOutputDeviceLocked();
    void CloseInputDeviceLocked();
#if CONFIG_BOARD_TYPE_ESP_VOCAT
    bool CreateCodecDevicesLocked();
    void DeleteCodecDevicesLocked();
    void HandleReadFailureLocked(esp_err_t ret, int samples);
    bool RecoverInputStreamLocked();
#endif

    virtual int Read(int16_t* dest, int samples) override;
    virtual int Write(const int16_t* data, int samples) override;

public:
    BoxAudioCodec(void* i2c_master_handle, int input_sample_rate, int output_sample_rate,
        gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
        gpio_num_t pa_pin, uint8_t es8311_addr, uint8_t es7210_addr, bool input_reference);
    virtual ~BoxAudioCodec();

    virtual void SetOutputVolume(int volume) override;
    virtual void EnableInput(bool enable) override;
    virtual void EnableOutput(bool enable) override;
};

#endif // _BOX_AUDIO_CODEC_H
