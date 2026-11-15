#include "engine/render/backends/deko/dk_device.h"

#include <switch.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cfenv>

#include "dk_shaders_gen.h"
#include "engine/render/backends/deko/dk_binding_plan.h"
#include "engine/gs/hle/ir/draw_ir.h"
#include "engine/gs/hle/texsrc/texture_source.h"
#include "engine/render/blend_ff_map.h"
#include "engine/render/cd_copy_policy.h"
#include "engine/render/draw_policy.h"
#include "engine/render/vertex_convert.h"

namespace ps2r {
namespace render {

namespace {

bool Is16BitFrame(uint8_t psm) { return psm == 0x02 || psm == 0x0A; }
float SnapColumn8(float u) { return std::nearbyint(u / 8.0f) * 8.0f; }
float SnapRow(float v) {
    const float s = std::nearbyint(v / 8.0f) * 8.0f;
    return s + std::nearbyint(v - s);
}

constexpr uint32_t kGlNearest = 0x2600, kGlLinear = 0x2601, kGlNearestMipNearest = 0x2700, kGlLinearMipNearest = 0x2701,
                   kGlLinearMipLinear = 0x2703;

DkBlendFactor BlendFactor(uint32_t gl) {
    switch (gl) {
        case kGlZero: return DkBlendFactor_Zero;
        case kGlOne: return DkBlendFactor_One;
        case kGlSrcAlpha: return DkBlendFactor_SrcAlpha;
        case kGlOneMinusSrcAlpha: return DkBlendFactor_InvSrcAlpha;
        case 0x0304: return DkBlendFactor_DstAlpha;
        case 0x0305: return DkBlendFactor_InvDstAlpha;
        case 0x0300: return DkBlendFactor_SrcColor;
        case 0x0301: return DkBlendFactor_InvSrcColor;
        case 0x0306: return DkBlendFactor_DstColor;
        case 0x0307: return DkBlendFactor_InvDstColor;
        case 0x8001: return DkBlendFactor_ConstColor;
        case 0x8002: return DkBlendFactor_InvConstColor;
        case 0x8003: return DkBlendFactor_ConstAlpha;
        case 0x8004: return DkBlendFactor_InvConstAlpha;
        default: return DkBlendFactor_One;
    }
}
DkBlendOp BlendOp(uint32_t gl) {
    switch (gl) {
        case kGlFuncSubtract: return DkBlendOp_Sub;
        case kGlFuncReverseSubtract: return DkBlendOp_RevSub;
        default: return DkBlendOp_Add;
    }
}
DkStencilOp StencilOp(StencilState::Op o) {
    switch (o) {
        case StencilState::kInvert: return DkStencilOp_Invert;
        case StencilState::kIncrWrap: return DkStencilOp_IncrWrap;
        case StencilState::kDecrWrap: return DkStencilOp_DecrWrap;
        default: return DkStencilOp_Keep;
    }
}

const DkVtxAttribState kAttribs[7][9] = {

    {{0, 0, offsetof(GpuVertex, x), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
     {0, 0, offsetof(GpuVertex, u), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
     {0, 0, offsetof(GpuVertex, r), DkVtxAttribSize_4x8, DkVtxAttribType_Unorm, 0},
     {0, 0, offsetof(GpuVertex, fog), DkVtxAttribSize_1x8, DkVtxAttribType_Unorm, 0},
     {0, 1, 0, DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
     {0, 1, 0, DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0},
     {0, 1, 0, DkVtxAttribSize_1x32, DkVtxAttribType_Float, 0},
     {0, 1, 0, DkVtxAttribSize_1x32, DkVtxAttribType_Float, 0}, {0, 1, 0, DkVtxAttribSize_1x32, DkVtxAttribType_Float, 0}},

    {{0, 0, offsetof(RawVertex, x), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
     {0, 0, offsetof(RawVertex, s), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
     {0, 0, offsetof(RawVertex, r), DkVtxAttribSize_4x8, DkVtxAttribType_Unorm, 0},
     {0, 1, 0, DkVtxAttribSize_1x32, DkVtxAttribType_Float, 0},
     {0, 1, 0, DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
     {0, 1, 0, DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0},
     {0, 0, offsetof(RawVertex, q), DkVtxAttribSize_1x32, DkVtxAttribType_Float, 0},
     {0, 1, 0, DkVtxAttribSize_1x32, DkVtxAttribType_Float, 0}, {0, 1, 0, DkVtxAttribSize_1x32, DkVtxAttribType_Float, 0}},

    {{0, 0, offsetof(MeshVertex, x), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
     {0, 0, offsetof(MeshVertex, s), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
     {0, 0, offsetof(MeshVertex, r), DkVtxAttribSize_4x8, DkVtxAttribType_Unorm, 0},
     {0, 0, offsetof(MeshVertex, fog), DkVtxAttribSize_1x8, DkVtxAttribType_Unorm, 0},
     {0, 0, offsetof(MeshVertex, nx), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
     {0, 0, offsetof(MeshVertex, lr), DkVtxAttribSize_4x8, DkVtxAttribType_Unorm, 0},
     {0, 1, 0, DkVtxAttribSize_1x32, DkVtxAttribType_Float, 0},
     {0, 1, 0, DkVtxAttribSize_1x32, DkVtxAttribType_Float, 0}, {0, 1, 0, DkVtxAttribSize_1x32, DkVtxAttribType_Float, 0}},

    {{0, 0, 0, DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
     {0, 1, 0, DkVtxAttribSize_1x32, DkVtxAttribType_Float, 0},
     {0, 1, 0, DkVtxAttribSize_1x32, DkVtxAttribType_Float, 0},
     {0, 1, 0, DkVtxAttribSize_1x32, DkVtxAttribType_Float, 0},
     {0, 1, 0, DkVtxAttribSize_1x32, DkVtxAttribType_Float, 0},
     {0, 1, 0, DkVtxAttribSize_1x32, DkVtxAttribType_Float, 0},
     {0, 1, 0, DkVtxAttribSize_1x32, DkVtxAttribType_Float, 0},
     {0, 1, 0, DkVtxAttribSize_1x32, DkVtxAttribType_Float, 0}, {0, 1, 0, DkVtxAttribSize_1x32, DkVtxAttribType_Float, 0}},

    {{0, 0, 0, DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
     {0, 0, 8, DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
     {0, 1, 0, DkVtxAttribSize_1x32, DkVtxAttribType_Float, 0},
     {0, 1, 0, DkVtxAttribSize_1x32, DkVtxAttribType_Float, 0},
     {0, 1, 0, DkVtxAttribSize_1x32, DkVtxAttribType_Float, 0},
     {0, 1, 0, DkVtxAttribSize_1x32, DkVtxAttribType_Float, 0},
     {0, 1, 0, DkVtxAttribSize_1x32, DkVtxAttribType_Float, 0},
     {0, 1, 0, DkVtxAttribSize_1x32, DkVtxAttribType_Float, 0}, {0, 1, 0, DkVtxAttribSize_1x32, DkVtxAttribType_Float, 0}},

    {{0, 0, offsetof(MeshVertex, x), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
     {0, 0, offsetof(MeshVertex, s), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
     {0, 0, offsetof(MeshVertex, r), DkVtxAttribSize_4x8, DkVtxAttribType_Unorm, 0},
     {0, 0, offsetof(MeshVertex, fog), DkVtxAttribSize_1x8, DkVtxAttribType_Unorm, 0},
     {0, 0, offsetof(MeshVertex, nx), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
     {0, 0, offsetof(MeshVertex, lr), DkVtxAttribSize_4x8, DkVtxAttribType_Unorm, 0},
     {0, 1, 0, DkVtxAttribSize_1x32, DkVtxAttribType_Float, 0},
     {1, 0, offsetof(SkinBinding, bone), DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0},
     {1, 0, offsetof(SkinBinding, weight), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0}},

    {{0, 0, 0, DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
     {0, 0, 12, DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
     {0, 0, 24, DkVtxAttribSize_4x8, DkVtxAttribType_Unorm, 0},
     {0, 0, 28, DkVtxAttribSize_1x8, DkVtxAttribType_Unorm, 0},
     {0, 1, 0, DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
     {0, 1, 0, DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0},
     {0, 1, 0, DkVtxAttribSize_1x32, DkVtxAttribType_Float, 0},
     {0, 1, 0, DkVtxAttribSize_1x32, DkVtxAttribType_Float, 0},
     {0, 1, 0, DkVtxAttribSize_1x32, DkVtxAttribType_Float, 0}},
};
const uint32_t kStrides[7] = {sizeof(GpuVertex), sizeof(RawVertex), sizeof(MeshVertex), 8, 16, sizeof(MeshVertex), 32};

uint32_t BlendKey(const DkBlendState& b) {
    uint32_t k = 0;
    static_assert(sizeof(DkBlendState) == 4, "DkBlendState packs into one word");
    std::memcpy(&k, &b, 4);
    return k;
}

}

int DekoDevice::Ps2FsVariant(Program& p) {

    static const uint32_t kKeyChunks = [] {
        uint32_t m = 0;
        for (int f = 0; f < GsSpecialize::kFieldCount; ++f) m |= 1u << (dkgen::kPs2KeyOffsets[f] >> 4);
        return m;
    }();
    if (ps2_variant_memo_valid_ && !(p.key_dirty & kKeyChunks)) return ps2_variant_memo_index_;
    p.key_dirty = 0;
    int state[GsSpecialize::kFieldCount];
    for (int f = 0; f < GsSpecialize::kFieldCount; ++f) std::memcpy(&state[f], p.shadow.data() + dkgen::kPs2KeyOffsets[f], 4);
    state[GsSpecialize::kMipOn] = state[GsSpecialize::kMipOn] > 0 ? 1 : 0;
    state[GsSpecialize::kCt16qOn] = state[GsSpecialize::kCt16qOn] != 0 ? 1 : 0;
    state[GsSpecialize::kLodFixed] = state[GsSpecialize::kLodFixed] != 0 ? 1 : 0;
    if (ps2_variant_memo_valid_ && std::memcmp(state, ps2_variant_memo_, sizeof state) == 0) return ps2_variant_memo_index_;
    std::memcpy(ps2_variant_memo_, state, sizeof state);
    ps2_variant_memo_valid_ = true;
    ps2_variant_memo_index_ = -1;
    GsSpecialize::Key key;
    if (!GsSpecialize::MakeKey(state, &key)) return -1;
    for (size_t i = 0; i < ps2_variant_keys_.size(); ++i)
        if (ps2_variant_keys_[i] == key) return ps2_variant_memo_index_ = int(i);
    return -1;
}

void DekoDevice::PrefetchResources(uint32_t mesh, SurfaceId texture, SurfaceId clut) const {
    if (mesh < mesh_index_.size())
        if (const Mesh* m = mesh_index_[mesh]) __builtin_prefetch(m, 0, 3);
    const SurfaceId ids[2] = {texture, clut};
    for (const SurfaceId id : ids) {
        if (!id.id || id.id >= surface_index_.size()) continue;
        if (const DkSurface* s = surface_index_[id.id]) {
            __builtin_prefetch(s, 1, 3);
            __builtin_prefetch(&s->desc_id, 1, 3);
            __builtin_prefetch(&s->write_epoch, 1, 3);
        }
    }
}

void DekoDevice::FlushUniforms(Program& p) {
    if (!p.size) return;
    bool written = false;
    const uint32_t base = uint32_t(cur_) * uniform_slot_bytes_;
    const uint32_t off = uniform_ring_.ResolveLean(p.snapshot, p.shadow.data(), p.size, p.ubo_size, p.dirty,
                                                   uniform_cpu_ + base, uniform_slot_bytes_, &written);
    if (off == DkUniformRing::kInvalid) {

        std::fprintf(stderr, "[deko] uniform reservation violated before draw\n");
        std::abort();
    }
    p.dirty = 0;
    if (written) {
    }
}

void DekoDevice::ApplyTarget() {
    const void* c = want_.target_image ? static_cast<const void*>(want_.target_image)
                                       : (want_.color ? static_cast<const void*>(&want_.color->image) : nullptr);
    const void* d = want_.depth ? static_cast<const void*>(&want_.depth->image) : nullptr;
    if (applied_.have_target && applied_.color == c && applied_.depth == d) return;
    DkImageView cv, dv;
    if (c) dkImageViewDefaults(&cv, static_cast<DkImage const*>(c));
    if (d) dkImageViewDefaults(&dv, static_cast<DkImage const*>(d));
    DkImageView const* cvs[1] = {&cv};
    dkCmdBufBindRenderTargets(cmd_, c ? cvs : nullptr, c ? 1 : 0, d ? &dv : nullptr);
    applied_.color = c;
    applied_.depth = d;
    applied_.have_target = true;
}

void DekoDevice::ApplyState() {
    Before3D();
    ApplyTarget();

    Program& prog = programs_[want_.program];
    int fs_variant = want_.program == ps2_ ? Ps2FsVariant(prog) : -1;

    int vs_variant = -1;
    if (want_.program == ps2_ && !ps2_vs_variants_.empty()) {
        int native, uv, lit;
        std::memcpy(&native, prog.shadow.data() + dkgen::ps2::u_native, 4);
        std::memcpy(&uv, prog.shadow.data() + dkgen::ps2::u_uv_mode, 4);
        std::memcpy(&lit, prog.shadow.data() + dkgen::ps2::u_mesh_lit, 4);
        vs_variant = DkPs2VsVariant(native, uv, lit);
    }

    auto zero = [&](uint32_t offset) { int v; std::memcpy(&v, prog.shadow.data() + offset, 4); return v == 0; };
    if (shadow_count_loaded_ && want_.program == ps2_ && want_.stencil && want_.color_mask == 0 && !want_.depth_write &&
        want_.stencil_write_mask == 0x1f && want_.stencil_pass == DkStencilOp_Keep &&
        (want_.stencil_zfail == DkStencilOp_IncrWrap || want_.stencil_zfail == DkStencilOp_DecrWrap) &&
        zero(dkgen::ps2::u_ate) && zero(dkgen::ps2::u_date) && zero(dkgen::ps2::u_scanmsk) && zero(dkgen::ps2::u_zsrc_en))
        fs_variant = -2;

    const uint32_t read_mask = fs_variant == -2 ? 0 : fs_variant >= 0 ? ps2_sampler_masks_[size_t(fs_variant)] : 0x1f;
    DrawHazards(read_mask);

    const bool program_changed = want_.program != applied_.program;
    const bool vs_changed = vs_variant != applied_.vs_variant;
    const bool fs_changed = fs_variant != applied_.fs_variant;
    if (program_changed || vs_changed || fs_changed) {
        DkShader const* shaders[2];
        uint32_t count = 0;
        uint32_t stages = program_changed ? DkStageFlag_GraphicsMask : 0;
        if (program_changed || vs_changed) {
            stages |= DkStageFlag_Vertex;
            shaders[count++] = vs_variant >= 0 ? &ps2_vs_variants_[size_t(vs_variant)] : &prog.vs;
        }
        if (program_changed || fs_changed) {
            stages |= DkStageFlag_Fragment;
            const bool lean = fs_variant >= 0 &&
                dkgen::kPs2VariantsLean[fs_variant].fs.data != dkgen::kPs2Variants[fs_variant].fs.data;
            const bool fast = fs_variant >= 0 &&
                dkgen::kPs2VariantsAtestFast[fs_variant].fs.data != dkgen::kPs2Variants[fs_variant].fs.data;
            const auto& fs_set = lean ? lean_fs_ : fast ? atest_fast_fs_ : alpha_first_fs_;
            shaders[count++] = fs_variant == -2 ? &programs_[dkgen::k_shadow_count].fs
                              : fs_variant >= 0 ? &fs_set[size_t(fs_variant)] : &prog.fs;
        }
        dkCmdBufBindShaders(cmd_, stages, shaders, count);
        applied_.program = want_.program;
        applied_.vs_variant = vs_variant;
        applied_.fs_variant = fs_variant;
        applied_.sampler_mask = fs_variant == -2 ? 0 : fs_variant >= 0 ? ps2_sampler_masks_[size_t(fs_variant)] : 0x1f;
    }

    const bool keep_fragment = want_.program == ps2_ &&
        !program_changed && applied_.ubo_addr[1] != 0 && applied_.ubo_size[1] == prog.ubo_size &&
        !(prog.dirty & dkgen::ps2::kFragmentChunks);
    const auto bind_uniforms = [&](DkGpuAddr ubo) {
        for (unsigned stage = 0; stage < 2; ++stage) {
            if (stage == 1 && keep_fragment) continue;
            if (applied_.ubo_addr[stage] == ubo && applied_.ubo_size[stage] == prog.ubo_size) continue;
            dkCmdBufBindUniformBuffer(cmd_, stage == 0 ? DkStage_Vertex : DkStage_Fragment, 0, ubo, prog.ubo_size);
            applied_.ubo_addr[stage] = ubo;
            applied_.ubo_size[stage] = prog.ubo_size;
        }
    };

    FlushUniforms(prog);
    bind_uniforms(!prog.size ? 0 : uniform_gpu_ + DkGpuAddr(cur_) * uniform_slot_bytes_ +
                                                       prog.snapshot.offset);
    if (!applied_.viewport_valid || std::memcmp(&applied_.viewport, &want_.viewport, sizeof(DkViewport)) != 0) {
        dkCmdBufSetViewports(cmd_, 0, &want_.viewport, 1);
        applied_.viewport = want_.viewport;
        applied_.viewport_valid = true;
    }
    if (!applied_.scissor_valid || std::memcmp(&applied_.scissor, &want_.scissor, sizeof(DkScissor)) != 0) {
        dkCmdBufSetScissors(cmd_, 0, &want_.scissor, 1);
        applied_.scissor = want_.scissor;
        applied_.scissor_valid = true;
    }
    const uint32_t blend_on = want_.blend ? 1u : 0u;
    if (applied_.color_state != blend_on) {
        DkColorState cs;
        dkColorStateDefaults(&cs);
        cs.blendEnableMask = uint8_t(blend_on);
        dkCmdBufBindColorState(cmd_, &cs);
        applied_.color_state = blend_on;
    }
    if (want_.blend) {
        const uint32_t key = BlendKey(want_.blend_state);
        if (applied_.blend_key != key) {
            dkCmdBufBindBlendState(cmd_, 0, &want_.blend_state);
            applied_.blend_key = key;
        }
        if (applied_.blend_const != want_.blend_const) {
            dkCmdBufSetBlendConst(cmd_, 0.f, 0.f, 0.f, want_.blend_const);
            applied_.blend_const = want_.blend_const;
        }
    }
    if (applied_.color_mask != want_.color_mask) {
        DkColorWriteState cw;
        dkColorWriteStateDefaults(&cw);
        dkColorWriteStateSetMask(&cw, 0, want_.color_mask);
        dkCmdBufBindColorWriteState(cmd_, &cw);
        applied_.color_mask = want_.color_mask;
    }
    {

        const bool have_depth = want_.depth != nullptr;
        const bool have_stencil = have_depth && want_.depth->stencil;
        const bool st = have_stencil && want_.stencil;

        static_assert(DkCompareOp_Always < 256 && DkStencilOp_DecrWrap < 256, "DS input key");
        const uint64_t inputs = uint64_t(have_depth) | uint64_t(want_.depth_test) << 1 |
            uint64_t(want_.depth_write) << 2 | uint64_t(st) << 3 | uint64_t(want_.depth_func) << 8 |
            uint64_t(want_.stencil_pass) << 16 | uint64_t(want_.stencil_zfail) << 24 | uint64_t(want_.stencil_func) << 32;
        if (applied_.ds_inputs != inputs) {
            DkDepthStencilState ds;
            dkDepthStencilStateDefaults(&ds);
            ds.depthTestEnable = have_depth && want_.depth_test;
            ds.depthWriteEnable = have_depth && want_.depth_test && want_.depth_write;
            ds.depthCompareOp = want_.depth_func;
            ds.stencilTestEnable = st;
            ds.stencilFrontFailOp = ds.stencilBackFailOp = DkStencilOp_Keep;
            ds.stencilFrontPassOp = ds.stencilBackPassOp = st ? want_.stencil_pass : DkStencilOp_Keep;
            ds.stencilFrontDepthFailOp = ds.stencilBackDepthFailOp = st ? want_.stencil_zfail : DkStencilOp_Keep;
            ds.stencilFrontCompareOp = ds.stencilBackCompareOp = st ? want_.stencil_func : DkCompareOp_Always;
            uint64_t key = 0;
            static_assert(sizeof(DkDepthStencilState) == 8, "DkDepthStencilState packs into 8 bytes");
            std::memcpy(&key, &ds, 8);
            if (applied_.ds_key != key) {
                dkCmdBufBindDepthStencilState(cmd_, &ds);
                applied_.ds_key = key;
            }
            applied_.ds_inputs = inputs;
        }
        if (st) {
            const uint32_t sk = uint32_t(want_.stencil_write_mask) | uint32_t(want_.stencil_ref) << 8 |
                                uint32_t(want_.stencil_cmp_mask) << 16;
            if (applied_.stencil_key != sk) {
                dkCmdBufSetStencil(cmd_, DkFace_FrontAndBack, want_.stencil_write_mask, want_.stencil_ref, want_.stencil_cmp_mask);
                applied_.stencil_key = sk;
            }
        }
    }
    DkBindChangedTextures(applied_.sampler_mask, applied_.tex, want_.tex, [&](unsigned first, unsigned count) {
        dkCmdBufBindTextures(cmd_, DkStage_Fragment, first, want_.tex + first, count);
    });
    if (descriptors_dirty_) {
        dkCmdBufBarrier(cmd_, DkBarrier_None, DkInvalidateFlags_Descriptors);
        descriptors_dirty_ = false;
    }
}

void DekoDevice::RecordVertexDraw(int layout, DkGpuAddr vtx_addr, uint32_t vtx_size, uint32_t first, uint32_t count) {
    if (applied_.layout != layout) {
        dkCmdBufBindVtxAttribState(cmd_, kAttribs[layout], 9);
        const DkVtxBufferState vb{kStrides[layout], 0};
        dkCmdBufBindVtxBufferState(cmd_, &vb, 1);
        applied_.layout = layout;
    }
    if (applied_.vtx_addr != vtx_addr || applied_.vtx_size != vtx_size) {
        dkCmdBufBindVtxBuffer(cmd_, 0, vtx_addr, vtx_size);
        applied_.vtx_addr = vtx_addr;
        applied_.vtx_size = vtx_size;
    }
    dkCmdBufDraw(cmd_, DkPrimitive_Triangles, count, 1, first, 0);
}

void DekoDevice::DrawArrays(int layout, DkGpuAddr addr, uint32_t size, uint32_t first, uint32_t count) {
    ApplyState();

    if (layout == kLayoutMesh) {
        const uint32_t changes = uint32_t(applied_.layout != layout) |
            (uint32_t(applied_.vtx_addr != addr || applied_.vtx_size != size) << 1);
        const DkDrawPacketKey key{addr, size, first, count, uint32_t(layout), changes};
        auto& e = draw_packets_->Lookup(key);
        if (!e.count || !(e.key == key)) {

            DkCmdBuf live = cmd_;
            cmd_ = packet_cmd_;
            dkCmdBufBeginCaptureCmds(cmd_, e.words.data(), DkDrawPacketCache::kWords);
            RecordVertexDraw(layout, addr, size, first, count);
            e.count = dkCmdBufEndCaptureCmds(cmd_);
            cmd_ = live;
            e.key = key;
        } else {

            applied_.layout = layout;
            applied_.vtx_addr = addr;
            applied_.vtx_size = size;
        }
        dkCmdBufReplayCmds(cmd_, e.words.data(), e.count);
    } else RecordVertexDraw(layout, addr, size, first, count);
    MaybeSubmit(count);
}

void DekoDevice::StencilAround(const StencilState& st) {
    want_.stencil = st.enabled;
    if (!st.enabled) return;
    want_.stencil_func = st.func == StencilState::kEqual ? DkCompareOp_Equal : DkCompareOp_Always;
    want_.stencil_ref = st.ref;
    want_.stencil_cmp_mask = st.cmp_mask;
    want_.stencil_write_mask = st.write_mask;
    want_.stencil_pass = StencilOp(st.pass_op);
    want_.stencil_zfail = StencilOp(st.depth_fail_op);
}

DrawResult DekoDevice::Draw(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& p) {
    SafePoint();
    ops_++;
    run_valid_ = false;
    if (p.program == StencilProgram::StripCount) return DrawStripCount(ir, tex, p);
    if (p.program == StencilProgram::Invert) return DrawInvert(ir, tex, p);
    Step s;
    DrawResult none;
    const size_t n = ir.verts.size();
    if (n < 3) return none;
    if (!BindTarget(p, s)) return none;
    FrameClear(p, s);
    BindTextures(tex, p, s);
    if (!ConvertVertices(ir, tex, s)) return none;
    want_.program = ps2_;
    U1i(dkgen::ps2::u_native, 0);
    U1i(dkgen::ps2::u_uv_mode, 0);
    U2f(dkgen::ps2::u_screen_size, float(s.render_w), float(s.render_h));
    TextureUniforms(ir, tex, p, s);
    RegisterUniforms(ir, p);
    ClassifyRoutes(ir, tex, p, s);
    BlendState(ir, p, s);
    DepthState(ir, p);
    ScissorState(ir, s);
    DkGpuAddr g = 0;
    const uint32_t bytes = uint32_t(s.tri_verts * sizeof(GpuVertex));
    void* dst = StreamAlloc(bytes, sizeof(GpuVertex), &g);
    if (!dst) return none;
    std::memcpy(dst, scratch_.data(), bytes);
    ColorMaskState(ir, s);
    StencilAround(p.stencil_state);
    DrawArrays(kLayoutGpu, g, bytes, 0, uint32_t(s.tri_verts));
    want_.stencil = false;
    return MakeResult(ir, p, s);
}

DrawResult DekoDevice::DrawMesh(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& p, uint32_t mesh,
                                const MeshDrawParams& mp) {
    SafePoint();
    ops_++;
    run_valid_ = false;
    Step s;
    DrawResult none;
    const Mesh* found = FindMesh(mesh);
    if (!found || found->count < 3) return none;
    const Mesh& m = *found;
    if (!BindTarget(p, s)) return none;
    FrameClear(p, s);
    BindTextures(tex, p, s);
    const auto& t2 = tex.transform;
    const bool textured = tex.enabled && t2.uv_tex_w > 0 && t2.uv_tex_h > 0;
    for (int k = 0; k < 4; ++k) s.bb_all[k] = s.bb_tri[k] = mp.box[k];
    s.bb_valid = true;
    s.tri_verts = m.count / 3 * 3;
    want_.program = ps2_;
    U2f(dkgen::ps2::u_screen_size, float(s.render_w), float(s.render_h));
    TextureUniforms(ir, tex, p, s);
    RegisterUniforms(ir, p);
    ClassifyRoutes(ir, tex, p, s);
    Rect write_rect{};
    {

        const auto& sc = ir.raster.scissor;
        int rx0 = std::max(BboxEdge(mp.box[0], false), int(sc.x0));
        int ry0 = std::max(BboxEdge(mp.box[1], false), int(sc.y0));
        int rx1 = std::min(BboxEdge(mp.box[2], true), int(sc.x1) + 1);
        int ry1 = std::min(BboxEdge(mp.box[3], true), int(sc.y1) + 1);
        rx0 = std::max(0, std::min(rx0, p.target_w));
        ry0 = std::max(0, std::min(ry0, p.target_h));
        rx1 = std::max(0, std::min(rx1, p.target_w));
        ry1 = std::max(0, std::min(ry1, p.target_h));

        if (rx1 > rx0 && ry1 > ry0) write_rect = Rect{rx0, ry0, rx1 - rx0, ry1 - ry0};
        if (rx1 > rx0 && ry1 > ry0 && !s.idx_self_serve) {
            s.copy_x = rx0; s.copy_y = ry0; s.copy_w = rx1 - rx0; s.copy_h = ry1 - ry0;
        }
    }
    BlendState(ir, p, s);
    DepthState(ir, p);
    ScissorState(ir, s);
    want_.program = ps2_;
    U1i(dkgen::ps2::u_native, mp.bones ? 3 : 1);
    U1i(dkgen::ps2::u_uv_mode, textured ? 2 : 3);
    if (textured) U2f(dkgen::ps2::u_uv_scale, float(t2.uv_pre_w), float(t2.uv_pre_h));
    MeshUniforms(mp);
    ColorMaskState(ir, s);
    StencilAround(p.stencil_state);
    DrawMeshVertices(m, mp, uint32_t(s.tri_verts));
    want_.stencil = false;
    run_valid_ = !s.needs_rt_copy && !s.sw_blend_active && !s.cd_copied && !p.stencil_state.enabled && !p.do_frame_clear;
    if (run_valid_) {
        run_step_ = s;
        run_ir_ = &ir;
        run_params_ = p;
        run_ops_ = ops_;
    }
    return MakeResult(ir, p, s, &write_rect);
}

void DekoDevice::MeshUniforms(const MeshDrawParams& mp) {
    static_assert(offsetof(MeshDrawParams, screen) == 0 && offsetof(MeshDrawParams, offset) == 64 &&
                  offsetof(MeshDrawParams, scroll) == 72, "mesh uniform prefix");
    SetUFixed<80>(programs_[ps2_], dkgen::ps2::u_mesh, &mp);
    U1i(dkgen::ps2::u_mesh_lit, mp.mode == 2 ? 2 : mp.lit ? 1 : 0);
    if (mp.lit) {
        float light[28] = {};
        for (int l = 0; l < 3; ++l)
            for (int k = 0; k < 3; ++k) {
                light[l * 4 + k] = mp.light_dir[l][k];
                light[(3 + l) * 4 + k] = mp.light_colour[l][k];
            }
        for (int k = 0; k < 3; ++k) light[24 + k] = mp.ambient[k];
        SetUFixed<sizeof light>(programs_[ps2_], dkgen::ps2::u_mesh_light, light);
    }
}

void DekoDevice::DrawMeshIndices(const Mesh& m, uint32_t count) {
    dkCmdBufBindIdxBuffer(cmd_, DkIdxFormat_Uint32, mesh_arena_.blocks[m.idx_block].gpu + m.idx_off);
    dkCmdBufDrawIndexed(cmd_, DkPrimitive_Triangles, count, 1, 0, 0, 0);
}

void DekoDevice::DrawMeshVertices(const Mesh& m, const MeshDrawParams& mp, uint32_t count) {
    const Arena::Block& mb = mesh_arena_.blocks[m.block];
    if (!mp.bones || !m.skin_size) {
        if (m.idx_size) {
            DkRasterizerState rs;
            dkRasterizerStateDefaults(&rs);
            if (mp.cull) {
                rs.cullMode = mp.cull == 1 ? DkFace_Back : DkFace_Front;
                dkCmdBufBindRasterizerState(cmd_, &rs);
            }
            ApplyState();
            if (applied_.layout != kLayoutMesh) {
                dkCmdBufBindVtxAttribState(cmd_, kAttribs[kLayoutMesh], 9);
                const DkVtxBufferState vb{sizeof(MeshVertex), 0};
                dkCmdBufBindVtxBufferState(cmd_, &vb, 1);
                applied_.layout = kLayoutMesh;
            }
            const DkGpuAddr addr = mb.gpu + m.off;
            if (applied_.vtx_addr != addr || applied_.vtx_size != m.size) {
                dkCmdBufBindVtxBuffer(cmd_, 0, addr, m.size);
                applied_.vtx_addr = addr;
                applied_.vtx_size = m.size;
            }
            DrawMeshIndices(m, count);
            MaybeSubmit(count);
            if (mp.cull) {
                rs.cullMode = DkFace_None;
                dkCmdBufBindRasterizerState(cmd_, &rs);
            }
            return;
        }
        if (!mp.cull) {
            DrawArrays(kLayoutMesh, mb.gpu + m.off, m.size, 0, count);
            return;
        }

        DkRasterizerState rs;
        dkRasterizerStateDefaults(&rs);
        rs.cullMode = mp.cull == 1 ? DkFace_Back : DkFace_Front;
        dkCmdBufBindRasterizerState(cmd_, &rs);
        DrawArrays(kLayoutMesh, mb.gpu + m.off, m.size, 0, count);
        rs.cullMode = DkFace_None;
        dkCmdBufBindRasterizerState(cmd_, &rs);
        return;
    }

    const uint32_t n = std::min(mp.bone_vec4, MeshDrawParams::kMaxSkinVec4);
    const uint32_t disp = std::min(mp.disp_vec4, MeshDrawParams::kMaxDispVec4);
    U1i(dkgen::ps2::u_bone_vec4, int(n | (disp ? 1u << 16 : 0u)));
    DkGpuAddr bones_gpu = 0;
    const uint32_t bytes = std::max<uint32_t>(n + disp, 1) * 16;
    auto* dst = static_cast<uint8_t*>(StreamAlloc(bytes, 256, &bones_gpu));
    if (!dst) return;
    std::memcpy(dst, mp.bones, size_t(n + disp) * 16);
    ApplyState();
    dkCmdBufBindUniformBuffer(cmd_, DkStage_Vertex, 1, bones_gpu, bytes);
    const Arena::Block& sb = mesh_arena_.blocks[m.skin_block];
    const DkGpuAddr vtx0 = mb.gpu + m.off, vtx1 = sb.gpu + m.skin_off;
    if (applied_.layout != kLayoutSkin) {
        dkCmdBufBindVtxAttribState(cmd_, kAttribs[kLayoutSkin], 9);
        const DkVtxBufferState vb[2] = {{sizeof(MeshVertex), 0}, {sizeof(SkinBinding), 0}};
        dkCmdBufBindVtxBufferState(cmd_, vb, 2);
        applied_.layout = kLayoutSkin;
    }
    if (applied_.vtx_addr != vtx0 || applied_.vtx_size != m.size) {
        dkCmdBufBindVtxBuffer(cmd_, 0, vtx0, m.size);
    }
    if (applied_.skin_addr != vtx1 || applied_.skin_size != m.skin_size) {
        dkCmdBufBindVtxBuffer(cmd_, 1, vtx1, m.skin_size);
        applied_.skin_addr = vtx1;
        applied_.skin_size = m.skin_size;
    }
    applied_.vtx_addr = vtx0;
    applied_.vtx_size = m.size;
    if (m.idx_size) DrawMeshIndices(m, count);
    else dkCmdBufDraw(cmd_, DkPrimitive_Triangles, count, 1, 0, 0);
}

DrawResult DekoDevice::DrawMeshAgain(uint32_t mesh, const MeshDrawParams& mp) {
    const Mesh* found = FindMesh(mesh);
    if (!run_valid_ || !found || found->count < 3 || ops_ != run_ops_ || rotate_pending_) return DrawResult{};
    SafePoint();
    const Mesh& m = *found;
    Step s = run_step_;
    for (int k = 0; k < 4; ++k) s.bb_all[k] = s.bb_tri[k] = mp.box[k];
    s.tri_verts = m.count / 3 * 3;
    MeshUniforms(mp);
    U1i(dkgen::ps2::u_native, mp.bones ? 3 : 1);
    ColorMaskState(*run_ir_, s);
    DepthState(*run_ir_, run_params_);
    DrawMeshVertices(m, mp, uint32_t(s.tri_verts));
    return MakeResult(*run_ir_, run_params_, s);
}

DrawResult DekoDevice::DrawMeshRetex(const gs::DrawIr& ir, const gs::TextureSource& tex, uint32_t mesh, const MeshDrawParams& mp) {
    const Mesh* found = FindMesh(mesh);
    if (!run_valid_ || !found || found->count < 3 || ops_ != run_ops_ || rotate_pending_ || &ir != run_ir_) return DrawResult{};

    Step s = run_step_;
    BindTextures(tex, run_params_, s);

    if (s.using_rt || run_step_.using_rt) return DrawResult{};
    SafePoint();
    const Mesh& m = *found;
    for (int k = 0; k < 4; ++k) s.bb_all[k] = s.bb_tri[k] = mp.box[k];
    s.bb_valid = true;
    s.tri_verts = m.count / 3 * 3;
    const auto& t2 = tex.transform;
    const bool textured = tex.enabled && t2.uv_tex_w > 0 && t2.uv_tex_h > 0;
    want_.program = ps2_;
    TextureUniforms(ir, tex, run_params_, s);
    U1i(dkgen::ps2::u_native, mp.bones ? 3 : 1);
    U1i(dkgen::ps2::u_uv_mode, textured ? 2 : 3);
    if (textured) U2f(dkgen::ps2::u_uv_scale, float(t2.uv_pre_w), float(t2.uv_pre_h));
    MeshUniforms(mp);
    ColorMaskState(ir, s);
    DepthState(ir, run_params_);
    DrawMeshVertices(m, mp, uint32_t(s.tri_verts));
    run_step_ = s;

    DrawResult r;
    r.drew = true;
    return r;
}

bool DekoDevice::DrawsRaw(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& p) const {
    if (p.program != StencilProgram::None || ir.fog || ir.tex.fst) return false;
    const auto& t = tex.transform;
    return !t.region.applied && t.shuffle_uv_scale_u == 1.0f && !t.shuffle_snap;
}

DrawResult DekoDevice::DrawRaw(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& p, const RawVertex* verts,
                               size_t n, const float* box) {
    SafePoint();
    ops_++;
    run_valid_ = false;
    Step s;
    DrawResult none;
    if (n < 3 || !DrawsRaw(ir, tex, p)) return none;
    if (!BindTarget(p, s)) return none;
    FrameClear(p, s);
    BindTextures(tex, p, s);
    const auto& t2 = tex.transform;
    const bool textured = tex.enabled && t2.uv_tex_w > 0 && t2.uv_tex_h > 0;
    for (int k = 0; k < 4; ++k) s.bb_all[k] = s.bb_tri[k] = box[k];
    s.bb_valid = true;
    s.tri_verts = n / 3 * 3;
    want_.program = ps2_;
    U1i(dkgen::ps2::u_native, 2);
    U1i(dkgen::ps2::u_uv_mode, textured ? 2 : 3);
    if (textured) U2f(dkgen::ps2::u_uv_scale, float(t2.uv_pre_w), float(t2.uv_pre_h));
    U2f(dkgen::ps2::u_screen_size, float(s.render_w), float(s.render_h));
    TextureUniforms(ir, tex, p, s);
    RegisterUniforms(ir, p);
    ClassifyRoutes(ir, tex, p, s);
    BlendState(ir, p, s);
    DepthState(ir, p);
    ScissorState(ir, s);
    want_.program = ps2_;
    const uint32_t bytes = uint32_t(s.tri_verts * sizeof(RawVertex));
    DkGpuAddr g = 0;
    void* dst = StreamAlloc(bytes, 32, &g);
    if (!dst) return none;
    std::memcpy(dst, verts, bytes);
    ColorMaskState(ir, s);
    StencilAround(p.stencil_state);
    DrawArrays(kLayoutRaw, g, bytes, 0, uint32_t(s.tri_verts));
    want_.stencil = false;
    return MakeResult(ir, p, s);
}

DrawResult DekoDevice::DrawStripCount(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& p) {
    Step s;
    DrawResult none;
    if (ir.verts.size() < 3) return none;
    if (p.do_frame_clear) {
        if (!BindTarget(p, s)) return none;
        FrameClear(p, s);
    }
    s.render_w = p.render_w;
    s.render_h = p.render_h;
    s.scale = 1;
    DkSurface* st = Surf(p.stencil);
    if (!st || !st->stencil) return none;
    want_.target_image = nullptr;
    want_.color = nullptr;
    want_.depth = st;
    want_.viewport = DkViewport{0.f, 0.f, float(s.render_w), float(s.render_h), 0.f, 1.f};
    want_.program = strip_;
    Program& sp = programs_[strip_];
    const float screen[2] = {float(s.render_w), float(s.render_h)};
    SetUFixed<8>(sp, dkgen::strip::u_screen_size, screen);
    const int zen = int(ir.raster.test.ztst), znum = std::max<int>(1, p.zsrc_scale), zden = std::max<int>(1, p.scale),
              datm = ir.raster.test.datm ? 1 : 0;
    SetUFixed<4>(sp, dkgen::strip::u_zsrc_en, &zen);
    SetUFixed<4>(sp, dkgen::strip::u_zsrc_num, &znum);
    SetUFixed<4>(sp, dkgen::strip::u_zsrc_den, &zden);
    SetUFixed<4>(sp, dkgen::strip::u_datm, &datm);
    DkSurface* routed = Surf(p.alt16_route && p.alt16.id ? p.alt16 : p.color);
    DkSurface* zsrc = Surf(p.zsrc);
    want_.blend = false;
    want_.tex[1] = Handle(routed && !routed->stencil ? routed : nullptr, 0);
    want_.tex_surface[1] = routed && !routed->stencil ? routed : nullptr;
    want_.tex[4] = Handle(zsrc && !zsrc->stencil ? zsrc : nullptr, 0);
    want_.tex_surface[4] = zsrc && !zsrc->stencil ? zsrc : nullptr;
    want_.depth_test = true;
    want_.depth_func = DkCompareOp_Always;
    want_.depth_write = false;
    ScissorState(ir, s);
    if (!ConvertVertices(ir, tex, s)) return none;
    if (Is16BitFrame(ir.raster.frame.psm) && ir.raster.frame.fbmsk != 0) {
        s.fbmsk16 = DrawPolicy::Fbmsk16From32(ir.raster.frame.fbmsk);
        s.fbmsk16_exact = DrawPolicy::Fbmsk16Exact(s.fbmsk16);
    }
    const uint32_t bytes = uint32_t(s.tri_verts * sizeof(GpuVertex));
    DkGpuAddr g = 0;
    void* dst = StreamAlloc(bytes, sizeof(GpuVertex), &g);
    if (!dst) return none;
    std::memcpy(dst, scratch_.data(), bytes);
    want_.color_mask = 0;
    StencilAround(p.stencil_state);
    DrawArrays(kLayoutGpu, g, bytes, 0, uint32_t(s.tri_verts));
    want_.stencil = false;
    want_.color_mask = 0xF;
    want_.depth_write = true;
    return MakeResult(ir, p, s);
}

DrawResult DekoDevice::DrawInvert(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& p) {
    Step s;
    DrawResult none;
    if (ir.verts.size() < 3) return none;
    if (!BindTarget(p, s)) return none;
    FrameClear(p, s);
    want_.program = invert_;
    Program& ip = programs_[invert_];
    const float screen[2] = {float(s.render_w), float(s.render_h)};
    SetUFixed<8>(ip, dkgen::invert::u_screen_size, screen);
    const float fix = ir.raster.alpha.fix / 128.0f;
    SetUFixed<4>(ip, dkgen::invert::u_fix_alpha, &fix);
    if (!ConvertVertices(ir, tex, s)) return none;
    s.idx_self_serve = true;
    s.copy_x = s.copy_y = s.copy_w = s.copy_h = 0;
    s.needs_rt_copy = true;
    CdCopy(p, s);
    if (!s.sw_blend_active) return none;
    want_.program = invert_;
    want_.blend = false;
    want_.tex[4] = Handle(nullptr, 0);
    want_.tex_surface[4] = nullptr;
    want_.depth_test = true;
    want_.depth_func = DkCompareOp_Always;
    want_.depth_write = false;
    ScissorState(ir, s);
    const uint32_t bytes = uint32_t(s.tri_verts * sizeof(GpuVertex));
    DkGpuAddr g = 0;
    void* dst = StreamAlloc(bytes, sizeof(GpuVertex), &g);
    if (!dst) return none;
    std::memcpy(dst, scratch_.data(), bytes);
    want_.color_mask = 0xF;
    want_.stencil = false;
    DrawArrays(kLayoutGpu, g, bytes, 0, uint32_t(s.tri_verts));
    want_.depth_write = true;
    return MakeResult(ir, p, s);
}

bool DekoDevice::BindTarget(const DrawParams& p, Step& s) {
    s.render_w = p.render_w;
    s.render_h = p.render_h;
    s.scale = DrawScale(p);
    s.scale_x = DrawScaleX(p);
    DkSurface* c = Surf(p.alt16_route && p.alt16.id ? p.alt16 : p.color);
    if (!c || !c->desc.renderable || c->depth) return false;
    DkSurface* d = Surf(p.depth);
    s.target = c;
    want_.target_image = nullptr;
    want_.color = c;
    want_.depth = d && d->depth ? d : nullptr;

    const float vw = p.scale_exact_x > 0.f ? float(PhysicalEdge(s.render_w, s.scale_x)) : float(s.render_w) * s.scale;
    const float vh = p.scale_exact_x > 0.f ? float(PhysicalEdge(s.render_h, s.scale)) : float(s.render_h) * s.scale;
    want_.viewport = DkViewport{0.f, 0.f, vw, vh, 0.f, 1.f};
    return true;
}

void DekoDevice::FrameClear(const DrawParams& p, const Step& s) {
    if (!p.do_frame_clear) return;
    (void)s;
    Before3D();
    ApplyTarget();
    const DkScissor full{0, 0, 0x4000, 0x4000};
    dkCmdBufSetScissors(cmd_, 0, &full, 1);
    applied_.scissor = full;
    applied_.scissor_valid = true;
    if (want_.depth) {
        NoteWrite(want_.depth);
        dkCmdBufClearDepthStencil(cmd_, true, 0.f, 0, 0);
    }
    if (p.clear_color && want_.color) {
        NoteWrite(want_.color);
        dkCmdBufClearColorFloat(cmd_, 0, DkColorMask_RGBA, 0.f, 0.f, 0.f, 0.f);
    }
}

void DekoDevice::BindTextures(const gs::TextureSource& tex, const DrawParams& p, Step& s) {
    using Kind = gs::TextureSource::OwnerKind;
    DkSurface* owner = Surf(tex.owner.surface);
    const bool sampleable = owner && owner->desc_id != 0;
    s.using_rt = tex.enabled && tex.owner.kind == Kind::Surface && sampleable;
    s.has_texture = tex.enabled && sampleable && (tex.owner.kind == Kind::Surface || tex.owner.kind == Kind::Decoded);
    s.hw_lod = false;
    if (s.has_texture) {

        auto repeat = [](uint8_t mode) { return mode == 0 || mode == 3; };
        const bool ws = s.using_rt ? false : repeat(tex.sampler.wms);
        const bool wt = s.using_rt ? false : repeat(tex.sampler.wmt);
        uint32_t mag = kGlNearest, min = kGlNearest;
        const uint8_t levels = s.using_rt ? 0 : owner->levels;
        DrawPolicy::SelectFilters(tex.sampler.mag_linear, tex.sampler.mmin, levels, &mag, &min);
        int aniso = 1;
        if (tex.owner.owned && !s.using_rt) {
            const TextureFilterSelection* sel = texture_filter_.get();
            const TextureFilter mode = sel ? sel->Filter() : TextureFilter::Title;
            const int requested = sel ? sel->Anisotropy() : 1;
            s.hw_lod = DrawPolicy::EnhanceFilters(mode, requested, 16, tex.transform.mip_levels > 0, levels, &mag, &min,
                                                      &aniso);
        }

        if ((s.using_rt && (tex.transform.depth_fmt != 0 || tex.transform.idx_fmt != 0)) ||
            (!s.using_rt && tex.transform.idx_fmt == 8))
            mag = min = kGlNearest;

        want_.tex[0] = Handle(owner, SamplerIndex(mag, min, ws, wt, aniso));
        want_.tex_surface[0] = owner;
    } else {
        want_.tex[0] = Handle(nullptr, 0);
        want_.tex_surface[0] = nullptr;
    }

    const bool idx_active = s.has_texture && s.using_rt && tex.transform.idx_fmt != 0;
    DkSurface* clut = Surf(p.clut);
    DkSurface* partner = Surf(p.idx_partner);
    DkSurface* clut8 = s.has_texture && !s.using_rt && tex.transform.idx_fmt == 8 ? Surf(tex.owner.clut) : nullptr;
    DkSurface* unit2 = clut8 && clut8->desc_id ? clut8 : idx_active && clut && clut->desc_id ? clut : nullptr;
    const bool partner_active = idx_active && tex.transform.idx_fmt == 5 && partner && partner->desc_id;
    want_.tex[2] = Handle(unit2, 0);
    want_.tex_surface[2] = unit2;
    want_.tex[3] = Handle(partner_active ? partner : nullptr, 0);
    want_.tex_surface[3] = partner_active ? partner : nullptr;
}

void DekoDevice::TextureUniforms(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& p, const Step& s) {
    TextureUniformsBody<true>(ir, tex, p, s);
}

template<bool Direct>
void DekoDevice::TextureUniformsBody(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& p, const Step& s) {

    Program& prog = programs_[ps2_];

    uint8_t* const shadow = Direct ? prog.shadow.data() : nullptr;
    uint32_t chunks = 0;
    const auto set = [&](uint32_t off, const void* v, uint32_t n) __attribute__((always_inline)) {
        uint8_t* dst = shadow + off;
        if (std::memcmp(dst, v, n) == 0) return;
        std::memcpy(dst, v, n);
        chunks |= DkUniformChunks(off, n);
    };
    const auto U1i = [&](uint32_t off, int v) __attribute__((always_inline)) {
        if constexpr (Direct) set(off, &v, 4);
        else this->U1i(off, v);
    };
    const auto U2i = [&](uint32_t off, int a, int b) __attribute__((always_inline)) {
        if constexpr (Direct) { const int v[2] = {a, b}; set(off, v, 8); }
        else this->U2i(off, a, b);
    };
    const auto U2f = [&](uint32_t off, float a, float b) __attribute__((always_inline)) {
        if constexpr (Direct) { const float v[2] = {a, b}; set(off, v, 8); }
        else this->U2f(off, a, b);
    };
    namespace L = dkgen::ps2;
    const auto& t = tex.transform;
    const bool rt = s.has_texture && s.using_rt;
    U1i(L::u_use_texture, s.has_texture ? 1 : 0);
    U1i(L::u_depth_fmt, rt ? t.depth_fmt : 0);
    U1i(L::u_depth_zoff, rt ? t.z_row_offset : 0);
    const int idx_fmt = rt ? t.idx_fmt : (s.has_texture && t.idx_fmt == 8 ? 8 : 0);
    U1i(L::u_idx_fmt, idx_fmt);
    if (!(p.inherited_uniforms & kInheritUvNative))
        U1i(L::u_uv_native, (s.has_texture && t.uv_native && idx_fmt == 0) ? 1 : 0);
    if (!(p.inherited_uniforms & kInheritDrawScale)) U1i(L::u_draw_scale, std::max<int>(1, p.scale));
    int idx_self = idx_fmt != 0 ? int(t.idx_self) : 0;
    if (idx_fmt == 6 && ir.tex.fst) idx_self |= 0x1000;
    U1i(L::u_idx_self, idx_self);
    const bool clut_raw = idx_fmt == 4 && t.idx_self != 0 && tex.enabled && ir.tex.tfx == 3 && ir.tex.tcc == 1;
    U1i(L::u_clut_raw, clut_raw ? 1 : 0);
    U1i(L::u_flip_v, 0);
    if (t.region.applied)
        U2f(L::u_tex_size, float(t.region.w), float(t.region.h));
    else
        U2f(L::u_tex_size, float(t.uv_tex_w), float(t.uv_tex_h));
    {
        const float w = t.region.applied ? float(t.region.w) : float(t.uv_tex_w);
        const float h = t.region.applied ? float(t.region.h) : float(t.uv_tex_h);
        U2f(L::u_tex_rsize, w > 0.0f ? 1.0f / w : 0.0f, h > 0.0f ? 1.0f / h : 0.0f);
    }
    U1i(L::u_tfx, tex.enabled ? int(ir.tex.tfx) : 0);
    const int levels = rt ? 0 : t.mip_levels;
    U2i(L::u_mip_ctl, levels, t.mip_lcm ? 1 : 0);
    U1i(L::u_tex_lod_hw, s.hw_lod ? 1 : 0);
    U2f(L::u_mip_lodp, float(t.mip_k) / 16.0f, float(1u << (t.mip_l & 3)));
    U1i(L::u_tcc, tex.enabled ? int(ir.tex.tcc) : 0);
    if constexpr (Direct) { prog.dirty |= chunks; prog.key_dirty |= chunks; }
}

bool DekoDevice::ConvertVertices(const gs::DrawIr& ir, const gs::TextureSource& tex, Step& s) {
    const auto& t = tex.transform;
    const size_t n = ir.verts.size();
    if (scratch_.size() < n) scratch_.resize(n);
    const bool textured = tex.enabled && t.uv_tex_w > 0 && t.uv_tex_h > 0;
    const VertexConvert::UvMode mode = !textured ? VertexConvert::UvMode::Untextured
                                                 : (ir.tex.fst ? VertexConvert::UvMode::UvFixed : VertexConvert::UvMode::Stq);
    const VertexConvert::FusedBboxes bb =
        VertexConvert::Convert(ir.verts.data(), n, scratch_.data(), mode, t.uv_pre_w, t.uv_pre_h);
    s.bb_valid = bb.valid;
    s.bb_tri[0] = bb.tri.x0; s.bb_tri[1] = bb.tri.y0; s.bb_tri[2] = bb.tri.x1; s.bb_tri[3] = bb.tri.y1;
    s.bb_all[0] = bb.all.x0; s.bb_all[1] = bb.all.y0; s.bb_all[2] = bb.all.x1; s.bb_all[3] = bb.all.y1;
    if (t.region.applied) {
        const float rx = float(t.region.u0), ry = float(t.region.v0);
        for (GpuVertex* gv = scratch_.data(); gv != scratch_.data() + n; ++gv) {
            gv->u -= rx * gv->q;
            gv->v -= ry * gv->q;
        }
    }
    if (t.shuffle_uv_scale_u != 1.0f && !ir.tex.fst)
        for (GpuVertex* gv = scratch_.data(); gv != scratch_.data() + n; ++gv) gv->u *= t.shuffle_uv_scale_u;
    if (t.shuffle_snap)
        for (GpuVertex* gv = scratch_.data(); gv != scratch_.data() + n; ++gv) {
            gv->u = SnapColumn8(gv->u);
            gv->v = SnapRow(gv->v);
        }
    s.tri_verts = (n / 3) * 3;
    return s.tri_verts >= 3;
}

void DekoDevice::RegisterUniforms(const gs::DrawIr& ir, const DrawParams& p) {
    namespace L = dkgen::ps2;
    const auto& r = ir.raster;

    const std::array<uint32_t, 10> key{{
        uint32_t(ir.alpha_blend) | uint32_t(r.fba) << 8 | uint32_t(r.colclamp) << 16 | uint32_t(r.frame.psm) << 24,
        uint32_t(ir.scanmsk) | uint32_t(r.test.ate) << 8 | uint32_t(r.test.atst) << 16 | uint32_t(r.test.aref) << 24,
        uint32_t(r.test.afail) | uint32_t(r.test.date) << 8 | uint32_t(r.test.datm) << 16 | uint32_t(ir.fog) << 24,
        uint32_t(r.fogcol_r) | uint32_t(r.fogcol_g) << 8 | uint32_t(r.fogcol_b) << 16 | uint32_t(r.dthe) << 24,
        uint32_t(r.dimx), uint32_t(r.dimx >> 32), uint32_t(p.scale), uint32_t(p.inherited_uniforms),
        uint32_t(want_.program), uint32_t(std::fegetround())}};
    if (register_memo_valid_ && key == register_memo_) {
        return;
    }
    register_memo_ = key;
    register_memo_valid_ = true;
    U1i(L::u_blend_enabled, ir.alpha_blend ? 1 : 0);
    U1i(L::u_fba, r.fba ? 1 : 0);
    U1i(L::u_colclamp, r.colclamp ? 1 : 0);
    U1i(L::u_fb16, r.frame.psm == 0x0A ? 1 : 0);
    if (!(p.inherited_uniforms & kInheritCt16Quant)) U1i(L::u_ct16q, Is16BitFrame(r.frame.psm) ? std::max<int>(1, p.scale) : 0);
    if (!(p.inherited_uniforms & kInheritDither)) {
        U1i(L::u_dthe, r.dthe ? 1 : 0);
        U1i(L::u_dimx_lo, int(uint32_t(r.dimx & 0xFFFFFFFFull)));
        U1i(L::u_dimx_hi, int(uint32_t(r.dimx >> 32)));
    }
    U1i(L::u_scanmsk, ir.scanmsk);
    U1i(L::u_ate, r.test.ate ? 1 : 0);
    U1i(L::u_atst, r.test.atst);
    U1i(L::u_aref, r.test.aref);
    U1i(L::u_afail, r.test.afail);
    U1i(L::u_date, r.test.date ? 1 : 0);
    U1i(L::u_datm, r.test.datm ? 1 : 0);
    U1i(L::u_fog_enabled, ir.fog ? 1 : 0);
    U3f(L::u_fogcol, r.fogcol_r / 255.0f, r.fogcol_g / 255.0f, r.fogcol_b / 255.0f);
}

void DekoDevice::ClassifyRoutes(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& p, Step& s) {
    const auto& r = ir.raster;
    s.alpha_blend = ir.alpha_blend;
    s.fbmsk16 = 0;
    s.fbmsk16_exact = false;
    if (Is16BitFrame(r.frame.psm) && r.frame.fbmsk != 0) {
        s.fbmsk16 = DrawPolicy::Fbmsk16From32(r.frame.fbmsk);
        s.fbmsk16_exact = DrawPolicy::Fbmsk16Exact(s.fbmsk16);
    }
    s.route_rt_blend = DrawPolicy::RouteRtBlend(ir, s.using_rt, s.fbmsk16_exact);
    s.idx_self_serve = s.has_texture && s.using_rt && tex.transform.idx_fmt == 4 && tex.transform.idx_self != 0;
    if (s.idx_self_serve) s.route_rt_blend = false;
    s.copy_x = s.copy_y = s.copy_w = s.copy_h = 0;
    if (!ir.verts.empty()) {
        const float cbx0 = std::min(s.bb_all[0], 1e9f), cby0 = std::min(s.bb_all[1], 1e9f);
        const float cbx1 = std::max(s.bb_all[2], -1e9f), cby1 = std::max(s.bb_all[3], -1e9f);
        int rx0 = std::max(BboxEdge(cbx0, false), int(r.scissor.x0));
        int ry0 = std::max(BboxEdge(cby0, false), int(r.scissor.y0));
        int rx1 = std::min(BboxEdge(cbx1, true), int(r.scissor.x1) + 1);
        int ry1 = std::min(BboxEdge(cby1, true), int(r.scissor.y1) + 1);
        rx0 = std::max(0, std::min(rx0, p.target_w));
        ry0 = std::max(0, std::min(ry0, p.target_h));
        rx1 = std::max(0, std::min(rx1, p.target_w));
        ry1 = std::max(0, std::min(ry1, p.target_h));
        if (rx1 > rx0 && ry1 > ry0) {
            s.copy_x = rx0; s.copy_y = ry0; s.copy_w = rx1 - rx0; s.copy_h = ry1 - ry0;
        }
    }
    if (s.idx_self_serve) { s.copy_w = 0; s.copy_h = 0; }
    s.needs_rt_copy = r.test.date || s.fbmsk16_exact || s.idx_self_serve;
    s.wrap_route = BlendFfMap::RouteWrapBlend(ir.alpha_blend, r.colclamp);
}

void DekoDevice::CdCopy(const DrawParams& p, Step& s) {
    DkSurface* copy = Surf(p.rt_copy);
    if (!p.rt_copy_available || !copy || !copy->desc_id || copy->depth) return;
    bool skip = false;
    int cx = s.copy_x, cy = s.copy_y, cw = s.copy_w, ch = s.copy_h;
    if (p.cd_copy) {
        const int8_t route_now = p.alt16_route ? 1 : 0;
        if (p.cd_copy->last_route != route_now) {
            CdCopyPolicy::Invalidate(*p.cd_copy);
            p.cd_copy->last_route = route_now;
        }
        int oc[4];
        if (!CdCopyPolicy::Decide(*p.cd_copy, s.copy_x, s.copy_y, s.copy_w, s.copy_h, s.idx_self_serve, p.target_w, p.target_h,
                                  oc)) {
            skip = true;
        } else {
            cx = oc[0]; cy = oc[1]; cw = oc[2]; ch = oc[3];
        }
    }
    want_.tex[1] = Handle(copy, 0);
    want_.tex_surface[1] = copy;
    if (!skip && s.target) {

        const float sc = s.scale, sx = s.scale_x;
        if (cw > 0 && ch > 0) {
            const int px = PhysicalEdge(cx, sx), py = PhysicalEdge(cy, sc);
            CopyDraw(s.target, px, py, copy, px, py, PhysicalEdge(cx + cw, sx) - px, PhysicalEdge(cy + ch, sc) - py);
        } else {
            CopyDraw(s.target, 0, 0, copy, 0, 0, PhysicalEdge(p.target_w, sx), PhysicalEdge(p.target_h, sc));
        }
        s.cd_copied = true;
        s.cd_rect = Rect{cx, cy, cw, ch};
    }
    s.sw_blend_active = true;
}

void DekoDevice::BlendAbcdUniforms(const gs::DrawIr& ir, const DrawParams& p) {
    if (p.inherited_uniforms & kInheritBlendAbcd) return;
    namespace L = dkgen::ps2;
    const auto& a = ir.raster.alpha;
    U1i(L::u_blend_a, a.a);
    U1i(L::u_blend_b, a.b);
    U1i(L::u_blend_c, a.c);
    U1i(L::u_blend_d, a.d);
    U1f(L::u_fix_alpha, a.fix / 128.0f);
}

void DekoDevice::BlendState(const gs::DrawIr& ir, const DrawParams& p, Step& s) {
    const auto& a = ir.raster.alpha;
    s.sw_blend_active = false;
    if (ir.alpha_blend && (s.using_rt || ir.raster.fba || s.fbmsk16_exact || s.wrap_route) && !s.route_rt_blend) {
        s.needs_rt_copy = true;
        CdCopy(p, s);
        BlendAbcdUniforms(ir, p);
        want_.blend = false;
    } else if (s.needs_rt_copy) {
        CdCopy(p, s);
        if (!ir.alpha_blend) want_.blend = false;
    } else if (ir.alpha_blend) {
        const FfBlendMap ffm = BlendFfMap::Map(a.a, a.b, a.c, a.d);
        if (ffm.uses_fix) want_.blend_const = a.fix / 128.0f;
        want_.blend = true;

        DkBlendState& b = want_.blend_state;
        dkBlendStateDefaults(&b);
        dkBlendStateSetOps(&b, BlendOp(ffm.eq_rgb), DkBlendOp_Add);
        dkBlendStateSetFactors(&b, BlendFactor(ffm.src_rgb), BlendFactor(ffm.dst_rgb), DkBlendFactor_One, DkBlendFactor_Zero);
        BlendAbcdUniforms(ir, p);
    } else {
        want_.blend = false;
    }
    U1i(dkgen::ps2::u_sw_blend, s.sw_blend_active ? 1 : 0);
}

void DekoDevice::DepthState(const gs::DrawIr& ir, const DrawParams& p) {
    namespace L = dkgen::ps2;
    const auto& t = ir.raster.test;
    DkSurface* zsrc = Surf(p.zsrc);
    const bool armed = p.zsrc_armed && zsrc && zsrc->desc_id;
    want_.tex[4] = Handle(armed ? zsrc : nullptr, 0);
    want_.tex_surface[4] = armed ? zsrc : nullptr;
    U1i(L::u_zsrc_en, armed ? t.ztst : 0);
    if (!(p.inherited_uniforms & kInheritZsrcMap)) {
        U1i(L::u_zsrc_num, armed ? std::max<int>(1, p.zsrc_scale) : 1);
        U1i(L::u_zsrc_den, armed ? std::max<int>(1, p.scale) : 1);
    }
    want_.depth_test = true;
    if (armed) {
        want_.depth_func = DkCompareOp_Always;
        want_.depth_write = false;
    } else if (t.zte) {
        switch (t.ztst) {
            case 0: want_.depth_func = DkCompareOp_Never; break;
            case 1: want_.depth_func = DkCompareOp_Always; break;
            case 2: want_.depth_func = DkCompareOp_Gequal; break;
            case 3: want_.depth_func = DkCompareOp_Greater; break;
            default: want_.depth_func = DkCompareOp_Gequal; break;
        }
        want_.depth_write = !ir.raster.zbuf.zmsk;
    } else {
        want_.depth_func = DkCompareOp_Always;
        want_.depth_write = false;
    }
}

void DekoDevice::ScissorState(const gs::DrawIr& ir, const Step& s) {
    const auto& sc = ir.raster.scissor;
    const int sx0 = std::max(0, int(sc.x0));
    const int sy0 = std::max(0, int(sc.y0));
    const int sx1 = std::min(s.render_w - 1, int(sc.x1));
    const int sy1 = std::min(s.render_h - 1, int(sc.y1));
    if (sx1 >= sx0 && sy1 >= sy0 && (sx0 > 0 || sy0 > 0 || sx1 < s.render_w - 1 || sy1 < s.render_h - 1)) {
        const int px0 = PhysicalEdge(sx0, s.scale_x), py0 = PhysicalEdge(sy0, s.scale);
        want_.scissor = DkScissor{uint32_t(px0), uint32_t(py0), uint32_t(PhysicalEdge(sx1 + 1, s.scale_x) - px0),
                                  uint32_t(PhysicalEdge(sy1 + 1, s.scale) - py0)};
    } else {
        want_.scissor = DkScissor{0, 0, 0x4000, 0x4000};
    }
}

void DekoDevice::ColorMaskState(const gs::DrawIr& ir, const Step& s) {
    const uint32_t mask = ir.raster.frame.fbmsk;
    const uint8_t psm = ir.raster.frame.psm;
    const bool engaged = s.fbmsk16_exact && s.sw_blend_active;
    bool wr, wg, wb, wa;
    if (engaged) {
        wr = wg = wb = wa = true;
    } else if (Is16BitFrame(psm)) {
        wr = (s.fbmsk16 & 0x001F) != 0x001F;
        wg = (s.fbmsk16 & 0x03E0) != 0x03E0;
        wb = (s.fbmsk16 & 0x7C00) != 0x7C00;
        wa = (s.fbmsk16 & 0x8000) != 0x8000;
    } else {
        wr = (mask & 0x000000FF) != 0x000000FF;
        wg = (mask & 0x0000FF00) != 0x0000FF00;
        wb = (mask & 0x00FF0000) != 0x00FF0000;
        wa = (mask & 0xFF000000) != 0xFF000000;
    }
    if (psm == 0x01) wa = false;
    want_.color_mask = (wr ? uint32_t(DkColorMask_R) : 0u) | (wg ? uint32_t(DkColorMask_G) : 0u) | (wb ? uint32_t(DkColorMask_B) : 0u) |
                       (wa ? uint32_t(DkColorMask_A) : 0u);
    U1i(dkgen::ps2::u_fbmsk16_en, engaged ? 1 : 0);
    U1i(dkgen::ps2::u_fbmsk16, int(s.fbmsk16));
}

DrawResult DekoDevice::MakeResult(const gs::DrawIr& ir, const DrawParams& p, const Step& s, const Rect* write_rect) const {
    DrawResult r;
    r.drew = true;
    r.bbox_valid = s.bb_valid;
    for (int i = 0; i < 4; i++) { r.bbox_tri[i] = s.bb_tri[i]; r.bbox_all[i] = s.bb_all[i]; }
    r.alt16_route = p.alt16_route;
    r.sw_blend_active = s.sw_blend_active;
    r.needs_rt_copy = s.needs_rt_copy;
    r.route_rt_blend = s.route_rt_blend;
    r.wrap_route = s.wrap_route;
    r.fbmsk16_exact = s.fbmsk16_exact;
    r.fbmsk16_engaged = s.fbmsk16_exact && s.sw_blend_active;
    r.fbmsk16 = s.fbmsk16;
    r.cd_copy_performed = s.cd_copied;
    r.cd_copy_rect = s.cd_rect;
    if (write_rect) r.write_rect = *write_rect;
    else {
        const auto& sc = ir.raster.scissor;
        int x0 = std::max(BboxEdge(s.bb_all[0], false), int(sc.x0));
        int y0 = std::max(BboxEdge(s.bb_all[1], false), int(sc.y0));
        int x1 = std::min(BboxEdge(s.bb_all[2], true), int(sc.x1) + 1);
        int y1 = std::min(BboxEdge(s.bb_all[3], true), int(sc.y1) + 1);
        x0 = std::max(0, std::min(x0, p.target_w));
        y0 = std::max(0, std::min(y0, p.target_h));
        x1 = std::max(0, std::min(x1, p.target_w));
        y1 = std::max(0, std::min(y1, p.target_h));
        if (x1 > x0 && y1 > y0) r.write_rect = Rect{x0, y0, x1 - x0, y1 - y0};
    }
    return r;
}

}
}
