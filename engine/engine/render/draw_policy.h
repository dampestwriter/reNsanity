#pragma once

#include <cstdint>

#include "engine/render/texture_filter_selection.h"

namespace ps2r {
namespace gs {
struct DrawIr;
}

namespace render {

class DrawPolicy {
public:
    static uint32_t Fbmsk16From32(uint32_t m);
    static bool Fbmsk16Exact(uint32_t fbmsk16);
    static bool RouteRtBlend(const gs::DrawIr& ir, bool using_rt, bool fbmsk16_exact);
    static void SelectFilters(bool mag_linear, uint8_t mmin, uint8_t levels, uint32_t* mag,
                              uint32_t* min);

    static bool EnhanceFilters(TextureFilter filter, int anisotropy, int max_anisotropy, bool title_mips,
                               uint8_t levels, uint32_t* mag, uint32_t* min, int* applied_anisotropy);
};

}
}
