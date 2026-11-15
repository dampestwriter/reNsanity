#pragma once
#include <cstdlib>
#include <cstring>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#include "engine/platform/ps2e/ps2e_draw.h"
#include "engine/platform/ps2e/scene_interpolator.h"
#include "gs_texture.h"
#include "te_gfx_layout.h"

namespace te::gfx {

struct RenderTargetDescription {
    uint32_t controller;
    int32_t offsetX, offsetY, width, height;
    uint32_t clearColor;
    int32_t displayWidth, displayHeight;
    float clipToScreen[4][4];
    int32_t x, y;
    uint32_t packet, unused6C;
};

struct MaterialView {
    const te_gfx_material* material = nullptr;
    const te_gfx_shader* shader = nullptr;
    const te_gfx_texture* texture = nullptr;
    uint32_t texture_handle = 0;
    uint32_t bucket = TE_GFX_BUCKET_UI;
};

struct DeferredBuild;

struct Draw {
    ps2e_draw_state state;
    std::vector<ps2e_draw_vertex> verts;
    bool copy_frame = false;
    DeferredBuild* pending = nullptr;

    ps2r::ps2e::InterpTag tag;
    bool has_transform = false;

    uint32_t mesh = 0;
    bool mesh_above_far = false;

    ps2r::ps2e::InterpTransform transform;
    ps2e_draw_mesh_params mesh_params{};
    std::array<float, 6> mesh_box{};
    std::vector<float> bones;
    uint32_t skin_disp = 0;
};

bool EnsureDevice();

extern bool g_sceneSkipNow;
inline bool SceneSkipNow() { return g_sceneSkipNow; }
void SceneSkipDecide();
MaterialView ReadMaterial(uint32_t address);

bool TexelsAt(uint32_t address, DecodedTexture& out);

MaterialView ShaderView(const MaterialView& m, uint32_t k);

ps2e_draw_state StateOf(const MaterialView& m);

bool FiniteQ(const ps2e_draw_vertex* vertices, size_t count);
Draw& OpenDraw(uint32_t bucket, const ps2e_draw_state& state, bool copy_frame = false, bool finite_q = false);
void CloseDraw(uint32_t bucket);
void Record(uint32_t bucket, Draw&& d);

struct alignas(64) DeferredBuild {
    std::shared_ptr<void> r3_memo;
    uint64_t r3_picture = 0;
    size_t r3_ordinal = 0;

    std::shared_ptr<void> rigid_memo;
    std::shared_ptr<void> skin_inputs;
    std::shared_ptr<void> skin_memo;
    ps2e_draw_state state{};
    std::vector<ps2e_draw_vertex> verts;
    std::vector<uint32_t> segments;

