#pragma once

#include <deko3d.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <unordered_map>
#include <string>
#include <vector>

#include "engine/render/backend.h"
#include "engine/render/backends/deko/dk_submit_policy.h"
#include "engine/render/backends/deko/dk_uniform_ring.h"
#include "engine/render/backends/deko/dk_uniform_write.h"
#include "engine/render/backends/deko/dk_draw_packet.h"
#include "engine/render/gpu_vertex.h"
#include "engine/render/gs_specialize.h"
#include "engine/render/texture_filter_selection.h"

namespace ps2r {
namespace render {

struct DkSurface {
    SurfaceDesc desc;
    uint32_t mem_block = ~0u, mem_off = 0, mem_size = 0;
    DkImage image;
    DkImageFormat format = DkImageFormat_None;
    uint32_t desc_id = 0;
    uint8_t mip_capacity = 1;
    uint8_t levels = 0;
    uint8_t synth_levels = 0;
    uint8_t first_missing = 255;
    bool depth = false, stencil = false;
    uint64_t write_epoch = 0;
    uint64_t read_epoch = 0;

    std::vector<uint8_t> last_level;
    int last_level_index = -1, last_level_w = 0, last_level_h = 0;
};

class DekoDevice : public Backend {
public:
    DekoDevice();
    ~DekoDevice() override;

    bool Init(int window_w, int window_h);
    void Shutdown();
    bool IsInitialized() const { return initialized_; }

    void SetSwapInterval(int interval);

    Caps QueryCaps() const override;
    SurfaceId CreateSurface(const SurfaceDesc& desc) override;
    void DestroySurface(SurfaceId id) override;
    SurfaceDesc DescribeSurface(SurfaceId id) const override;
    void Clear(SurfaceId color, SurfaceId depth, uint32_t rgba, float z, uint32_t flags) override;
    void Upload(SurfaceId id, const Rect& rect, const void* bytes, size_t stride) override;
    void UploadLevel(SurfaceId id, int level, int w, int h, const void* rgba) override;
    void FinishMipChain(SurfaceId id, int authored_levels, bool synthesize_tail) override;
    SurfaceId CreateCompressed(uint32_t w, uint32_t h, uint32_t format, int levels, const void* const* data,
                               const size_t* sizes) override;
    DrawResult Draw(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& params) override;
    uint32_t CreateMesh(const MeshVertex* verts, size_t count) override;
    uint32_t CreateSkinMesh(const MeshVertex* verts, const SkinBinding* bones, size_t count) override;
    void DestroyMesh(uint32_t mesh) override;
    DrawResult DrawMesh(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& params, uint32_t mesh,
                        const MeshDrawParams& mp) override;
    DrawResult DrawMeshAgain(uint32_t mesh, const MeshDrawParams& mp) override;
    DrawResult DrawMeshRetex(const gs::DrawIr& ir, const gs::TextureSource& tex, uint32_t mesh, const MeshDrawParams& mp) override;
    bool DrawsRaw(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& params) const override;
    DrawResult DrawRaw(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& params, const RawVertex* verts,
                       size_t n, const float* box) override;
    void ClearStencil(SurfaceId stencil, uint8_t value) override;
    void DrawConstQuad(SurfaceId color, SurfaceId stencil, const Rect& scissor, uint32_t rgba, bool write_alpha,
                       const StencilState& st) override;
    bool MarkStencilWhereOpaque(SurfaceId color, SurfaceId stencil, uint8_t bits, const Rect& scissor = {}) override;

    bool DepthHaze(SurfaceId color, SurfaceId depth, const uint32_t* palette256, bool direct = false) override;

