#include "engine/render/gs_specialize.h"

namespace ps2r {
namespace render {

namespace {
const char* const kNames[GsSpecialize::kFieldCount] = {
    "u_use_texture", "u_tfx", "u_tcc", "u_tex_lod_hw", "TE_MIP_ON", "u_ate", "u_atst", "u_afail", "u_fba",
    "u_colclamp", "u_scanmsk", "u_fog_enabled", "u_blend_enabled", "u_sw_blend", "u_blend_a", "u_blend_b",
    "u_blend_c", "u_blend_d", "u_date", "u_datm", "u_fbmsk16_en", "u_depth_fmt", "u_idx_fmt", "u_uv_native",
    "u_fb16", "TE_CT16Q_ON", "u_dthe", "u_zsrc_en", "u_clut_raw", "u_flip_v", "TE_LOD_FIXED",
};

void Put(GsSpecialize::Key* k, int field, int v) {
    const uint64_t nib = uint64_t(v & 15);
    if (field < 16) k->lo |= nib << (4 * field);
    else k->hi |= nib << (4 * (field - 16));
}
}

int GsSpecialize::Value(const Key& k, int field) {
    return int(field < 16 ? (k.lo >> (4 * field)) & 15 : (k.hi >> (4 * (field - 16))) & 15);
}

bool GsSpecialize::MakeKey(const int* s, Key* key) {
    int v[kFieldCount];
    for (int f = 0; f < kFieldCount; ++f) {
        if (s[f] < 0 || s[f] >= kLive) return false;
        v[f] = s[f];
    }
    if (!v[kUseTexture]) {
        for (int f : {kTfx, kTcc, kTexLodHw, kMipOn, kDepthFmt, kIdxFmt, kUvNative, kClutRaw, kFlipV}) v[f] = kLive;
    } else if (!v[kMipOn]) {
        v[kTexLodHw] = kLive;
    }
    if (!v[kAte]) v[kAtst] = v[kAfail] = kLive;
    if (!v[kDate]) v[kDatm] = kLive;
    if (!(v[kBlendEnabled] && v[kSwBlend])) v[kBlendA] = v[kBlendB] = v[kBlendC] = v[kBlendD] = kLive;
    if (!v[kCt16qOn]) v[kDthe] = kLive;

    if (!v[kUseTexture] || !v[kMipOn] || v[kTexLodHw] || v[kDepthFmt] || v[kIdxFmt]) v[kLodFixed] = kLive;
    Key k;
    for (int f = 0; f < kFieldCount; ++f) Put(&k, f, v[f]);
    *key = k;
    return true;
}

}
}
