#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "engine/ipu/hle/fmv_pcm_sink.h"

namespace ps2r::ipu {

class FmvAudioCapture {
public:
    static constexpr uint32_t kPairBytes = 1024;
    static constexpr uint32_t kFramesPerPair = 256;

    explicit FmvAudioCapture(FmvPcmSink& sink);

    void OnAudioPes(const uint8_t* p, size_t n);

    static constexpr int kAnyChannel = -1;
    void SelectChannel(int channel) { channel_ = channel; }
    bool Disabled() const { return disabled_; }

private:
    void Disable();
    bool ParseHeader();
    void PushCompletePairs();

    FmvPcmSink& sink_;
    bool header_parsed_ = false;
    bool disabled_ = false;
    uint32_t body_left_ = 0;
    int channel_ = kAnyChannel;
    std::vector<uint8_t> stage_;
};

}
