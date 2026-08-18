#pragma once

class AudioCodec {
public:
    void SetOutputVolume(int volume);
    int output_volume() const { return output_volume_; }

private:
    int output_volume_ = 70;
};
