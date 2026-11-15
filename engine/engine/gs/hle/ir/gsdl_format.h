#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/gs/hle/ir/job_rec.h"
#include "engine/gs/hle/ir/vertex_rec.h"

namespace ps2r {
namespace gs {

#pragma pack(push, 1)
struct GsdlFileHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t flags;
    uint32_t reserved;
    uint64_t start_unix_ns;
};

struct GsdlRecordHeader {
    uint32_t kind;
    uint32_t size;
};

struct GsdlNameRec {
    uint32_t handler;
    uint32_t name_len;
};

struct DrawRec {
    uint32_t seq, src_frame;
    uint64_t vram_epoch;

    uint8_t prim_type, gouraud, fog_enabled, use_fst, alpha_blend, scanmsk, shuffle_type, pad0;

    uint8_t tex_enabled, tex_fst, tfx, tcc, cpsm, csm, csa, cld;
    uint32_t tbp, tbw, tpsm, tw, th, cbp;
    uint8_t mmag, mmin, wms, wmt;
    uint16_t minu, maxu, minv, maxv;
    uint8_t texa_ta0, texa_ta1, texa_aem, pad1;
    uint64_t vram_snapshot_hash;
    uint64_t clut_snapshot_hash;
    uint32_t vram_snapshot_bytes, clut_snapshot_bytes;

    uint8_t colclamp, pabe, fba, alpha_a, alpha_b, alpha_c, alpha_d, fix_alpha;
    uint8_t fogcol_r, fogcol_g, fogcol_b, pad2;

    uint32_t frame_fbp, frame_fbw, frame_fbmsk;
    uint8_t frame_psm, zbuf_psm, zbuf_zmsk, pad3;
    uint32_t zbuf_zbp;
    uint16_t scissor_x0, scissor_y0, scissor_x1, scissor_y1;

    uint8_t test_ate, test_atst, test_aref, test_afail, test_date, test_datm, test_zte, test_ztst;

    uint32_t vertex_count;
    uint32_t verts_incl;
    uint64_t vertex_hash;
    float bbox[6];
    float shuffle_uv_scale_u;

    uint32_t hle_handler;
    uint32_t hle_frame_inst;
    uint64_t hle_image;
    uint16_t hle_kick_pc;
    uint16_t hle_kick_ord;
    float off_x, off_y;
    uint8_t gs_context, pad4[3];
};
#pragma pack(pop)

class GsdlFormat {
public:
    static constexpr uint32_t kMagic = 0x524C4432u;
    static constexpr uint32_t kVersion = 2;
    static constexpr uint32_t kFlagVerts = 1u << 0;
    static constexpr uint32_t kFlagPixels = 1u << 1;

    enum Kind : uint32_t { kJob = 1, kDraw = 2, kFrame = 3, kName = 4 };

    static constexpr size_t kDrawRecSizeV1 = offsetof(DrawRec, hle_handler);

    static uint64_t Fnv1aFast(const void* p, size_t n);

    static uint64_t Fnv1aSampled(const void* p, size_t n);

    static uint64_t VertexHash(const VertexRec* v, size_t n);

