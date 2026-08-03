#ifndef _NATIVE_BLUETOOTH_AUDIO_H
#define _NATIVE_BLUETOOTH_AUDIO_H

#include <cstdint>
class NativeBluetoothAudio {
public:
    enum class Mode : uint8_t {
        kSpeakerSink,
        kAudioSource,
    };

    enum class Command : uint8_t {
        kPlay,
        kPause,
        kPrevious,
        kNext,
        kVolumeDown,
        kVolumeUp,
    };

    using StateCallback = void (*)(bool connected, bool playing);

    static NativeBluetoothAudio& GetInstance();

    bool IsSupported() const;
    bool Initialize();
    bool SetMode(Mode mode);
    void Shutdown();
    bool SendCommand(Command command);
    void SetStateCallback(StateCallback callback);
    bool IsConnected() const;
    bool IsPlaying() const;
    const char* DeviceName() const;

private:
    NativeBluetoothAudio() = default;
    NativeBluetoothAudio(const NativeBluetoothAudio&) = delete;
    NativeBluetoothAudio& operator=(const NativeBluetoothAudio&) = delete;
};

#endif  // _NATIVE_BLUETOOTH_AUDIO_H
