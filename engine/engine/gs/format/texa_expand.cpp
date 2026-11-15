#include "engine/gs/format/texa_expand.h"

namespace ps2r {
namespace gs {

uint8_t TexaExpand::Replicate5To8(uint8_t v5) {
    v5 &= 0x1F;
    return uint8_t((v5 << 3) | (v5 >> 2));
}

uint32_t TexaExpand::Expand16To32(uint16_t rgba16) {
    const uint32_t r = Replicate5To8(uint8_t(rgba16 & 0x1F));
    const uint32_t g = Replicate5To8(uint8_t((rgba16 >> 5) & 0x1F));
    const uint32_t b = Replicate5To8(uint8_t((rgba16 >> 10) & 0x1F));
    const uint32_t a = (rgba16 & 0x8000u) ? 0xFFu : 0x00u;
    return r | (g << 8) | (b << 16) | (a << 24);
}

uint8_t TexaExpand::Alpha16(bool a_bit, bool rgb_zero, const Texa& texa) {
    if (a_bit) return texa.ta1;
    if (texa.aem && rgb_zero) return 0;
    return texa.ta0;
}

uint8_t TexaExpand::Alpha24(bool rgb_zero, const Texa& texa) {
    if (texa.aem && rgb_zero) return 0;
    return texa.ta0;
}

uint32_t TexaExpand::ApplyTexa16(uint16_t rgba16, const Texa& texa) {
    const bool rgb_zero = (rgba16 & 0x7FFFu) == 0;
    const uint32_t a = Alpha16((rgba16 & 0x8000u) != 0, rgb_zero, texa);
    return (Expand16To32(rgba16) & 0x00FFFFFFu) | (a << 24);
}

uint32_t TexaExpand::ApplyTexa24(uint32_t rgb24, const Texa& texa) {
    const uint32_t rgb = rgb24 & 0x00FFFFFFu;
    const uint32_t a = Alpha24(rgb == 0, texa);
    return rgb | (a << 24);
}

}
}