    static void Bbox(const VertexRec* v, size_t n, float out[6]);
};

static_assert(sizeof(GsdlFileHeader) == 24, "FileHeader is 24 bytes");
static_assert(offsetof(GsdlFileHeader, version) == 4, "");
static_assert(offsetof(GsdlFileHeader, flags) == 8, "");
static_assert(offsetof(GsdlFileHeader, reserved) == 12, "");
static_assert(offsetof(GsdlFileHeader, start_unix_ns) == 16, "");
static_assert(sizeof(GsdlRecordHeader) == 8, "RecordHeader is 8 bytes");
static_assert(offsetof(GsdlRecordHeader, size) == 4, "");
static_assert(sizeof(GsdlNameRec) == 8, "NameRec is 8 bytes");
static_assert(offsetof(JobRec, type) == 0 && offsetof(JobRec, seq) == 4, "");
static_assert(offsetof(JobRec, src_frame) == 8 && offsetof(JobRec, arg1) == 12, "");
static_assert(offsetof(JobRec, arg2) == 16 && offsetof(JobRec, arg3) == 20, "");
static_assert(offsetof(JobRec, arg4) == 24 && offsetof(JobRec, vram_epoch) == 28, "");
static_assert(offsetof(JobRec, payload_bytes) == 36 && offsetof(JobRec, payload_incl) == 40, "");
static_assert(offsetof(JobRec, payload_hash) == 44, "");
static_assert(offsetof(VertexRec, x) == 0 && offsetof(VertexRec, y) == 4, "");
static_assert(offsetof(VertexRec, z) == 8 && offsetof(VertexRec, s) == 12, "");
static_assert(offsetof(VertexRec, t) == 16 && offsetof(VertexRec, q) == 20, "");
static_assert(offsetof(VertexRec, uv_u) == 24 && offsetof(VertexRec, uv_v) == 26, "");
static_assert(offsetof(VertexRec, r) == 28 && offsetof(VertexRec, g) == 29, "");
static_assert(offsetof(VertexRec, b) == 30 && offsetof(VertexRec, a) == 31, "");
static_assert(offsetof(VertexRec, fog) == 32 && offsetof(VertexRec, draw_kick) == 33, "");
static_assert(offsetof(VertexRec, pad) == 34, "");
static_assert(sizeof(DrawRec) == 220, "DrawRec is 220 bytes");
static_assert(GsdlFormat::kDrawRecSizeV1 == 188, "v1 DrawRec is 188 bytes");
static_assert(offsetof(DrawRec, seq) == 0 && offsetof(DrawRec, src_frame) == 4, "");
static_assert(offsetof(DrawRec, vram_epoch) == 8 && offsetof(DrawRec, prim_type) == 16, "");
static_assert(offsetof(DrawRec, gouraud) == 17 && offsetof(DrawRec, fog_enabled) == 18, "");
static_assert(offsetof(DrawRec, use_fst) == 19 && offsetof(DrawRec, alpha_blend) == 20, "");
static_assert(offsetof(DrawRec, scanmsk) == 21 && offsetof(DrawRec, shuffle_type) == 22, "");
static_assert(offsetof(DrawRec, pad0) == 23 && offsetof(DrawRec, tex_enabled) == 24, "");
static_assert(offsetof(DrawRec, tex_fst) == 25 && offsetof(DrawRec, tfx) == 26, "");
static_assert(offsetof(DrawRec, tcc) == 27 && offsetof(DrawRec, cpsm) == 28, "");
static_assert(offsetof(DrawRec, csm) == 29 && offsetof(DrawRec, csa) == 30, "");
static_assert(offsetof(DrawRec, cld) == 31 && offsetof(DrawRec, tbp) == 32, "");
static_assert(offsetof(DrawRec, tbw) == 36 && offsetof(DrawRec, tpsm) == 40, "");
static_assert(offsetof(DrawRec, tw) == 44 && offsetof(DrawRec, th) == 48, "");
static_assert(offsetof(DrawRec, cbp) == 52 && offsetof(DrawRec, mmag) == 56, "");
static_assert(offsetof(DrawRec, mmin) == 57 && offsetof(DrawRec, wms) == 58, "");
static_assert(offsetof(DrawRec, wmt) == 59 && offsetof(DrawRec, minu) == 60, "");
static_assert(offsetof(DrawRec, maxu) == 62 && offsetof(DrawRec, minv) == 64, "");
static_assert(offsetof(DrawRec, maxv) == 66 && offsetof(DrawRec, texa_ta0) == 68, "");
static_assert(offsetof(DrawRec, texa_ta1) == 69 && offsetof(DrawRec, texa_aem) == 70, "");
static_assert(offsetof(DrawRec, pad1) == 71 && offsetof(DrawRec, vram_snapshot_hash) == 72, "");
static_assert(offsetof(DrawRec, clut_snapshot_hash) == 80, "");
static_assert(offsetof(DrawRec, vram_snapshot_bytes) == 88, "");
static_assert(offsetof(DrawRec, clut_snapshot_bytes) == 92, "");
static_assert(offsetof(DrawRec, colclamp) == 96 && offsetof(DrawRec, pabe) == 97, "");
static_assert(offsetof(DrawRec, fba) == 98 && offsetof(DrawRec, alpha_a) == 99, "");
static_assert(offsetof(DrawRec, alpha_b) == 100 && offsetof(DrawRec, alpha_c) == 101, "");
static_assert(offsetof(DrawRec, alpha_d) == 102 && offsetof(DrawRec, fix_alpha) == 103, "");
static_assert(offsetof(DrawRec, fogcol_r) == 104 && offsetof(DrawRec, fogcol_g) == 105, "");
static_assert(offsetof(DrawRec, fogcol_b) == 106 && offsetof(DrawRec, pad2) == 107, "");
static_assert(offsetof(DrawRec, frame_fbp) == 108 && offsetof(DrawRec, frame_fbw) == 112, "");
static_assert(offsetof(DrawRec, frame_fbmsk) == 116 && offsetof(DrawRec, frame_psm) == 120, "");
static_assert(offsetof(DrawRec, zbuf_psm) == 121 && offsetof(DrawRec, zbuf_zmsk) == 122, "");
static_assert(offsetof(DrawRec, pad3) == 123 && offsetof(DrawRec, zbuf_zbp) == 124, "");
static_assert(offsetof(DrawRec, scissor_x0) == 128 && offsetof(DrawRec, scissor_y0) == 130, "");
static_assert(offsetof(DrawRec, scissor_x1) == 132 && offsetof(DrawRec, scissor_y1) == 134, "");
static_assert(offsetof(DrawRec, test_ate) == 136 && offsetof(DrawRec, test_atst) == 137, "");
static_assert(offsetof(DrawRec, test_aref) == 138 && offsetof(DrawRec, test_afail) == 139, "");
static_assert(offsetof(DrawRec, test_date) == 140 && offsetof(DrawRec, test_datm) == 141, "");
static_assert(offsetof(DrawRec, test_zte) == 142 && offsetof(DrawRec, test_ztst) == 143, "");
static_assert(offsetof(DrawRec, vertex_count) == 144 && offsetof(DrawRec, verts_incl) == 148, "");
static_assert(offsetof(DrawRec, vertex_hash) == 152 && offsetof(DrawRec, bbox) == 160, "");
static_assert(offsetof(DrawRec, shuffle_uv_scale_u) == 184, "");
static_assert(offsetof(DrawRec, hle_handler) == 188 && offsetof(DrawRec, hle_frame_inst) == 192, "");
static_assert(offsetof(DrawRec, hle_image) == 196 && offsetof(DrawRec, hle_kick_pc) == 204, "");
static_assert(offsetof(DrawRec, hle_kick_ord) == 206 && offsetof(DrawRec, off_x) == 208, "");
static_assert(offsetof(DrawRec, off_y) == 212 && offsetof(DrawRec, gs_context) == 216, "");
static_assert(offsetof(DrawRec, pad4) == 217, "");

}
}
