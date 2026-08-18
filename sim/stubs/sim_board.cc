#include "audio_codec.h"
#include "backlight.h"
#include "board.h"
#include "cx25601n.h"
#include "native_bluetooth_audio.h"

void Backlight::SetBrightness(uint8_t brightness, bool /*permanent*/) {
    brightness_ = brightness;
}

void AudioCodec::SetOutputVolume(int volume) {
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }
    output_volume_ = volume;
}

Board& Board::GetInstance() {
    static Board board;
    return board;
}

bool cx25601n_is_ready(void) {
    // 仿真里打开充电 Tab，方便预览完整设置页。
    return true;
}

esp_err_t cx25601n_set_ichg_ma(uint32_t /*ma*/) {
    return ESP_OK;
}

NativeBluetoothAudio& NativeBluetoothAudio::GetInstance() {
    static NativeBluetoothAudio inst;
    return inst;
}

bool NativeBluetoothAudio::IsSupported() const {
    return true;
}

bool NativeBluetoothAudio::SetMode(Mode /*mode*/) {
    return true;
}
