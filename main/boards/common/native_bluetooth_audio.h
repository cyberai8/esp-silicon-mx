#ifndef _NATIVE_BLUETOOTH_AUDIO_H
#define _NATIVE_BLUETOOTH_AUDIO_H

#include <cstdint>
class NativeBluetoothAudio {
public:
    struct Metadata {
        const char* title;
        const char* artist;
        const char* album;
    };

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
    using MetadataCallback = void (*)(const Metadata& metadata);

    static NativeBluetoothAudio& GetInstance();

    bool IsSupported() const;
    bool IsInitialized() const;
    bool Initialize();
    bool SetMode(Mode mode);
    void Suspend();
    void Shutdown();
    bool SendCommand(Command command);
    void SetStateCallback(StateCallback callback);
    void SetMetadataCallback(MetadataCallback callback);
    bool IsConnected() const;
    bool IsPlaying() const;
    const char* DeviceName() const;

private:
    NativeBluetoothAudio() = default;
    NativeBluetoothAudio(const NativeBluetoothAudio&) = delete;
    NativeBluetoothAudio& operator=(const NativeBluetoothAudio&) = delete;
};

#endif  // _NATIVE_BLUETOOTH_AUDIO_H