    bool ConvertYuv420(SurfaceId dst, SurfaceId y, SurfaceId u, SurfaceId v, SurfaceId rb, SurfaceId g) override;
    void CopyRect(SurfaceId src, const Rect& src_rect, SurfaceId dst, const Rect& dst_rect, uint32_t transform) override;
    bool CopyAttachments(SurfaceId, SurfaceId, SurfaceId, SurfaceId) override;
    void Readback(SurfaceId id, const Rect& rect, void* out, size_t stride) override;
    void Present(const ScanoutDesc& desc) override;
    void SetWindowSize(int w, int h) override;
    void PrefetchResources(uint32_t mesh, SurfaceId texture, SurfaceId clut) const override;
    void BeginFrame() override;
    void EndFrame() override;
    void BeginDrawRun() override {}
    void EndDrawRun() override {}

    void SetTextureFilter(std::shared_ptr<const TextureFilterSelection> selection) { texture_filter_ = std::move(selection); }

private:

    static constexpr int kSlots = 3;
    int slots_n_ = 2;
    struct Retired {
        int arena = -1;
        uint32_t block = 0, off = 0, size = 0;
        uint32_t desc_id = 0;
    };

    struct Arena {
        struct Block {
            DkMemBlock mem = nullptr;
            uint8_t* cpu = nullptr;
            DkGpuAddr gpu = 0;
            uint32_t size = 0;
            std::map<uint32_t, uint32_t> free;
        };
        uint32_t flags = 0, block_size = 0;
        std::vector<Block> blocks;
        bool Alloc(DkDevice dev, uint32_t size, uint32_t align, uint32_t* block, uint32_t* off);
        void Free(uint32_t block, uint32_t off, uint32_t size);
        void Destroy();
    };
    struct Slot {
        DkFence fence{};
        bool armed = false;
        std::vector<DkMemBlock> extra_cmd;
        std::vector<Retired> graveyard;
        std::vector<Retired> limbo;
    };
    static void CmdAddMem(void* user, DkCmdBuf cmdbuf, size_t min_size);
    void BeginSlot();
    void EndSlot();
    void Rotate();
    void SafePoint();
    void Submit(bool flush);
    void MaybeSubmit(uint32_t vertices);
    uint32_t CreateMeshStorage(const MeshVertex* verts, size_t count);
    void WaitIdle();
    bool MakeSwapchain(int w, int h);

    void* StreamAlloc(uint32_t size, uint32_t align, DkGpuAddr* gpu);

    bool AllocImage(DkSurface& s, const DkImageLayoutMaker& lm);
    void RetireImage(DkSurface& s) { Retire(Retired{0, s.mem_block, s.mem_off, s.mem_size, s.desc_id}); s.mem_size = 0; s.desc_id = 0; }
    void Retire(const Retired& r) { slots_[cur_].graveyard.push_back(r); }
    void ReleaseRetired(Slot& s);
    uint32_t AllocDescriptor();
    void WriteImageDescriptor(DkSurface& s);

    void Barrier3D();
    void BeforeCopyEngine();
    void AfterCopyEngine() { ce_pending_ = true; }
    void Before3D() {
        if (ce_pending_) {
            dkCmdBufBarrier(cmd_, DkBarrier_Full, DkInvalidateFlags_Image);
            ce_pending_ = false;
            epoch_++;
        }
        if (zcull_stale_) {

            dkCmdBufBarrier(cmd_, DkBarrier_None, DkInvalidateFlags_Zcull);
            zcull_stale_ = false;
        }
        gr_pending_ = true;
    }
    void DrawHazards(uint32_t read_mask);
    void NoteSample(DkSurface* s);
    void NoteWrite(DkSurface* s);

