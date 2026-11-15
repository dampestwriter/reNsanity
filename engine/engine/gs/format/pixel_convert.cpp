#include "engine/gs/format/pixel_convert.h"

#include <cassert>

#include "engine/gs/format/psm_layout.h"
#include "engine/gs/format/texa_expand.h"

namespace ps2r {
namespace gs {

uint32_t PixelConvert::UnpackIndex(Psm psm, uint32_t element) {
    const PsmDescriptor& d = PsmLayout::Describe(psm);
    assert(d.is_indexed && "UnpackIndex: not an indexed mode");
    return (element >> d.index_shift) & ((1u << d.index_bits) - 1u);
}

uint8_t PixelConvert::ScaleAlphaPs2ToGpu(uint8_t ps2_alpha) {
    return uint8_t(ps2_alpha >= 0x80 ? 0xFF : ps2_alpha * 2);
}

}
}
