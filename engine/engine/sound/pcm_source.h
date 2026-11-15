#pragma once

#include <array>
#include <cstdint>

namespace ps2r::sound {

class PcmSource {
public:
    virtual ~PcmSource() = default;
    virtual std::array<float, 2> Gains() const = 0;
    virtual int Pop(int16_t* stereo, int frames) = 0;
};
}