    enum VtxLayout : int { kLayoutNone = -1, kLayoutGpu, kLayoutRaw, kLayoutMesh, kLayoutPos2, kLayoutScan, kLayoutSkin, kLayoutFetch, kLayoutCount };
    struct Program {
        DkShader vs, fs;
        DkGpuAddr ubo = 0;
        uint32_t ubo_size = 0;
        uint32_t size = 0;
        std::vector<uint8_t> shadow;
        DkUniformSnapshot snapshot;
        uint32_t dirty = 0;
        uint32_t key_dirty = ~0u;
    };
    struct Want {
        int program = -1;
        DkSurface* color = nullptr;
        DkImage const* target_image = nullptr;
        DkSurface* depth = nullptr;
        DkViewport viewport{0, 0, 1, 1, 0, 1};
        DkScissor scissor{0, 0, 0x4000, 0x4000};
        bool blend = false;
        DkBlendState blend_state{};
        float blend_const = 0.f;
        uint32_t color_mask = 0xF;
        bool depth_test = true, depth_write = true;
        DkCompareOp depth_func = DkCompareOp_Gequal;
        bool stencil = false;
        DkCompareOp stencil_func = DkCompareOp_Always;
        DkStencilOp stencil_pass = DkStencilOp_Keep, stencil_zfail = DkStencilOp_Keep;
        uint8_t stencil_ref = 0, stencil_cmp_mask = 0xFF, stencil_write_mask = 0xFF;
        DkResHandle tex[5] = {0, 0, 0, 0, 0};
        DkSurface* tex_surface[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    };
    struct Applied {
        int program = -1;
        int vs_variant = -1;
        int fs_variant = -1;
        uint32_t sampler_mask = 0x1f;
        const void* color = nullptr;
        const void* depth = nullptr;
        bool have_target = false;
        DkViewport viewport{};
        bool viewport_valid = false;
        DkScissor scissor{};
        bool scissor_valid = false;
        uint32_t color_state = ~0u;
        uint32_t blend_key = ~0u;
        float blend_const = -1.f;
        uint32_t color_mask = ~0u;
        uint64_t ds_key = ~0ull;
        uint64_t ds_inputs = ~0ull;
        uint32_t stencil_key = ~0u;
        DkResHandle tex[5] = {~0u, ~0u, ~0u, ~0u, ~0u};
        DkGpuAddr ubo_addr[2] = {};
        uint32_t ubo_size[2] = {};
        int layout = kLayoutNone;
        DkGpuAddr vtx_addr = 0;
        uint32_t vtx_size = 0;
        DkGpuAddr skin_addr = 0;
        uint32_t skin_size = 0;
    };
    void ApplyTarget();
    void ApplyState();
    void RecordVertexDraw(int layout, DkGpuAddr addr, uint32_t size, uint32_t first, uint32_t count);
    void FlushUniforms(Program& p);

    int Ps2FsVariant(Program& p);
    template<uint32_t N> void SetUFixed(Program& p, uint32_t off, const void* v) {

        DkSetUniform<N>(p, off, v);
    }
    void U1i(uint32_t off, int v) { SetUFixed<4>(programs_[ps2_], off, &v); }
    void U1f(uint32_t off, float v) { SetUFixed<4>(programs_[ps2_], off, &v); }
    void U2i(uint32_t off, int a, int b) { const int v[2] = {a, b}; SetUFixed<8>(programs_[ps2_], off, v); }
    void U2f(uint32_t off, float a, float b) { const float v[2] = {a, b}; SetUFixed<8>(programs_[ps2_], off, v); }
    void U3f(uint32_t off, float a, float b, float c) { const float v[3] = {a, b, c}; SetUFixed<12>(programs_[ps2_], off, v); }
    void DrawArrays(int layout, DkGpuAddr addr, uint32_t size, uint32_t first, uint32_t count);
    DkResHandle Handle(const DkSurface* s, uint32_t sampler) const;
    static uint32_t SamplerIndex(uint32_t gl_mag, uint32_t gl_min, bool wrap_s_repeat, bool wrap_t_repeat, int aniso);
    void FullTarget(DkSurface* color, DkSurface* depth, int w, int h);
    void DrawFullscreenTri();

