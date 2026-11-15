#pragma once

#include <cstdint>

namespace ps2r {
namespace gs {

class TexaExpand {
public:

    struct Texa {
        uint8_t ta0 = 0;
        uint8_t ta1 = 0;
        bool    aem = false;
    };

    static uint8_t Replicate5To8(uint8_t v5);

    static uint32_t Expand16To32(uint16_t rgba16);

    static uint8_t Alpha16(bool a_bit, bool rgb_zero, const Texa& texa);
    static uint8_t Alpha24(bool rgb_zero, const Texa& texa);

    static uint32_t ApplyTexa16(uint16_t rgba16, const Texa& texa);

    static uint32_t ApplyTexa24(uint32_t rgb24, const Texa& texa);
};

}
}
