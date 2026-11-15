#pragma once

#include <cstdint>

#include "engine/render/backend.h"

namespace ps2r {
namespace gs {

struct TextureSource {
    enum class OwnerKind : uint8_t { None = 0, Surface = 1, Decoded = 2 };

    struct Owner {
        OwnerKind kind = OwnerKind::None;
        render::SurfaceId surface;

        uint8_t fmt_case = 0;

        bool self_serve = false;
        bool owned = false;

        uint8_t levels = 0;
        uint8_t synth_levels = 0;
        render::SurfaceId clut;
        render::SurfaceId partner;
    };

    struct RegionRebase {
        bool applied = false;
        int32_t u0 = 0, v0 = 0;
        int32_t w = 0, h = 0;
    };

    struct Transform {

        enum Shuffle : uint8_t { kShuffleNone = 0, kShuffle16to32 = 1, kShuffle32to16 = 2,
                                 kShuffleDepthToColor = 3, kShuffleChannel = 4 };
        uint8_t shuffle_type = 0;
        float shuffle_uv_scale_u = 1.0f;
        bool is_shuffle = false;
        bool shuffle_snap = false;
        int32_t uv_tex_w = 0;
        int32_t uv_tex_h = 0;
        int32_t uv_pre_w = 0;
        int32_t uv_pre_h = 0;
        bool uv_native = false;
        int32_t z_row_offset = 0;
        uint8_t depth_fmt = 0;
        uint8_t idx_fmt = 0;
        uint32_t idx_self = 0;
        uint8_t mip_levels = 0;
        bool mip_lcm = false;
        int16_t mip_k = 0;
        uint8_t mip_l = 0;
        uint8_t draw_scale = 1;
        RegionRebase region;
    };

    struct Sampler {
        bool mag_linear = false;
        uint8_t mmin = 0;
        uint8_t wms = 0, wmt = 0;
        uint16_t minu = 0, maxu = 0, minv = 0, maxv = 0;

    };

    enum class Declined : uint8_t {
        None = 0,
        NoTexture,
        StaleTarget,
        ClassMismatch,
        ClutUnservable,
        NoDepthHistory,
        SelfRead,
        NoSnapshot,
        Other
    };

    Owner owner;
    Transform transform;
    Sampler sampler;
    Declined declined = Declined::None;

    bool enabled = false;
};

}
}