    struct Step {
        bool has_texture = false;
        bool using_rt = false;
        bool hw_lod = false;
        DkSurface* target = nullptr;
        int render_w = 0, render_h = 0;
        float scale_x = 1.f;
        float scale = 1.f;
        size_t tri_verts = 0;
        bool alpha_blend = false;
        uint32_t fbmsk16 = 0;
        bool fbmsk16_exact = false;
        bool route_rt_blend = false;
        bool idx_self_serve = false;
        bool wrap_route = false;
        bool needs_rt_copy = false;
        bool sw_blend_active = false;
        int copy_x = 0, copy_y = 0, copy_w = 0, copy_h = 0;
        bool cd_copied = false;
        Rect cd_rect;
        float bb_tri[4] = {0, 0, 0, 0};
        float bb_all[4] = {0, 0, 0, 0};
        bool bb_valid = false;
    };
    DrawResult DrawStripCount(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& p);
    DrawResult DrawInvert(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& p);
    bool BindTarget(const DrawParams& p, Step& s);
    void FrameClear(const DrawParams& p, const Step& s);
    void BindTextures(const gs::TextureSource& tex, const DrawParams& p, Step& s);
    template<bool Direct> void TextureUniformsBody(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& p, const Step& s);
    void TextureUniforms(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& p, const Step& s);
    bool ConvertVertices(const gs::DrawIr& ir, const gs::TextureSource& tex, Step& s);
    void RegisterUniforms(const gs::DrawIr& ir, const DrawParams& p);
    void ClassifyRoutes(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& p, Step& s);
    void CdCopy(const DrawParams& p, Step& s);
    void BlendAbcdUniforms(const gs::DrawIr& ir, const DrawParams& p);
    void BlendState(const gs::DrawIr& ir, const DrawParams& p, Step& s);
    void DepthState(const gs::DrawIr& ir, const DrawParams& p);
    void ScissorState(const gs::DrawIr& ir, const Step& s);
    void ColorMaskState(const gs::DrawIr& ir, const Step& s);
    void StencilAround(const StencilState& st);
    DrawResult MakeResult(const gs::DrawIr& ir, const DrawParams& p, const Step& s, const Rect* write_rect = nullptr) const;
    void MeshUniforms(const MeshDrawParams& mp);

    void CopyDraw(DkSurface* src, int sx, int sy, DkSurface* dst, int dx, int dy, int w, int h);

    DkSurface* Surf(SurfaceId id) const {
        if (!id.id) return nullptr;

        return id.id < surface_index_.size() ? surface_index_[id.id] : nullptr;
    }
    void IndexSurface(uint32_t id, DkSurface* s);

    bool initialized_ = false;
    DkDevice dev_ = nullptr;
    DkQueue queue_ = nullptr;
    DkCmdBuf cmd_ = nullptr;
    DkMemBlock cmd_block_ = nullptr;
    uint32_t cmd_slot_bytes_ = 0;
    DkMemBlock stream_block_ = nullptr;
    uint8_t* stream_cpu_ = nullptr;
    DkGpuAddr stream_gpu_ = 0;
    uint32_t stream_slot_bytes_ = 0;
    uint32_t stream_off_ = 0;

    void DrainForFence();

    uint32_t stream_flushed_ = 0, uniform_flushed_ = 0;
    DkMemBlock uniform_block_ = nullptr;
    uint8_t* uniform_cpu_ = nullptr;
    DkGpuAddr uniform_gpu_ = 0;
    uint32_t uniform_slot_bytes_ = 0;
    DkUniformRing uniform_ring_;
    DkCmdBuf packet_cmd_ = nullptr;
    std::unique_ptr<DkDrawPacketCache> draw_packets_;

    std::vector<uint8_t> ps2_sampler_masks_;
    bool register_memo_valid_ = false;
    std::array<uint32_t, 10> register_memo_{};
    DkMemBlock code_block_ = nullptr;
    DkMemBlock data_block_ = nullptr;
    DkGpuAddr image_desc_gpu_ = 0, sampler_desc_gpu_ = 0;
    Slot slots_[kSlots];
    int cur_ = 0;
    bool rotate_pending_ = false;
    uint32_t draws_since_submit_ = 0;
    uint64_t vertices_since_submit_ = 0, submit_stream_base_ = 0, submit_push_base_ = 0;