    ps2e_draw_state alt_state{};
    std::vector<uint8_t> alternate;
    std::atomic<bool> done{false};
    std::atomic<bool> waited{false};
};

class BuildJob {
public:
    static constexpr size_t kInline = 768;
    BuildJob() = default;
    template <typename F, typename Fn = std::decay_t<F>, typename = std::enable_if_t<!std::is_same_v<Fn, BuildJob>>>
    explicit BuildJob(F&& f) {
        if constexpr (sizeof(Fn) <= kInline && alignof(Fn) <= alignof(std::max_align_t) && std::is_nothrow_move_constructible_v<Fn>) {
            new (storage_) Fn(std::forward<F>(f));
            ops_ = &InlineOps<Fn>::ops;
        } else {
            *reinterpret_cast<Fn**>(storage_) = new Fn(std::forward<F>(f));
            ops_ = &HeapOps<Fn>::ops;
        }
    }
    BuildJob(BuildJob&& o) noexcept { Take(o); }
    BuildJob& operator=(BuildJob&& o) noexcept {
        if (this != &o) { Reset(); Take(o); }
        return *this;
    }
    BuildJob(const BuildJob&) = delete;
    BuildJob& operator=(const BuildJob&) = delete;
    ~BuildJob() { Reset(); }
    explicit operator bool() const { return ops_ != nullptr; }
    void operator()() { ops_->call(storage_); }
    void Reset() {
        if (ops_) { ops_->destroy(storage_); ops_ = nullptr; }
    }
private:
    struct Ops {
        void (*call)(void*);
        void (*move)(void* to, void* from);
        void (*destroy)(void*);
    };
    template <typename Fn> struct InlineOps {
        static constexpr Ops ops{[](void* p) { (*static_cast<Fn*>(p))(); },
                                 [](void* to, void* from) { new (to) Fn(std::move(*static_cast<Fn*>(from))); static_cast<Fn*>(from)->~Fn(); },
                                 [](void* p) { static_cast<Fn*>(p)->~Fn(); }};
    };
    template <typename Fn> struct HeapOps {
        static constexpr Ops ops{[](void* p) { (**static_cast<Fn**>(p))(); },
                                 [](void* to, void* from) { *static_cast<Fn**>(to) = *static_cast<Fn**>(from); },
                                 [](void* p) { delete *static_cast<Fn**>(p); }};
    };
    void Take(BuildJob& o) {
        if (o.ops_) { o.ops_->move(storage_, o.storage_); ops_ = o.ops_; o.ops_ = nullptr; }
    }
    alignas(std::max_align_t) unsigned char storage_[kInline];
    const Ops* ops_ = nullptr;
};

bool DeferringBuilds();
DeferredBuild& ReserveDeferred(uint32_t bucket, const ps2e_draw_state& state);

bool SkipBuild(DeferredBuild& build);
void PostBuildJob(DeferredBuild& build, BuildJob&& job);
template <typename F>
void PostBuild(DeferredBuild& build, F&& job) {
    if (SkipBuild(build)) return;
    PostBuildJob(build, BuildJob(std::forward<F>(job)));
}
void DrainBuilds();

void SetDrawTag(const ps2r::ps2e::InterpTag& tag, const float* world16 = nullptr, const float* screen16 = nullptr);
void ClearDrawTag();

bool Interpolating();

bool NativeMeshes();
struct MeshRequest {
    std::vector<ps2e_draw_mesh_vertex> vertices;
    uint32_t handle = 0;
};
void CreateMeshes(std::vector<MeshRequest>& requests);

struct SkinRequest {
    std::vector<ps2e_draw_mesh_vertex> vertices;
    std::vector<ps2e_draw_skin_binding> bones;
    uint32_t handle = 0;
};
void CreateSkins(std::vector<SkinRequest>& requests);
void RecordMesh(uint32_t bucket, const ps2e_draw_state& state, uint32_t mesh, const ps2e_draw_mesh_params& params,
                const std::array<float, 6>& box);

void RecordSkin(uint32_t bucket, const ps2e_draw_state& state, uint32_t skin, const ps2e_draw_mesh_params& params,
                const float* bones, uint32_t vec4s, const float* disp = nullptr, uint32_t disp_vec4s = 0);

ps2e_draw_mesh_params SkinParams(const std::array<std::array<uint32_t, 4>, 4>& to_screen, const std::array<float, 2>& scroll,
                                 const std::array<std::array<uint32_t, 4>, 3>& lights, const std::array<std::array<uint32_t, 4>, 3>& colours,
                                 const std::array<uint32_t, 4>& ambient);

bool GpuSkins();

bool GpuShadows();
void RetireMesh(uint32_t mesh);

void FlushNativeMeshes();

bool CurrentCamera(ps2r::ps2e::InterpCamera& out, uint32_t* camera_object);

void ResetDrawOrdinals();

float GsOriginX();
float GsOriginY();
int ScreenW();
int ScreenH();

const RenderTargetDescription* CurrentTarget();

struct ModelSource {
    uint32_t model;
    int32_t index, count, vertices;
    std::vector<uint8_t> packet;
};
bool LowerPackedPacket(const std::vector<uint8_t>& packet, float offX, float offY, float tw, float th, bool textured,
                       std::vector<ps2e_draw_vertex>& out);

void SetPresentation(const ps2e_draw_presentation& presentation);

void SetFrameRate(int rate);

}
