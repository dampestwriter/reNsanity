#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "engine/render/surface_format.h"

namespace ps2r {
namespace gs {
struct DrawIr;
struct TextureSource;
}

namespace render {

struct Caps {
    int max_texture_size = 0;
    bool staging_readback = false;
    bool reverse_subtract = false;
    bool depth_sample = false;
    bool texture_barrier = false;
    bool compute = false;
    bool yuv420_convert = false;
    bool depth_haze = false;
};

struct SurfaceId {
    uint32_t id = 0;
};

struct Rect {
    int32_t x = 0, y = 0, w = 0, h = 0;
};

struct SurfaceDesc {
    uint32_t width = 0;
    uint32_t height = 0;
    SurfaceFormat format = SurfaceFormat::Rgba8;
    bool renderable = false;
    uint8_t scale = 1;
};

enum ClearFlags : uint32_t { kClearColor = 1u, kClearDepth = 2u };

struct StencilState {
    enum Func : uint8_t { kAlways = 0, kEqual = 1 };
    enum Op : uint8_t { kKeep = 0, kInvert = 1, kIncrWrap = 2, kDecrWrap = 3 };
    bool enabled = false;
    Func func = kAlways;
    uint8_t ref = 0;
    uint8_t cmp_mask = 0xFF;
    uint8_t write_mask = 0xFF;
    Op pass_op = kKeep;
    Op depth_fail_op = kKeep;
};

enum class StencilProgram : uint8_t { None = 0, StripCount = 1, Invert = 2 };

enum InheritedUniform : uint32_t {
    kInheritCt16Quant = 1u << 0,
    kInheritDither = 1u << 1,
    kInheritDrawScale = 1u << 2,
    kInheritUvNative = 1u << 3,
    kInheritBlendAbcd = 1u << 4,
    kInheritZsrcMap = 1u << 5,
};

struct CdCopyState {
    bool valid = false;
    int32_t dirty_x0 = 0, dirty_y0 = 0, dirty_x1 = 0, dirty_y1 = 0;
    int8_t last_route = -1;
};

struct DrawParams {
    SurfaceId color;
    SurfaceId depth;
    SurfaceId rt_copy;
    SurfaceId alt16;
    SurfaceId clut;
    SurfaceId idx_partner;
    SurfaceId zsrc;
    int32_t render_w = 0;
    int32_t render_h = 0;
    int32_t target_w = 0;
    int32_t target_h = 0;
    uint8_t scale = 1;

    float scale_exact = 0.f;

    float scale_exact_x = 0.f;
    bool has_32bit_content = false;
    bool alt16_route = false;
    bool rt_copy_available = false;
    CdCopyState* cd_copy = nullptr;
    bool zsrc_armed = false;
    uint8_t zsrc_scale = 1;
    bool do_frame_clear = false;
    bool clear_color = false;
    StencilProgram program = StencilProgram::None;
    SurfaceId stencil;

    StencilState stencil_state;
    uint32_t inherited_uniforms = 0;
};

inline float DrawScale(const DrawParams& p) { return p.scale_exact > 0.f ? p.scale_exact : float(p.scale > 0 ? p.scale : 1); }
inline float DrawScaleX(const DrawParams& p) { return p.scale_exact_x > 0.f ? p.scale_exact_x : DrawScale(p); }

inline int BboxEdge(float value, bool upper) {
    if (!(value == value)) return upper ? 1000000000 : -1000000000;
    const double r = upper ? std::ceil(double(value)) : std::floor(double(value));
    return r <= -1e9 ? -1000000000 : r >= 1e9 ? 1000000000 : int(r);
}

inline int32_t PhysicalEdge(int32_t edge, float scale) {
    const double e = double(edge) * double(scale) - 0.5;
    const int32_t f = int32_t(e);
    return (double(f) < e) ? f + 1 : f;
}

struct DrawResult {
    bool drew = false;
    bool bbox_valid = false;
    float bbox_tri[4] = {0, 0, 0, 0};
    float bbox_all[4] = {0, 0, 0, 0};
    bool alt16_route = false;
    bool sw_blend_active = false;
    bool needs_rt_copy = false;
    bool route_rt_blend = false;
    bool wrap_route = false;
    bool fbmsk16_exact = false;
    bool fbmsk16_engaged = false;
    uint32_t fbmsk16 = 0;
    bool cd_copy_performed = false;
    Rect cd_copy_rect;
    Rect write_rect;
};

enum class PresentationFilter : uint8_t { Surface, Nearest, Linear };

struct ScanoutDesc {
    SurfaceId source;
    Rect src;
    Rect dst;
    bool flip_y = true;
    PresentationFilter filter = PresentationFilter::Surface;