    bool zcull_stale_ = false;
    uint32_t present_desc_[kSlots] = {};

    static constexpr uint32_t kMaxImages = 8192;
    static constexpr uint32_t kMaxSamplers = 256;
    std::vector<uint32_t> free_desc_;
    uint32_t next_desc_ = 1;
    bool descriptors_dirty_ = false;
    Arena image_arena_, mesh_arena_;

    uint64_t epoch_ = 1;
    bool ce_pending_ = false, gr_pending_ = false;

    Program programs_[16];
    bool shadow_count_loaded_ = false;
    int ps2_ = 0, strip_ = 0, invert_ = 0, constq_ = 0, opaque_ = 0, scanout_ = 0, scanout_lean_ = 0, scanout_fx_ = 0, copy_ = 0, yuv_ = 0, haze_ = 0;
    SurfaceId haze_depth_{}, haze_pal_{};
    uint32_t haze_w_ = 0, haze_h_ = 0, haze_last_[256] = {};
    bool haze_last_valid_ = false;

    std::vector<DkShader> ps2_fs_variants_;
    std::vector<DkShader> atest_fast_fs_;
    std::vector<DkShader> lean_fs_;
    std::vector<DkShader> alpha_first_fs_;
    std::vector<DkShader> ps2_vs_variants_;

    std::vector<GsSpecialize::Key> ps2_variant_keys_;
    int ps2_variant_memo_[GsSpecialize::kFieldCount] = {};
    bool ps2_variant_memo_valid_ = false;
    int ps2_variant_memo_index_ = -1;
    Want want_;
    Applied applied_;

    std::unordered_map<uint32_t, std::unique_ptr<DkSurface>> surfaces_;
    std::vector<DkSurface*> surface_index_;

    uint32_t next_surface_ = 1;
    std::unique_ptr<DkSurface> dummy_;
    struct Mesh {
        uint32_t block = 0, off = 0, size = 0;
        size_t count = 0;
        uint32_t id = 0, idx_block = 0, idx_off = 0, idx_size = 0;

        uint32_t skin_block = 0, skin_off = 0, skin_size = 0;
    };
    void IndexMesh(Mesh& m, const MeshVertex* verts, const SkinBinding* bones, size_t count);
    void DrawMeshIndices(const Mesh& m, uint32_t count);
    std::unordered_map<uint32_t, Mesh> meshes_;
    std::vector<Mesh*> mesh_index_;
    const Mesh* FindMesh(uint32_t id) const {
        return id < mesh_index_.size() ? mesh_index_[id] : nullptr;
    }
    uint32_t next_mesh_ = 1;

    void DrawMeshVertices(const Mesh& m, const MeshDrawParams& mp, uint32_t count);

    static constexpr int kSwapImages = 3;
    int swap_images_n_ = 2;
    DkMemBlock swap_block_ = nullptr;
    DkImage swap_images_[kSwapImages];
    DkSwapchain swapchain_ = nullptr;
    int swap_w_ = 0, swap_h_ = 0;
    int swap_interval_ = 1;
    int window_w_ = 0, window_h_ = 0;

    DkMemBlock readback_block_ = nullptr;
    uint32_t readback_size_ = 0;

    std::vector<GpuVertex> scratch_;
    bool run_valid_ = false;
    Step run_step_;
    const gs::DrawIr* run_ir_ = nullptr;
    DrawParams run_params_;
    uint64_t run_ops_ = 0;
    uint64_t ops_ = 0;
    std::shared_ptr<const TextureFilterSelection> texture_filter_;

    struct Stats { uint64_t stream_bytes = 0, push_bytes = 0; } stats_;
};

}
}
