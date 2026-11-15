#pragma once

#include <cstdint>
#include <vector>

#include "engine/gs/hle/ir/gsdl_format.h"
#include "engine/gs/hle/ir/tex_read_view.h"
#include "engine/gs/hle/ir/vertex_rec.h"

namespace ps2r {
namespace gs {

struct DrawIr {
    struct Identity {
        uint32_t hle_handler = 0;
        uint64_t image_hash = 0;
        uint32_t frame_inst = 0;
        uint16_t kick_pc = 0;
        uint16_t kick_ord = 0;
    };
    struct Frame {
        uint32_t fbp = 0;
        uint32_t fbw = 0;
        uint8_t psm = 0;
        uint32_t fbmsk = 0;
    };
    struct Zbuf {
        uint32_t zbp = 0;
        uint8_t psm = 0;
        bool zmsk = false;
    };
    struct Test {
        bool ate = false;
        uint8_t atst = 0, aref = 0, afail = 0;
        bool date = false, datm = false;
        bool zte = false;
        uint8_t ztst = 0;
    };
    struct Alpha {
        uint8_t a = 0, b = 0, c = 0, d = 0, fix = 0;
    };
    struct Scissor {
        uint16_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    };
    struct Raster {
        Frame frame;
        Zbuf zbuf;
        Test test;
        Alpha alpha;
        Scissor scissor;
        bool colclamp = false;
        bool pabe = false;
        bool fba = false;
        uint8_t fogcol_r = 0, fogcol_g = 0, fogcol_b = 0;
        uint8_t dthe = 0;
        uint64_t dimx = 0;
    };

    struct Snapshot {
        uint64_t vram_hash = 0;
        uint64_t clut_hash = 0;
        uint32_t vram_bytes = 0;
        uint32_t clut_bytes = 0;
        uint64_t vertex_hash = 0;
        uint32_t verts_incl = 0;
    };

    uint32_t seq = 0;
    uint32_t src_frame = 0;
    uint64_t vram_epoch = 0;
    Identity identity;

    uint8_t prim_type = 0;
    bool gouraud = false;
    bool fog = false;
    bool use_fst = false;
    bool alpha_blend = false;
    uint8_t scanmsk = 0;
    uint8_t shuffle_type = 0;
    float shuffle_uv_scale_u = 0.0f;

    bool tex_enabled = false;
    TexReadView tex;
    Raster raster;
    uint8_t ctx = 0;
    float off_x = 0.0f, off_y = 0.0f;

    uint32_t vertex_count = 0;
    std::vector<VertexRec> verts;
    float bbox[6] = {0, 0, 0, 0, 0, 0};
    Snapshot snapshot;

    static DrawIr FromDrawRec(const DrawRec& x, const VertexRec* v, size_t n);
    DrawRec ToDrawRec() const;
};

}
}
