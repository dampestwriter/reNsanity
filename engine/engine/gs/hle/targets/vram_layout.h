#pragma once

#include <cstdint>
#include <vector>

namespace ps2r {
namespace gs {

struct VramLayout {
    uint32_t scene_accum_fbp = 0;
    uint32_t main_fb_fbp = 0;
    uint32_t fmv_stage_tbp = 0;
    uint32_t stencil_zcopy_fbp = 0;
    uint32_t fog_lane_a_fbp = 0;
    uint32_t fog_lane_b_fbp = 0;
    uint32_t native_scanout_fbp_bytes = 0;
    uint32_t native_scanout_fbw_px = 0;
    uint8_t native_scanout_psm = 0;
    std::vector<uint16_t> reader_band_pages;
    uint32_t clut_heal_exempt_lo = 0;
    uint32_t clut_heal_exempt_hi = 0;
    uint32_t clut_heal_exempt_mask = 0;
    uint32_t clut_heal_exempt_value = 0;
    bool IsClutHealExempt(uint32_t cbp) const;
};

}
}
