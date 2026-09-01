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

    static NativeBluetoothAudio& GetInstance() {
        static NativeBluetoothAudio instance;
        return instance;
    }

    bool IsSupported() const { return false; }
    bool IsInitialized() const { return false; }
    bool Initialize() { return false; }
    bool SetMode(Mode /*mode*/) { return false; }
    void Suspend() {}
    void Shutdown() {}
    bool SendCommand(Command /*command*/) { return false; }
    void SetStateCallback(StateCallback /*callback*/) {}
    void SetMetadataCallback(MetadataCallback /*callback*/) {}
    bool IsConnected() const { return false; }
    bool IsPlaying() const { return false; }
    const char* DeviceName() const { return "esp-show"; }

private:
    NativeBluetoothAudio() = default;
    NativeBluetoothAudio(const NativeBluetoothAudio&) = delete;
    NativeBluetoothAudio& operator=(const NativeBluetoothAudio&) = delete;
};

#endif  // _NATIVE_BLUETOOTH_AUDIO_H
