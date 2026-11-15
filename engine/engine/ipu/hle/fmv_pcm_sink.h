#pragma once

#include <cstdint>

namespace ps2r::ipu {

class FmvPcmSink {
public:
    virtual ~FmvPcmSink() = default;

    virtual void Push(const int16_t* stereo, uint32_t frames) = 0;

    virtual void GateOpen() = 0;

    virtual void Flush() = 0;
};

}
