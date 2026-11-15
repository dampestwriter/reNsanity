#pragma once

#include <cstdint>
#include <string>

#include "engine/platform/platform.h"

namespace ps2r::platform::sdl {

class AudioPullRing;

class SdlAudioSink {
public:
    SdlAudioSink() = default;
    ~SdlAudioSink();
    SdlAudioSink(const SdlAudioSink&) = delete;
    SdlAudioSink& operator=(const SdlAudioSink&) = delete;

    bool Open(const AudioSpec& spec, AudioPullRing& ring);

    void Close();
    bool Paused() const { return device_ != 0 && paused_; }

    int DeviceBufferFrames() const;


private:
    static void Callback(void* userdata, uint8_t* stream, int len);

    bool OpenAudren(const AudioSpec& spec, AudioPullRing& ring);
    void CloseAudren();
    struct Audren;
    Audren*        audren_        = nullptr;
    uint32_t       device_        = 0;
    bool           audio_subsystem_ = false;
    bool           paused_ = false;
    AudioPullRing* ring_          = nullptr;
    int            got_freq_      = 0;
    int            got_channels_  = 0;
    int            got_samples_   = 0;
};

}
