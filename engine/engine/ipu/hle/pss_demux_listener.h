#pragma once

#include <cstddef>
#include <cstdint>

namespace ps2r::ipu {

class PssDemuxListener {
public:
    virtual ~PssDemuxListener() = default;

    virtual void OnVideoEs(const uint8_t* data, size_t size, bool contains_sequence_end) = 0;

    virtual void OnAudioPes(const uint8_t* data, size_t size) = 0;

    virtual void OnProgramEnd() = 0;
};

}
