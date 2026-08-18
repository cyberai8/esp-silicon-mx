#pragma once

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
    bool SetMode(Mode mode);

private:
    NativeBluetoothAudio() = default;
};
