#pragma once

#include <cstdint>

#include "engine/gs/format/psm.h"

namespace ps2r {
namespace gs {

class PixelConvert {
public:

    struct Depth { uint32_t value; };

    static uint32_t UnpackIndex(Psm psm, uint32_t element);

    static uint8_t ScaleAlphaPs2ToGpu(uint8_t ps2_alpha);

    static uint8_t  R(uint32_t rgba32) { return uint8_t(rgba32 & 0xFF); }
    static uint8_t  G(uint32_t rgba32) { return uint8_t((rgba32 >> 8) & 0xFF); }
    static uint8_t  B(uint32_t rgba32) { return uint8_t((rgba32 >> 16) & 0xFF); }
    static uint8_t  A(uint32_t rgba32) { return uint8_t((rgba32 >> 24) & 0xFF); }
    static uint32_t Rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        return uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (uint32_t(a) << 24);
    }
};

}
}