    float merge_line = 0.f;
    float merge_x = 0.f;
    bool fxaa = false;
};

struct MeshVertex {
    float x, y, z;
    float s, t, q;
    uint8_t r, g, b, a;
    uint8_t fog, pad[3];
    float nx, ny, nz;
    uint8_t lr, lg, lb, la;
};
static_assert(sizeof(MeshVertex) == 48, "MeshVertex is the 48-byte attribute layout");

struct SkinBinding {
    float bone[4];
    float weight[3];
    float pad;
};
static_assert(sizeof(SkinBinding) == 32, "SkinBinding is the 32-byte second attribute stream");

struct MeshDrawParams {
    float screen[16] = {};
    float offset[2] = {};
    float scroll[2] = {};
    bool lit = false;
    float light_dir[3][3] = {};
    float light_colour[3][3] = {};
    float ambient[3] = {};

    float box[4] = {};

    int cull = 0;

    int mode = 0;

    const float* bones = nullptr;
    uint32_t bone_vec4 = 0;

    uint32_t disp_vec4 = 0;
    static constexpr uint32_t kMaxSkinVec4 = 400, kMaxDispVec4 = 64, kBlockVec4 = kMaxSkinVec4 + kMaxDispVec4;
};

struct RawVertex {
    float x, y, z;
    float s, t;
    uint8_t r, g, b, a;
    float q;
};
static_assert(sizeof(RawVertex) == 28, "RawVertex is the 28-byte producer layout");

class Backend {
public:
    virtual ~Backend() = default;
    virtual Caps QueryCaps() const = 0;
    virtual SurfaceId CreateSurface(const SurfaceDesc& desc) = 0;
    virtual void DestroySurface(SurfaceId id) = 0;
    virtual SurfaceDesc DescribeSurface(SurfaceId id) const = 0;
    virtual void Clear(SurfaceId color, SurfaceId depth, uint32_t rgba, float z,
                       uint32_t flags = kClearColor | kClearDepth) = 0;

    virtual void Upload(SurfaceId id, const Rect& rect, const void* bytes, size_t stride) = 0;

    virtual void UploadLevel(SurfaceId id, int level, int w, int h, const void* rgba) = 0;
    virtual void FinishMipChain(SurfaceId id, int authored_levels, bool synthesize_tail) = 0;

    enum CompressedFormat : uint32_t { kReplacementRgba8 = 1, kCompressedBc1 = 2, kCompressedBc3 = 3, kCompressedBc7 = 4 };
    virtual SurfaceId CreateCompressed(uint32_t, uint32_t, uint32_t, int, const void* const*, const size_t*) { return SurfaceId{}; }

    virtual bool ConvertYuv420(SurfaceId, SurfaceId, SurfaceId, SurfaceId, SurfaceId, SurfaceId) { return false; }
    virtual DrawResult Draw(const gs::DrawIr& ir, const gs::TextureSource& tex,
                            const DrawParams& params) = 0;

    virtual uint32_t CreateMesh(const MeshVertex*, size_t) { return 0; }

    virtual uint32_t CreateSkinMesh(const MeshVertex*, const SkinBinding*, size_t) { return 0; }
    virtual void DestroyMesh(uint32_t) {}
    virtual DrawResult DrawMesh(const gs::DrawIr&, const gs::TextureSource&, const DrawParams&, uint32_t,
                                const MeshDrawParams&) { return DrawResult{}; }

    virtual DrawResult DrawMeshAgain(uint32_t, const MeshDrawParams&) { return DrawResult{}; }

    virtual DrawResult DrawMeshRetex(const gs::DrawIr&, const gs::TextureSource&, uint32_t, const MeshDrawParams&) { return DrawResult{}; }

    virtual bool DrawsRaw(const gs::DrawIr&, const gs::TextureSource&, const DrawParams&) const { return false; }
    virtual DrawResult DrawRaw(const gs::DrawIr&, const gs::TextureSource&, const DrawParams&, const RawVertex*, size_t,
                               const float*) { return DrawResult{}; }

    virtual void ClearStencil(SurfaceId stencil, uint8_t value) = 0;
    virtual void DrawConstQuad(SurfaceId color, SurfaceId stencil, const Rect& scissor,
                               uint32_t rgba, bool write_alpha, const StencilState& st) = 0;

    virtual bool MarkStencilWhereOpaque(SurfaceId, SurfaceId, uint8_t, const Rect& = {}) { return false; }

    virtual bool DepthHaze(SurfaceId, SurfaceId, const uint32_t* , bool  = false) { return false; }

    virtual void CopyRect(SurfaceId src, const Rect& src_rect, SurfaceId dst,
                          const Rect& dst_rect, uint32_t transform) = 0;

    virtual bool CopyAttachments(SurfaceId, SurfaceId, SurfaceId, SurfaceId) { return false; }

    virtual void Readback(SurfaceId id, const Rect& rect, void* out, size_t stride) = 0;

    virtual void Present(const ScanoutDesc& desc) = 0;


    virtual void SetWindowSize(int, int) {}

    virtual void PrefetchResources(uint32_t mesh, SurfaceId texture, SurfaceId clut) const { (void)mesh; (void)texture; (void)clut; }

    virtual void BeginFrame() = 0;
    virtual void EndFrame() = 0;

    virtual void BeginDrawRun() {}
    virtual void EndDrawRun() {}
};

}
}
