#include "engine/render/draw_policy.h"

#include <algorithm>

#include "engine/gs/hle/ir/draw_ir.h"

namespace ps2r {
namespace render {

namespace {

constexpr uint32_t kGlNearest = 0x2600, kGlLinear = 0x2601, kGlNearestMipNearest = 0x2700,
                   kGlLinearMipNearest = 0x2701, kGlNearestMipLinear = 0x2702,
                   kGlLinearMipLinear = 0x2703;

}

uint32_t DrawPolicy::Fbmsk16From32(uint32_t m) {
    return ((m >> 3) & 0x1F) | ((m >> 6) & 0x3E0) | ((m >> 9) & 0x7C00) | ((m >> 16) & 0x8000);
}

bool DrawPolicy::Fbmsk16Exact(uint32_t fbmsk16) {
    auto aligned = [](uint32_t v, uint32_t field) {
        const uint32_t f = v & field;
        return f == 0 || f == field;
    };
    const bool channel_aligned =
        aligned(fbmsk16, 0x001F) && aligned(fbmsk16, 0x03E0) && aligned(fbmsk16, 0x7C00);
    return !channel_aligned && fbmsk16 != 0;
}

bool DrawPolicy::RouteRtBlend(const gs::DrawIr& ir, bool using_rt, bool fbmsk16_exact) {
    const auto& r = ir.raster;
    if (!(ir.alpha_blend && using_rt && !r.fba && !fbmsk16_exact && !r.test.date && !r.pabe &&
          r.colclamp))
        return false;
    const uint8_t a = r.alpha.a, b = r.alpha.b, c = r.alpha.c, d = r.alpha.d;
    auto is = [&](uint8_t A, uint8_t B, uint8_t C, uint8_t D) {
        return a == A && b == B && c == C && d == D;
    };
    return is(0, 1, 0, 1) || is(0, 2, 0, 1) || is(0, 2, 0, 2) || is(0, 1, 2, 1) ||
           is(0, 2, 2, 1) || is(1, 0, 0, 0) || is(0, 2, 2, 2) || is(2, 1, 0, 1);
}

void DrawPolicy::SelectFilters(bool mag_linear, uint8_t mmin, uint8_t levels, uint32_t* mag,
                                   uint32_t* min) {
    *mag = mag_linear ? kGlLinear : kGlNearest;
    if (levels > 0) {
        switch (mmin) {
            case 0: *min = kGlNearest; break;
            case 1: *min = kGlLinear; break;
            case 2: *min = kGlNearestMipLinear; break;
            case 3: *min = kGlNearestMipLinear; break;
            case 4: *min = kGlLinearMipLinear; break;
            case 5: *min = kGlLinearMipLinear; break;
            default: *min = kGlNearest; break;
        }
    } else {
        switch (mmin) {
            case 1: case 4: case 5: *min = kGlLinear; break;
            default: *min = kGlNearest; break;
        }
    }
}

bool DrawPolicy::EnhanceFilters(TextureFilter filter, int anisotropy, int max_anisotropy,
                                    bool title_mips, uint8_t levels, uint32_t* mag, uint32_t* min,
                                    int* applied_anisotropy) {
    const int aniso = std::min(TextureFilterSelection::ClampAnisotropy(anisotropy), std::max(1, max_anisotropy));
    *applied_anisotropy = aniso;
    const bool chain = levels > 0;
    const bool hw_lod = chain && (aniso > 1 || filter == TextureFilter::Trilinear);
    const bool mips = chain && (title_mips || hw_lod);
    switch (filter) {
        case TextureFilter::Title:
            break;
        case TextureFilter::Point:
            *mag = kGlNearest;
            *min = mips ? kGlNearestMipNearest : kGlNearest;
            break;
        case TextureFilter::Bilinear:
            *mag = kGlLinear;
            *min = mips ? kGlLinearMipNearest : kGlLinear;
            break;
        case TextureFilter::Trilinear:
            *mag = kGlLinear;
            *min = chain ? kGlLinearMipLinear : kGlLinear;
            break;
    }
    return hw_lod;
}

}
}
