#include "engine/cdvd/iso_fs.h"
#include "record_lane.h"

#include <algorithm>
#include <limits>
#include "exact_fp.h"
#include <array>
#include <atomic>
#include <type_traits>
#include <malloc.h>
#include <cfenv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/platform/ps2e/ps2e_draw.h"
#include "engine/platform/ps2e/present_pacer.h"
#include "engine/platform/host/platform.h"
#include "graphics_internal.h"
#include "gs_texture.h"
#include "stream_service.h"
#include "texpack.h"
#include "ps2e_seam.h"
#include "te_gfx_layout.h"
#include "te_display.h"
#include "te_graphics_hooks.h"
#include "te_host.h"
#include "te_services.h"


namespace {

struct FrameStart {
    int32_t x, y, width, height, screenWidth, screenHeight;
    uint8_t red, green, blue;
    uint8_t clearColor, clearDepth;
};
struct Rectangle {
    float x, y, width, height;
};
struct Vector2 {
    float x, y;
};
using te::gfx::RenderTargetDescription;
using te::gfx::MaterialView;
using te::gfx::Draw;
using te::gfx::DeferredBuild;
using te::gfx::BuildJob;

constexpr uint32_t kRenderTargetPointer = 0x00309BDC;
constexpr uint32_t kScreenWidth = 0x00309AE4, kScreenHeight = 0x00309AE8;

constexpr uint32_t kDisplayWidth = 0x0030AAE4, kDisplayHeight = 0x0030AAE8;

constexpr float kGsMiddle = 2048.0f;
constexpr float kGsDepthFront = 16777215.0f;

template <typename T>
const T* Guest(uint32_t address) {
    return reinterpret_cast<const T*>(te::gfx::RecordGuestPointer(address, sizeof(T)));
}
template <typename T>
T* GuestMutable(uint32_t address) {
    return reinterpret_cast<T*>(te::gfx::RecordGuestPointer(address, sizeof(T)));
}

void HashState(const ps2e_draw_state& st, uint8_t* out) {
    std::memset(out, 0, sizeof st);
#define HS_FIELD(field) std::memcpy(out + offsetof(ps2e_draw_state, field), &st.field, sizeof st.field)
    const uint32_t texture = st.texture == PS2E_DRAW_TEXTURE_FRAME_COPY ? st.texture : st.texture ? 1u : 0u;
    std::memcpy(out + offsetof(ps2e_draw_state, texture), &texture, sizeof texture);
    HS_FIELD(gouraud); HS_FIELD(depth_test); HS_FIELD(depth_write); HS_FIELD(blend);
    HS_FIELD(blend_a); HS_FIELD(blend_b); HS_FIELD(blend_c); HS_FIELD(blend_d); HS_FIELD(blend_fix);
    HS_FIELD(alpha_test); HS_FIELD(alpha_method); HS_FIELD(alpha_reference); HS_FIELD(alpha_fail);
    HS_FIELD(linear); HS_FIELD(wrap_u); HS_FIELD(wrap_v); HS_FIELD(tfx); HS_FIELD(tcc);
    HS_FIELD(scissor_x0); HS_FIELD(scissor_y0); HS_FIELD(scissor_x1); HS_FIELD(scissor_y1);
    HS_FIELD(no_colour_write); HS_FIELD(stencil); HS_FIELD(fba); HS_FIELD(dest_alpha_test);
    HS_FIELD(mip_max_level); HS_FIELD(mip_k); HS_FIELD(mip_l); HS_FIELD(mip_minify);
#undef HS_FIELD
}
void HashParams(const ps2e_draw_mesh_params& p, uint8_t* out) {
    std::memset(out, 0, sizeof p);
#define HP_FIELD(field) std::memcpy(out + offsetof(ps2e_draw_mesh_params, field), &p.field, sizeof p.field)
    HP_FIELD(screen); HP_FIELD(offset); HP_FIELD(scroll); HP_FIELD(lit);
    HP_FIELD(light_dir); HP_FIELD(light_colour); HP_FIELD(ambient); HP_FIELD(box);
#undef HP_FIELD
}
struct MenuBackdrop;
struct SkipSnap {
    bool skipped = false;
    uint64_t key = 0, seq = 0;
};
struct Gfx {
    ps2e_draw* dev = nullptr;
    bool shut_down = false;
    ps2e_draw_presentation presentation{};
    bool create_failed = false;
    bool pal = false;
    int display_w = 512, display_h = 448;
    FrameStart frame{};
    bool have_frame = false;

    struct Bucket {
        std::vector<Draw> draws;
        size_t used = 0;
        bool has_pending = false;
    };
    Bucket buckets[TE_GFX_BUCKET_COUNT];

    std::vector<std::unique_ptr<DeferredBuild>> deferred;
    size_t deferred_used = 0;
    size_t deferred_recent = 0;
    std::unordered_map<uint32_t, uint32_t> textures;

    struct TextureSource { te_gfx_texture_header header; std::vector<uint8_t> packet; };
    std::unordered_map<uint32_t, TextureSource> texture_sources;
    std::unordered_map<uint32_t, uint64_t> replacement_bytes;

    struct PackLoad { std::mutex m; bool released = false; uint64_t bytes = 0; };
    std::unordered_map<uint32_t, std::shared_ptr<PackLoad>> pack_loads;

    std::atomic<uint64_t> dropped_draws{0};
    std::atomic<uint64_t>   pack_backlog{0};

    struct Frame {
        Bucket buckets[TE_GFX_BUCKET_COUNT];
        std::vector<std::unique_ptr<DeferredBuild>> deferred;
        size_t deferred_used = 0;
        bool send = false;
        bool menu_backdrop_candidate = false;
        SkipSnap skip;
        int screen_w = 0, screen_h = 0;
        uint64_t boundary_key = 0;
        uint64_t picture = 0;
        bool have_edge = false;
        int64_t edge_ns = 0;
        uint64_t field = 0;
        ps2r::ps2e::InterpCamera camera;
        uint32_t camera_object = 0;
        std::vector<ps2r::ps2e::InterpItem> items;
        std::vector<uint32_t> retire;
        std::vector<uint32_t> retire_tex;
        bool thin = false;
        bool shown30 = true;
    };
    std::shared_ptr<MenuBackdrop> backdrop;
    struct RenderThread {
        static constexpr int kSlots = 5;

        std::thread thread;
        std::mutex mutex;
        std::condition_variable wake, done;
        bool enabled = false, started = false, stop = false;
        bool drawing = false;
        Frame slots[kSlots];
        std::vector<int> queue;
        std::vector<int> free;
        int cur = -1, prev = -1;
        std::vector<std::function<void()>> jobs;
        uint64_t jobs_posted = 0, jobs_done = 0;

        ps2r::ps2e::PresentPacer pacer;
        int pacer_rate = -1;
        bool interpolate = true;
        int target_slot = -1;
        double target_fraction = -1;
        ps2r::ps2e::SceneInterpolator interpolator;
        std::vector<ps2r::ps2e::InterpOutput> interp_out;
        uint64_t last_noted_picture = 0;

        int64_t arrival_ages[64] = {};
        int arrival_count = 0;
        int64_t delay_ns = 0;
        ~RenderThread() {
            { std::lock_guard<std::mutex> lock(mutex); stop = true; }
            wake.notify_all();
            if (thread.joinable()) thread.join();
        }
    } rt;
    std::atomic<int> frame_rate{1};
    std::atomic<bool> interp_active{false};
    ps2r::ps2e::InterpTag tag;
    ps2r::ps2e::InterpTransform tag_transform{};
    bool tag_has_transform = false;
    std::atomic<uint64_t> pictures{0};
    uint64_t pictures_handed = 0;

    std::vector<uint32_t> mesh_retired;
    std::vector<uint32_t> texture_retired;
    bool picture_thin = false;

    uint64_t shown30_boundary = 0;
    bool shown30_any = false;

    int native_meshes = -1;
} g;

inline void PrefetchNextDraw(const Gfx::Bucket& b) {
    if (b.used >= b.draws.size()) return;
    const char* p = reinterpret_cast<const char*>(&b.draws[b.used]);
    for (size_t o = 0; o < sizeof(Draw); o += 64) __builtin_prefetch(p + o, 1, 3);
}

inline void PrefetchDraw(const Draw& d) {
    const char* p = reinterpret_cast<const char*>(&d);
    for (size_t o = 0; o < sizeof(Draw); o += 64) __builtin_prefetch(p + o, 0, 3);
}

void RenameTexture(uint32_t made, uint32_t id) {
    const bool renamed = ps2e_draw_texture_rename(g.dev, made, id);
    if (made && !renamed) ps2e_draw_texture_destroy(g.dev, made);
}

void DestroyTexture(uint32_t handle) { ps2e_draw_texture_destroy(g.dev, handle); }
void DestroyMesh(uint32_t handle) { ps2e_draw_mesh_destroy(g.dev, handle); }

void ExpandDeferred(Gfx::Bucket* buckets, std::vector<std::unique_ptr<DeferredBuild>>& deferred, size_t& used);
void ExpandDeferredShared(Gfx::Bucket* buckets, std::vector<std::unique_ptr<DeferredBuild>>& deferred, size_t& used);
Gfx::Bucket* ExpandScratch();
void ExpandBucket(Gfx::Bucket* buckets, int b, Gfx::Bucket* scratch);

void ReadOptions() {
    static bool done = false;
    if (done) return;
    done = true;
    g.rt.enabled = true;
}

enum class Show { Present, Capture, None };
void RenderFrame(Gfx::Frame& f, Show show);
void RenderBetween(Gfx::Frame& prev, Gfx::Frame& cur, double fraction, int64_t slot_ns);
using Steady = std::chrono::steady_clock;
int64_t NowNs() { return ps2r::ps2e::PresentPacer::NowNs(); }

using Rate = ps2r::ps2e::PresentPacer::Rate;

void RetainPicture(int slot) {
    auto& rt = g.rt;
    if (rt.prev >= 0) {

        for (uint32_t mesh : rt.slots[rt.prev].retire) DestroyMesh(mesh);
        rt.slots[rt.prev].retire.clear();
        for (uint32_t tex : rt.slots[rt.prev].retire_tex) DestroyTexture(tex);
        rt.slots[rt.prev].retire_tex.clear();
        rt.free.push_back(rt.prev);
    }
    rt.prev = rt.cur;
    rt.cur = slot;
}

int64_t TickPeriodNs() {
    return 1000000000LL / 60;
}

bool SlotPaced(Rate rate) { return ps2r::ps2e::PresentPacer::PacedBySlots(rate); }

void RenderLoop() {
    auto& rt = g.rt;

    ps2r::platform::host::PinCurrentThreadInteractive("te-render", ps2r::platform::host::ThreadRole::Graphics);
    if (!ps2e_draw_bind_thread(g.dev, 1)) std::fprintf(stderr, "[reNsanity] graphics: the render thread could not bind the device\n");
    ps2r::host::RaiseTimerResolution();

    for (;;) {
        std::unique_lock<std::mutex> lock(rt.mutex);

        const int rate_index = g.frame_rate.load(std::memory_order_relaxed);
        if (rate_index != rt.pacer_rate) {
            rt.pacer_rate = rate_index;
            rt.pacer.SetGameHz(60);
            rt.pacer.SetRate(static_cast<Rate>(rate_index));
            g.interp_active.store(rt.interpolate && SlotPaced(static_cast<Rate>(rate_index)), std::memory_order_relaxed);
            rt.interpolator.ResetHistory();
        }
        const Rate rate = static_cast<Rate>(rt.pacer_rate);
        const bool slots = !rt.stop && SlotPaced(rate) && rt.cur >= 0 && rt.slots[rt.cur].have_edge;

        if (slots) {
            const int64_t slot = rt.pacer.NextSlotNs(NowNs());
            const int64_t wake_at = slot - 1000000;
            rt.wake.wait_until(lock, Steady::time_point(std::chrono::nanoseconds(wake_at)),
                               [&] { return rt.stop || !rt.queue.empty() || !rt.jobs.empty() || NowNs() >= wake_at; });
        } else {
            rt.wake.wait(lock, [&] { return rt.stop || !rt.queue.empty() || !rt.jobs.empty(); });
        }

        if (rt.stop && rt.queue.empty() && rt.jobs.empty()) break;
        if (!rt.jobs.empty()) {
            std::vector<std::function<void()>> jobs = std::move(rt.jobs);
            rt.jobs.clear();
            lock.unlock();
            for (auto& job : jobs) job();
            lock.lock();
            rt.jobs_done += jobs.size();
            rt.done.notify_all();
            continue;
        }
        if (!rt.queue.empty()) {

            const int slot = rt.queue.front();
            rt.queue.erase(rt.queue.begin());
            rt.drawing = true;
            RetainPicture(slot);

            rt.done.notify_all();
            Gfx::Frame& f = rt.slots[slot];
            if (f.have_edge) {
                rt.arrival_ages[rt.arrival_count % 64] = NowNs() - f.edge_ns;
                rt.arrival_count++;
                const int n = std::min(rt.arrival_count, 64);
                int64_t sorted[64];
                std::copy(rt.arrival_ages, rt.arrival_ages + n, sorted);
                std::sort(sorted, sorted + n);
                const int64_t period = TickPeriodNs();
                const int64_t p90 = sorted[(n * 9) / 10 >= n ? n - 1 : (n * 9) / 10];

                rt.delay_ns = std::clamp<int64_t>(p90, 0, period);
            }
            lock.unlock();

            const bool shows = (rate == Rate::Rate30 ? f.shown30 : rt.pacer.ShowsPicture(f.picture)) && !f.thin;
            const bool lazy = !SlotPaced(rate) && shows;
            if (!lazy) {
                ExpandDeferredShared(f.buckets, f.deferred, f.deferred_used);
            }
            if (SlotPaced(rate)) {

                if (f.have_edge) rt.pacer.OnTickEdge(f.edge_ns);
                RenderFrame(f, Show::None);
            } else if (shows) {

                RenderFrame(f, Show::Present);
                ExpandDeferred(f.buckets, f.deferred, f.deferred_used);
            } else {
                RenderFrame(f, Show::None);
            }
            lock.lock();
            rt.drawing = false;
            rt.done.notify_all();
            continue;
        }
        if (slots) {
            const int64_t now = NowNs();
            const int64_t slot_ns = rt.pacer.NextSlotNs(now);
            if (now < slot_ns) {
                lock.unlock();
                {
                    ps2r::ps2e::PresentPacer::SleepUntilNs(slot_ns);
                }
                lock.lock();
                if (!rt.queue.empty() || !rt.jobs.empty() || rt.stop) continue;
            }

            const int cur = rt.cur, prev = rt.prev;
            rt.drawing = true;
            lock.unlock();
            const int64_t t = NowNs();
            const int64_t period = TickPeriodNs();
            double fraction = 1.0;
            if (rt.interpolate && prev >= 0 && rt.slots[prev].have_edge && rt.slots[cur].send && rt.slots[prev].send) {
                fraction = double(t - rt.delay_ns - rt.slots[cur].edge_ns) / double(period);
                if (fraction < 0.0) fraction = 0.0;
                if (fraction > 1.0) fraction = 1.0;
            }
            if (fraction >= 0.97) fraction = 1.0;
            RenderBetween(prev >= 0 ? rt.slots[prev] : rt.slots[cur], rt.slots[cur], fraction, slot_ns);
            lock.lock();
            rt.pacer.SlotServed(slot_ns, t);
            rt.drawing = false;
            rt.done.notify_all();
            continue;
        }
        if (rt.stop) break;
    }
    ps2e_draw_bind_thread(g.dev, 0);
}
bool OnRenderThread() { return g.rt.started && std::this_thread::get_id() == g.rt.thread.get_id(); }

__attribute__((noinline)) void OnRender(const std::function<void()>& fn) {
    auto& rt = g.rt;
    if (!rt.started || OnRenderThread()) { fn(); return; }
    uint64_t ticket;
    {
        std::lock_guard<std::mutex> lock(rt.mutex);
        rt.jobs.push_back(fn);
        ticket = ++rt.jobs_posted;
    }
    rt.wake.notify_one();
    std::unique_lock<std::mutex> lock(rt.mutex);
    rt.done.wait(lock, [&] { return rt.jobs_done >= ticket; });
}

void PostRender(std::function<void()> fn) {
    auto& rt = g.rt;
    if (!rt.started || OnRenderThread()) { fn(); return; }
    {
        std::lock_guard<std::mutex> lock(rt.mutex);
        rt.jobs.push_back(std::move(fn));
        ++rt.jobs_posted;
    }
    rt.wake.notify_one();
}

void RetireTexture(uint32_t handle) {
    if (!handle) return;
    g.texture_retired.push_back(handle);
}
void StartBuilds();
void StopRenderThread();
void StartRenderThread() {
    auto& rt = g.rt;
    if (!rt.enabled || rt.started || !g.dev) return;
    if (!ps2e_draw_bind_thread(g.dev, 0)) {
        rt.enabled = false;
        return;
    }
    for (int i = 0; i < Gfx::RenderThread::kSlots; ++i) rt.free.push_back(i);

    StartBuilds();
    rt.started = true;

    rt.thread = std::thread([rounding = std::fegetround()] {
        std::fesetround(rounding);
        RenderLoop();
    });
    std::atexit(StopRenderThread);
}

void StopRenderThread() {
    auto& rt = g.rt;
    if (!rt.started) return;
    { std::lock_guard<std::mutex> lock(rt.mutex); rt.stop = true; }
    rt.wake.notify_all();
    if (rt.thread.joinable()) rt.thread.join();
    rt.started = false;
    ps2e_draw_bind_thread(g.dev, 1);

    std::vector<std::function<void()>> jobs;
    {
        std::lock_guard<std::mutex> lock(rt.mutex);
        jobs.swap(rt.jobs);
    }
    for (auto& job : jobs) job();
    {
        std::lock_guard<std::mutex> lock(rt.mutex);
        rt.jobs_done += jobs.size();
    }
    rt.done.notify_all();
}

void PrimeThreadSync();
bool EnsureDevice() {
    ReadOptions();
    if (g.dev) return true;
    if (g.create_failed) return false;
    ps2e_draw_config cfg{};
    cfg.frame_width = g.display_w;
    cfg.frame_height = g.display_h;
    PrimeThreadSync();
    g.dev = ps2e_draw_create(&cfg);
    if (!g.dev) {
        g.create_failed = true;
        std::fprintf(stderr, "[reNsanity] graphics: the device could not be created\n");
        return false;
    }
    te::display::Apply();
    StartRenderThread();

    return true;
}

int ScreenW() { return g.have_frame && g.frame.screenWidth > 0 ? g.frame.screenWidth : g.display_w; }
int ScreenH() { return g.have_frame && g.frame.screenHeight > 0 ? g.frame.screenHeight : g.display_h; }
float GsOriginX() { return kGsMiddle - float(ScreenW() >> 1); }
float GsOriginY() { return kGsMiddle - float(ScreenH() >> 1); }

const RenderTargetDescription* CurrentTarget() {
    const uint32_t* p = Guest<uint32_t>(kRenderTargetPointer);
    if (!p || !*p) return nullptr;
    const auto* t = Guest<RenderTargetDescription>(*p);
    if (!t || t->displayWidth <= 0 || t->displayWidth > 4096 || t->displayHeight <= 0 || t->displayHeight > 4096) return nullptr;
    return t;
}

float FrameCornerX(int offset) {
    const RenderTargetDescription* t = CurrentTarget();
    const int display = t ? t->displayWidth : g.frame.screenWidth;
    return float(int(kGsMiddle - float(display >> 1)) + offset);
}
float FrameCornerY(int offset) {
    const RenderTargetDescription* t = CurrentTarget();
    const int display = t ? t->displayHeight : g.frame.screenHeight;
    return float(int(kGsMiddle - float(display >> 1)) + offset);
}
int TargetOffsetX() {
    const RenderTargetDescription* t = CurrentTarget();
    return t ? t->offsetX : 0;
}
int TargetOffsetY() {
    const RenderTargetDescription* t = CurrentTarget();
    return t ? t->offsetY : 0;
}

float GameScreenW() {
    const int32_t* p = Guest<int32_t>(kScreenWidth);
    return p && *p > 0 && *p <= 4096 ? float(*p) : float(ScreenW());
}
float GameScreenH() {
    const int32_t* p = Guest<int32_t>(kScreenHeight);
    return p && *p > 0 && *p <= 4096 ? float(*p) : float(ScreenH());
}

struct TextureLookupEntry { uint64_t epoch=0; uint32_t address=0, handle=0; };
TextureLookupEntry g_textureLookup[2048];
uint64_t g_textureLookupEpoch=1;
void InvalidateTextureLookup() {
    if (++g_textureLookupEpoch == 0) {
        for (auto& e:g_textureLookup) e.epoch=0;
        g_textureLookupEpoch=1;
    }
}
uint32_t TextureHandle(uint32_t address) {

    auto& e=g_textureLookup[((address>>4)^(address>>15))&2047];
    if(e.epoch==g_textureLookupEpoch && e.address==address) {
        return e.handle;
    }

    auto it=g.textures.find(address);e.handle=it==g.textures.end()?0:it->second;
    e.address=address;e.epoch=g_textureLookupEpoch;return e.handle;
}

MaterialView ReadMaterial(uint32_t address) {
    MaterialView v;
    v.material = Guest<te_gfx_material>(address);
    if (!v.material || v.material->magic != TE_GFX_MATERIAL_MAGIC) {
        v.material = nullptr;
        return v;
    }
    v.bucket = v.material->bucket < TE_GFX_BUCKET_COUNT ? v.material->bucket : TE_GFX_BUCKET_UI;
    if (v.material->shaderCount && v.material->shaders[0]) {
        v.shader = Guest<te_gfx_shader>(v.material->shaders[0]);
        if (v.shader && v.shader->magic != TE_GFX_SHADER_MAGIC) v.shader = nullptr;
    }
    if (v.shader && v.shader->texture) {
        v.texture = Guest<te_gfx_texture>(v.shader->texture);
        if (v.texture) __builtin_prefetch(v.texture, 0, 3);
        v.texture_handle = TextureHandle(v.shader->texture);
    }
    return v;
}

MaterialView ShaderView(const MaterialView& m, uint32_t k) {
    MaterialView v;
    v.material = m.material;
    v.bucket = m.bucket;
    if (!v.material || k >= v.material->shaderCount || k >= TE_GFX_MAX_SHADERS || !v.material->shaders[k]) return v;
    v.shader = Guest<te_gfx_shader>(v.material->shaders[k]);
    if (v.shader && v.shader->magic != TE_GFX_SHADER_MAGIC) v.shader = nullptr;
    if (v.shader && v.shader->texture) {
        v.texture = Guest<te_gfx_texture>(v.shader->texture);
        v.texture_handle = TextureHandle(v.shader->texture);
    }
    return v;
}

void AlphaPreset(uint32_t preset, ps2e_draw_state& st) {

    switch (preset) {
        case TE_GFX_PRESET_MIX: st.blend_a = 0; st.blend_b = 1; st.blend_c = 0; st.blend_d = 1; break;
        case TE_GFX_PRESET_ADD: st.blend_a = 0; st.blend_b = 2; st.blend_c = 0; st.blend_d = 1; break;
        case TE_GFX_PRESET_SUBTRACT: st.blend_a = 2; st.blend_b = 0; st.blend_c = 0; st.blend_d = 1; break;
        case TE_GFX_PRESET_BRIGHTEN: st.blend_a = 1; st.blend_b = 2; st.blend_c = 0; st.blend_d = 1; break;
        case TE_GFX_PRESET_DARKEN: st.blend_a = 2; st.blend_b = 1; st.blend_c = 0; st.blend_d = 1; break;
        case TE_GFX_PRESET_SCALE: st.blend_a = 1; st.blend_b = 2; st.blend_c = 0; st.blend_d = 2; break;
        default: st.blend_a = 0; st.blend_b = 2; st.blend_c = 2; st.blend_d = 2; st.blend_fix = 0x80; break;
    }
}

bool CutOut(const std::vector<uint32_t>& px, int w, int h) {
    if (w < 2 || h < 2 || px.size() < size_t(w) * h) return false;
    size_t zero = 0, opaque = 0, mid = 0;
    const size_t n = size_t(w) * h;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t a = px[i] >> 24;
        if (a == 0) ++zero; else if (a >= 200) ++opaque; else ++mid;
    }
    return !(zero * 50 < n || opaque * 10 < n || mid * 5 > n);
}
bool BleedLevel(std::vector<uint32_t>& px, int w, int h) {
    if (!CutOut(px, w, h)) return false;
    const size_t n = size_t(w) * h;
    std::vector<uint8_t> known(n);
    for (size_t i = 0; i < n; ++i) known[i] = (px[i] >> 24) >= 128;
    std::vector<uint32_t> out(px);
    bool any = false;
    for (int pass = 0; pass < 2; ++pass) {

        std::vector<uint8_t> grown(known);
        size_t filled = 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = size_t(y) * w + x;
                if (known[i] || (px[i] >> 24) != 0) continue;
                unsigned r = 0, g = 0, b = 0, c = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int xx = x + dx, yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                        const size_t j = size_t(yy) * w + xx;
                        if (!known[j]) continue;
                        const uint32_t v = out[j];
                        r += v & 255; g += (v >> 8) & 255; b += (v >> 16) & 255; ++c;
                    }
                if (!c) continue;
                out[i] = (r / c) | ((g / c) << 8) | ((b / c) << 16);
                grown[i] = 1;
                ++filled;
            }
        if (!filled) break;
        any = true;
        known.swap(grown);
    }
    if (any) px.swap(out);
    return any;
}
bool BleedTexture(te::gfx::DecodedTexture& d) {
    bool any = BleedLevel(d.rgba, d.width, d.height);
    int w = d.width, h = d.height;
    for (auto& level : d.mips) {
        w = std::max(1, w >> 1); h = std::max(1, h >> 1);
        any |= BleedLevel(level, w, h);
    }
    return any;
}

uint32_t CreateTexture(const te::gfx::DecodedTexture& decoded_in) {

    te::gfx::DecodedTexture bled;
    const te::gfx::DecodedTexture* use = &decoded_in;
    if (CutOut(decoded_in.rgba, decoded_in.width, decoded_in.height)) {
        bled = decoded_in;
        if (BleedTexture(bled)) use = &bled;
    }
    const te::gfx::DecodedTexture& decoded = *use;
    const uint32_t handle = ps2e_draw_texture_create_gpu_alpha(g.dev, decoded.width, decoded.height, decoded.rgba.data());
    if (handle && !decoded.mips.empty()) {
        std::vector<const void*> levels;
        for (const auto& level : decoded.mips) levels.push_back(level.data());
        ps2e_draw_texture_set_mips(g.dev, handle, int(levels.size()), levels.data());
    }
    return handle;
}

bool PostsTextures() {
    return g.rt.started && !OnRenderThread();
}

bool PackAsync() {
    if (!PostsTextures()) return false;
    return g.rt.started && !OnRenderThread();
}
constexpr int kPackSwapsAhead = 6;
void ApplyPackReplacement(uint32_t id, const std::shared_ptr<Gfx::PackLoad>& load, const te::gfx::texpack::Replacement& rep,
                          int width, int height) {
    namespace tp = te::gfx::texpack;
    {
        std::lock_guard<std::mutex> lock(load->m);
        if (load->released) return;
    }
    const auto drop = [&]() {
        std::lock_guard<std::mutex> lock(load->m);
        if (load->bytes) tp::Released(load->bytes);
        load->bytes = 0;
    };
    uint64_t census[3];
    if (!ps2e_draw_texture_census(g.dev, id, census)) return drop();
    std::vector<const void*> data;
    for (size_t off : rep.level_offset) data.push_back(rep.bytes.data() + off);
    const uint32_t handle = ps2e_draw_texture_create_replacement(g.dev, width, height, rep.width / width, int(rep.format), int(data.size()),
                                                                 data.data(), rep.level_size.data());
    if (!handle) return drop();
    DestroyTexture(id);
    RenameTexture(handle, id);
}

void StartPackRead(uint32_t id, std::shared_ptr<Gfx::PackLoad> load, std::shared_ptr<std::vector<uint32_t>> texels, int width, int height) {
    namespace tp = te::gfx::texpack;
    tp::Async([id, load, texels, width, height]() mutable {
        const uint64_t key = tp::Key(texels->data(), width, height);
        texels.reset();
        if (!tp::Has(key, width, height)) return;
        tp::Replacement rep;
        std::string why;
        if (!tp::Find(key, width, height, &rep, &why)) return;
        {
            std::lock_guard<std::mutex> lock(load->m);
            if (load->released || !g.rt.started) { tp::Released(rep.bytes.size()); return; }
            load->bytes = rep.bytes.size();
        }
        auto r = std::make_shared<tp::Replacement>(std::move(rep));

        for (int waited = 0; g.pack_backlog.load(std::memory_order_relaxed) >= kPackSwapsAhead && waited < 200; ++waited)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        g.pack_backlog.fetch_add(1, std::memory_order_relaxed);
        PostRender([id, load, r, width, height] {
            g.pack_backlog.fetch_sub(1, std::memory_order_relaxed);
            ApplyPackReplacement(id, load, *r, width, height);
        });
    });
}

uint32_t LoadTexture(te::gfx::DecodedTexture& decoded, bool post = false) {
    uint32_t handle = 0;
    namespace tp = te::gfx::texpack;
    if (tp::Enabled() && !decoded.rgba.empty()) {
        const uint64_t key = tp::Key(decoded.rgba.data(), decoded.width, decoded.height);
        tp::Replacement rep;
        std::string why;
        if (tp::Find(key, decoded.width, decoded.height, &rep, &why)) {
            std::vector<const void*> data;
            for (size_t off : rep.level_offset) data.push_back(rep.bytes.data() + off);
            const int scale = rep.width / decoded.width;
            OnRender([&] {
                handle = ps2e_draw_texture_create_replacement(g.dev, decoded.width, decoded.height, scale, int(rep.format),
                                                              int(data.size()), data.data(), rep.level_size.data());
            });
            if (handle) {
                g.replacement_bytes[handle] = rep.bytes.size();
                return handle;
            }
            tp::Released(rep.bytes.size());
        }
    }
    if (post && PostsTextures()) {
        const uint32_t id = ps2e_draw_texture_reserve(g.dev);
        auto made = std::make_shared<te::gfx::DecodedTexture>(std::move(decoded));
        PostRender([made, id] {
            const uint32_t h = CreateTexture(*made);
            if (h) RenameTexture(h, id);
        });
        return id;
    }
    OnRender([&] { handle = CreateTexture(decoded); });
    return handle;
}
void ReleaseReplacement(uint32_t handle) {
    auto pending = g.pack_loads.find(handle);
    if (pending != g.pack_loads.end()) {
        std::lock_guard<std::mutex> lock(pending->second->m);
        pending->second->released = true;
        if (pending->second->bytes) te::gfx::texpack::Released(pending->second->bytes);
        pending->second->bytes = 0;
        g.pack_loads.erase(pending);
        return;
    }
    auto it = g.replacement_bytes.find(handle);
    if (it == g.replacement_bytes.end()) return;
    te::gfx::texpack::Released(it->second);
    g.replacement_bytes.erase(it);
}

struct TexCache {

    struct View {
        uint16_t width_power, height_power;
        uint8_t levels, pixel_format, upload_format, has_alpha, function, pad[3];
        uint32_t level_offsets[7], level_widths[7], palette_offset;
        static View Of(const te_gfx_texture_header& h) {
            View v;
            std::memset(&v, 0, sizeof v);
            v.width_power = h.widthPower;
            v.height_power = h.heightPower;
            v.levels = h.levels;
            v.pixel_format = h.pixelFormat;
            v.upload_format = h.uploadFormat;
            v.has_alpha = h.hasAlpha ? 1 : 0;
            v.function = h.function & 3u;
            std::memcpy(v.level_offsets, h.levelOffsets, sizeof v.level_offsets);
            std::memcpy(v.level_widths, h.levelWidths, sizeof v.level_widths);
            v.palette_offset = h.paletteOffset;
            return v;
        }
    };
    struct Entry {
        uint64_t key = 0;
        View view{};
        std::vector<uint8_t> packet;
        uint32_t refs = 0;
        uint64_t bytes = 0;
        uint64_t parked_at = 0;
    };
    bool read = false;
    uint64_t budget = 0;
    std::unordered_multimap<uint64_t, uint32_t> by_key;
    std::unordered_map<uint32_t, Entry> entries;
    std::map<uint64_t, uint32_t> parked;
    uint64_t parked_bytes = 0, turn = 0;
} g_texcache;

TexCache& TexCacheState() {
    TexCache& c = g_texcache;
    if (c.read) return c;
    c.read = true;
    long mb = te::gfx::texpack::SmallProcess() ? 32 : 256;
    c.budget = uint64_t(mb) << 20;
    return c;
}

uint64_t TexCacheKey(const te_gfx_texture_header& header, const uint8_t* packet, size_t size) {
    const TexCache::View h = TexCache::View::Of(header);
    uint64_t x = 0x9E3779B97F4A7C15ull ^ uint64_t(size);
    const auto mix = [&x](uint64_t w) {
        x ^= w * 0xBF58476D1CE4E5B9ull;
        x = (x << 27 | x >> 37) * 0x94D049BB133111EBull;
    };
    const auto run = [&mix](const uint8_t* p, size_t n) {
        size_t i = 0;
        for (; i + 8 <= n; i += 8) {
            uint64_t w;
            std::memcpy(&w, p + i, 8);
            mix(w);
        }
        uint64_t tail = 0;
        for (size_t k = 0; i < n; ++i, ++k) tail |= uint64_t(p[i]) << (8 * k);
        mix(tail ^ 0xFF00000000000000ull);
    };
    run(reinterpret_cast<const uint8_t*>(&h), sizeof h);
    run(packet, size);
    return x ^ (x >> 31);
}

uint32_t TexCacheFind(uint64_t key, const te_gfx_texture_header& h, const uint8_t* packet, size_t size) {
    TexCache& c = TexCacheState();
    if (!c.budget) return 0;
    const TexCache::View view = TexCache::View::Of(h);
    const auto range = c.by_key.equal_range(key);
    for (auto it = range.first; it != range.second; ++it) {
        auto e = c.entries.find(it->second);
        if (e == c.entries.end() || e->second.packet.size() != size || std::memcmp(&e->second.view, &view, sizeof view) != 0 ||
            std::memcmp(e->second.packet.data(), packet, size) != 0)
            continue;
        TexCache::Entry& entry = e->second;
        if (entry.refs == 0) {
            c.parked.erase(entry.parked_at);
            c.parked_bytes -= entry.bytes;
        }
        ++entry.refs;
        return e->first;
    }
    return 0;
}

uint64_t DecodedBytes(const te::gfx::DecodedTexture& d) {
    uint64_t n = uint64_t(d.rgba.size()) * 4u;
    for (const auto& level : d.mips) n += uint64_t(level.size()) * 4u;
    return n;
}

void TexCacheAdd(uint32_t handle, uint64_t key, const te_gfx_texture_header& h, const uint8_t* packet, size_t size, uint64_t bytes) {
    TexCache& c = TexCacheState();
    if (!c.budget || !handle || g.replacement_bytes.count(handle) || g.pack_loads.count(handle)) return;
    TexCache::Entry e;
    e.key = key;
    e.view = TexCache::View::Of(h);
    e.packet.assign(packet, packet + size);
    e.refs = 1;
    e.bytes = bytes + size;
    c.entries[handle] = std::move(e);
    c.by_key.emplace(key, handle);
}

void DestroyTextureHandle(uint32_t handle) {
    RetireTexture(handle);
}

void TexCacheForget(TexCache& c, uint32_t handle) {
    auto e = c.entries.find(handle);
    if (e == c.entries.end()) return;
    const auto range = c.by_key.equal_range(e->second.key);
    for (auto it = range.first; it != range.second; ++it)
        if (it->second == handle) { c.by_key.erase(it); break; }
    c.entries.erase(e);
}

void ReleaseTextureHandle(uint32_t handle) {
    TexCache& c = TexCacheState();
    auto e = c.entries.find(handle);
    if (e == c.entries.end()) {
        DestroyTextureHandle(handle);
        return;
    }
    if (e->second.refs > 1) {
        --e->second.refs;
        return;
    }
    e->second.refs = 0;
    e->second.parked_at = ++c.turn;
    c.parked[e->second.parked_at] = handle;
    c.parked_bytes += e->second.bytes;
    while (c.parked_bytes > c.budget && !c.parked.empty()) {
        const uint32_t oldest = c.parked.begin()->second;
        c.parked.erase(c.parked.begin());
        auto o = c.entries.find(oldest);
        if (o != c.entries.end()) c.parked_bytes -= o->second.bytes;
        TexCacheForget(c, oldest);
        DestroyTextureHandle(oldest);
    }
}

bool MinifiesThroughMips(uint32_t type) {
    switch (type) {
        case 0x01: case 0x02: case 0x04: case 0x0D: case 0x14: case 0x15: case 0x16: case 0x17: case 0x19:
        case 0x1A: case 0x1B: case 0x1E: case 0x1F: case 0x20:
            return true;
        default:
            return false;
    }
}

ps2e_draw_state StateOf(const MaterialView& m) {
    ps2e_draw_state st{};
    st.gouraud = 1;
    st.depth_test = PS2E_DRAW_DEPTH_ALWAYS;
    st.depth_write = 0;
    st.tcc = 1;
    if (!m.shader) return st;
    const uint64_t s = m.shader->settings;
    st.gouraud = TE_GFX_S_GOURAUD(s) ? 1 : 0;
    st.depth_test = int(TE_GFX_S_DEPTH_TEST(s));
    st.depth_write = TE_GFX_S_NO_DEPTH_WRITES(s) ? 0 : 1;
    st.blend = TE_GFX_S_BLENDS(s) ? 1 : 0;
    if (st.blend) {
        if (TE_GFX_S_OWN_ALPHA(s)) {
            st.blend_a = uint8_t(TE_GFX_S_ALPHA_A(s));
            st.blend_b = uint8_t(TE_GFX_S_ALPHA_B(s));
            st.blend_c = uint8_t(TE_GFX_S_ALPHA_C(s));
            st.blend_d = uint8_t(TE_GFX_S_ALPHA_D(s));
            st.blend_fix = uint8_t(TE_GFX_S_ALPHA_FIX(s));
        } else {
            AlphaPreset(TE_GFX_S_PRESET(s), st);
        }
    }
    st.alpha_test = TE_GFX_S_ALPHA_TEST(s) ? 1 : 0;
    st.alpha_method = int(TE_GFX_S_ALPHA_METHOD(s));
    st.alpha_reference = uint8_t(TE_GFX_S_ALPHA_REFERENCE(s));
    st.alpha_fail = int(TE_GFX_S_ALPHA_FAIL(s));
    st.linear = TE_GFX_S_LINEAR(s) ? 1 : 0;

    st.fba = TE_GFX_S_NO_FBA(s) ? 0 : 1;
    if (TE_GFX_S_TEXTURED(s) && m.texture_handle) {
        st.texture = m.texture_handle;
        st.tfx = m.texture ? (m.texture->texture.function & 3) : PS2E_DRAW_TFX_MODULATE;
        st.tcc = m.texture ? (m.texture->texture.hasAlpha ? 1 : 0) : 1;
        st.wrap_u = st.wrap_v = PS2E_DRAW_WRAP_REPEAT;

        if (m.texture && m.texture->texture.levels > 1 && MinifiesThroughMips(m.shader->type)) {
            st.mip_max_level = m.texture->texture.levels - 1;
            st.mip_k = m.shader->lodK;
            st.mip_l = m.shader->lodL;
            st.mip_minify = 4;
            if (m.shader->type == 0x0D) st.linear = 1;
        }
    }
    return st;
}

ps2e_draw_vertex Vertex(float x, float y, float u, float v, uint32_t colour) {
    ps2e_draw_vertex o{};
    o.x = x;
    o.y = y;
    o.z = kGsDepthFront;
    o.u = u;
    o.v = v;
    o.r = uint8_t(colour);
    o.g = uint8_t(colour >> 8);
    o.b = uint8_t(colour >> 16);
    o.a = uint8_t(colour >> 24);
    return o;
}

bool SameState(const ps2e_draw_state& a, const ps2e_draw_state& b) {
    if (a.stencil != b.stencil) return false;
    if (a.fba != b.fba || a.dest_alpha_test != b.dest_alpha_test) return false;
    if (a.mip_max_level != b.mip_max_level || a.mip_k != b.mip_k || a.mip_l != b.mip_l || a.mip_minify != b.mip_minify) return false;
    return a.texture == b.texture && a.gouraud == b.gouraud && a.depth_test == b.depth_test && a.depth_write == b.depth_write &&
           a.blend == b.blend && a.blend_a == b.blend_a && a.blend_b == b.blend_b && a.blend_c == b.blend_c &&
           a.blend_d == b.blend_d && a.blend_fix == b.blend_fix && a.alpha_test == b.alpha_test &&
           a.alpha_method == b.alpha_method && a.alpha_reference == b.alpha_reference && a.alpha_fail == b.alpha_fail &&
           a.linear == b.linear && a.wrap_u == b.wrap_u && a.wrap_v == b.wrap_v && a.tfx == b.tfx && a.tcc == b.tcc &&
           a.scissor_x0 == b.scissor_x0 && a.scissor_y0 == b.scissor_y0 && a.scissor_x1 == b.scissor_x1 &&
           a.scissor_y1 == b.scissor_y1 && a.no_colour_write == b.no_colour_write;
}

bool BatchableState(const ps2e_draw_state& state) {
    if (state.blend == 0) return true;

    return state.stencil == PS2E_DRAW_STENCIL_OFF && !state.dest_alpha_test &&
           state.texture != PS2E_DRAW_TEXTURE_FRAME_COPY;
}

bool BatchFiniteQ(const ps2e_draw_vertex* vertices, size_t count);
Draw& OpenDraw(uint32_t bucket, const ps2e_draw_state& state, bool copy_frame = false, bool finite_q = false) {
    auto& b = g.buckets[bucket < TE_GFX_BUCKET_COUNT ? bucket : TE_GFX_BUCKET_UI];

    if (b.used && finite_q && state.blend == 0 && !copy_frame) {
        Draw& last = b.draws[b.used - 1];
        if (!last.mesh && !last.pending && last.verts.size() >= 3 && last.verts.size() % 3 == 0 &&
            BatchFiniteQ(last.verts.data(), last.verts.size()) && SameState(last.state, state)) {

            const bool same_object = !g.interp_active.load(std::memory_order_relaxed) ||
                                     (last.tag.id == g.tag.id && last.tag.kind == g.tag.kind &&
                                      (g.tag.kind != ps2r::ps2e::InterpKind::Vertices || last.tag.part == g.tag.part));
            if (same_object) {
                return last;
            }
        }
    }
    if (b.used == b.draws.size()) b.draws.emplace_back();
    Draw& d = b.draws[b.used++];
    PrefetchNextDraw(b);
    d.state = state;
    d.verts.clear();
    d.copy_frame = copy_frame;
    d.pending = nullptr;
    d.mesh = 0;
    d.bones.clear();
    d.skin_disp = 0;
    d.tag = g.tag;
    d.has_transform = g.tag_has_transform;
    if (g.tag_has_transform) d.transform = g.tag_transform;
    return d;
}

void SetDrawTag(const ps2r::ps2e::InterpTag& tag, const float* world16, const float* screen16) {
    g.tag = tag;
    g.tag_has_transform = world16 && screen16;
    if (g.tag_has_transform) {
        std::memcpy(g.tag_transform.world, world16, sizeof g.tag_transform.world);
        std::memcpy(g.tag_transform.screen, screen16, sizeof g.tag_transform.screen);
    }
}
void ClearDrawTag() {
    g.tag = ps2r::ps2e::InterpTag{};
    g.tag_has_transform = false;
}

void CloseDraw(uint32_t bucket) {
    auto& b = g.buckets[bucket < TE_GFX_BUCKET_COUNT ? bucket : TE_GFX_BUCKET_UI];
    if (!b.used) return;
    Draw& d = b.draws[b.used - 1];
    d.verts.resize(d.verts.size() / 3 * 3);
    if (d.verts.size() < 3) {
        d.verts.clear();
        b.used--;
    }
}

void Record(uint32_t bucket, Draw&& d) {
    if (d.verts.size() < 3) return;
    Draw& open = OpenDraw(bucket, d.state, d.copy_frame, BatchFiniteQ(d.verts.data(), d.verts.size()));
    open.verts.insert(open.verts.end(), d.verts.begin(), d.verts.end());
    CloseDraw(bucket);
}

using PoolMutex = std::mutex;
class PoolCondition : public std::condition_variable {
public:
    explicit PoolCondition(PoolMutex&) {}
};

struct alignas(64) BuildCell {
    std::atomic<size_t> seq{0};
    DeferredBuild* build = nullptr;
    int rounding = 0;
    BuildJob job;
};

inline void CpuRelax() {
    __asm__ __volatile__("yield");
}

struct BuildPool {
    static constexpr size_t kCells = 1024;
    std::vector<std::thread> threads;
    PoolMutex mutex;
    PoolCondition wake{mutex}, finished{mutex};
    std::unique_ptr<BuildCell[]> cells{new BuildCell[kCells]};
    alignas(64) std::atomic<size_t> enqueue_pos{0};
    alignas(64) std::atomic<size_t> dequeue_pos{0};
    alignas(64) std::atomic<int64_t> outstanding{0};
    std::atomic<int> idle_sleepers{0};
    std::atomic<int> spinners{0};
    std::atomic<bool> wake_pending{false};
    std::atomic<int> drainers{0};
    std::atomic<bool> stop{false};
    bool started = false;
    int64_t spin_ns = 0;

    struct Batch { void (*job)(void*, size_t) = nullptr; void* ctx = nullptr; int rounding = 0; };
    static constexpr uint64_t kBatchIndex = (1u << 20) - 1;
    Batch batch;
    uint64_t batch_epoch = 0;
    alignas(64) std::atomic<uint64_t> batch_word{0};
    alignas(64) std::atomic<size_t> batch_left{0};
    std::atomic<bool> batch_waiter{false};
    PoolCondition joined{mutex};
    int64_t join_spin_ns = 0;
    BuildPool() {
        for (size_t i = 0; i < kCells; ++i) cells[i].seq.store(i, std::memory_order_relaxed);
    }
    void Start() {
        started = true;
        const int n = 3;

        int64_t spin_us = 50;
        spin_ns = spin_us * 1000;

        int64_t join_spin_us = 40;
        join_spin_ns = join_spin_us * 1000;
        const int rounding = std::fegetround();
        for (int i = 0; i < n; ++i) threads.emplace_back([this, i, rounding] {
            std::fesetround(rounding);
            Run(i);
        });
    }

    bool TryPush(DeferredBuild& build, int rounding, BuildJob& job) {
        size_t pos = enqueue_pos.load(std::memory_order_relaxed);
        BuildCell* c;
        for (;;) {
            c = &cells[pos & (kCells - 1)];
            const size_t seq = c->seq.load(std::memory_order_acquire);
            const intptr_t dif = intptr_t(seq) - intptr_t(pos);
            if (dif == 0) {
                if (enqueue_pos.compare_exchange_weak(pos, pos + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) break;
            } else if (dif < 0) {
                return false;
            } else {
                pos = enqueue_pos.load(std::memory_order_relaxed);
            }
        }
        c->build = &build;
        c->rounding = rounding;
        c->job = std::move(job);
        c->seq.store(pos + 1, std::memory_order_seq_cst);
        return true;
    }
    BuildCell* TryPop(size_t& at) {
        size_t pos = dequeue_pos.load(std::memory_order_relaxed);
        BuildCell* c;
        for (;;) {
            c = &cells[pos & (kCells - 1)];
            const size_t seq = c->seq.load(std::memory_order_acquire);
            const intptr_t dif = intptr_t(seq) - intptr_t(pos + 1);
            if (dif == 0) {
                if (dequeue_pos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed, std::memory_order_relaxed)) break;
            } else if (dif < 0) {
                return nullptr;
            } else {
                pos = dequeue_pos.load(std::memory_order_relaxed);
            }
        }
        at = pos;
        return c;
    }

    bool HasWork() const {
        const size_t pos = dequeue_pos.load(std::memory_order_seq_cst);
        return cells[pos & (kCells - 1)].seq.load(std::memory_order_seq_cst) == pos + 1;
    }
    bool BatchOpen() const {
        const uint64_t w = batch_word.load(std::memory_order_seq_cst);
        return (w & kBatchIndex) < ((w >> 20) & kBatchIndex);
    }

    void WakeForBacklog() {
        if (idle_sleepers.load(std::memory_order_seq_cst) == 0 ||
            spinners.load(std::memory_order_seq_cst) != 0 || (!HasWork() && !BatchOpen()) ||
            wake_pending.exchange(true, std::memory_order_seq_cst)) return;
        { std::lock_guard<PoolMutex> lock(mutex); }
        wake.notify_one();
    }

    bool RunBatchChunks(bool worker) {
        bool ran = false;
        uint64_t w = batch_word.load(std::memory_order_acquire);
        for (;;) {
            const uint64_t next = w & kBatchIndex, total = (w >> 20) & kBatchIndex;
            if (next >= total) return ran;
            if (!batch_word.compare_exchange_weak(w, w + 1, std::memory_order_acq_rel, std::memory_order_acquire)) continue;
            WakeForBacklog();
            const Batch b = batch;
            const int saved = worker ? std::fegetround() : b.rounding;
            if (saved != b.rounding) std::fesetround(b.rounding);
            {
                b.job(b.ctx, size_t(next));
            }
            if (saved != b.rounding) std::fesetround(saved);

            if (batch_left.fetch_sub(1, std::memory_order_seq_cst) == 1 && batch_waiter.load(std::memory_order_seq_cst)) {
                { std::lock_guard<PoolMutex> lock(mutex); }
                joined.notify_all();
            }
            ran = true;
            w = batch_word.load(std::memory_order_acquire);
        }
    }

    void RunBatch(size_t chunks, void (*job)(void*, size_t), void* ctx) {
        if (!chunks) return;
        if (threads.empty() || chunks == 1 || chunks > kBatchIndex) {
            for (size_t k = 0; k < chunks; ++k) job(ctx, k);
            return;
        }
        batch = Batch{job, ctx, std::fegetround()};
        batch_left.store(chunks, std::memory_order_relaxed);
        batch_epoch = (batch_epoch + 1) & 0xFFFFFF;
        batch_word.store((batch_epoch << 40) | (uint64_t(chunks) << 20), std::memory_order_seq_cst);
        if (idle_sleepers.load(std::memory_order_seq_cst) > 0 && spinners.load(std::memory_order_seq_cst) == 0 &&
            !wake_pending.exchange(true, std::memory_order_seq_cst)) {
            { std::lock_guard<PoolMutex> lock(mutex); }
            wake.notify_all();
        }
        RunBatchChunks(false);
        if (batch_left.load(std::memory_order_acquire) == 0) return;

        const auto t0 = std::chrono::steady_clock::now();
        if (join_spin_ns > 0) {
            const auto deadline = t0 + std::chrono::nanoseconds(join_spin_ns);
            for (;;) {
                for (int k = 0; k < 64; ++k) {
                    if (batch_left.load(std::memory_order_acquire) == 0) return;
                    CpuRelax();
                }
                if (std::chrono::steady_clock::now() >= deadline) break;
            }
        }
        std::unique_lock<PoolMutex> lock(mutex);
        batch_waiter.store(true, std::memory_order_seq_cst);
        while (batch_left.load(std::memory_order_seq_cst) != 0) joined.wait(lock);
        batch_waiter.store(false, std::memory_order_seq_cst);
    }

    void RunCell(BuildCell* c, size_t at) {
        WakeForBacklog();
        DeferredBuild& build = *c->build;
        const int saved = std::fegetround();
        if (saved != c->rounding) std::fesetround(c->rounding);
        {
            c->job();
        }
        if (saved != c->rounding) std::fesetround(saved);
        c->job.Reset();
        c->seq.store(at + kCells, std::memory_order_release);
        Finish(build);
    }
    void RunInline(DeferredBuild& build, int rounding, BuildJob& job) {
        const int saved = std::fegetround();
        if (saved != rounding) std::fesetround(rounding);
        {
            job();
        }
        if (saved != rounding) std::fesetround(saved);
        job.Reset();
        Finish(build);
    }

    void Finish(DeferredBuild& build) {
        build.done.store(true, std::memory_order_seq_cst);
        bool signal = build.waited.load(std::memory_order_seq_cst);
        if (outstanding.fetch_sub(1, std::memory_order_seq_cst) == 1 && drainers.load(std::memory_order_seq_cst) > 0) signal = true;
        if (signal) {
            { std::lock_guard<PoolMutex> lock(mutex); }
            finished.notify_all();
        }
    }

    bool SpinForWork() {
        if (spin_ns <= 0) return false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(spin_ns);
        spinners.fetch_add(1, std::memory_order_seq_cst);
        bool found = false;
        while (!found) {
            for (int k = 0; k < 64 && !found; ++k) {
                found = HasWork() || BatchOpen() || stop.load(std::memory_order_relaxed);
                if (!found) CpuRelax();
            }
            if (found || std::chrono::steady_clock::now() >= deadline) break;
            std::this_thread::yield();
        }

        spinners.fetch_sub(1, std::memory_order_seq_cst);
        return found;
    }
    void Run(int index) {
        char name[24];
        std::snprintf(name, sizeof name, "te-build-%d", index);
        ps2r::platform::host::PinCurrentThreadInteractive(name, ps2r::platform::host::ThreadRole::Worker, index);
        for (;;) {
            if (stop.load(std::memory_order_acquire)) return;
            if (RunBatchChunks(true)) continue;
            size_t at;
            if (BuildCell* c = TryPop(at)) {
                RunCell(c, at);
                continue;
            }
            if (SpinForWork()) continue;
            std::unique_lock<PoolMutex> lock(mutex);
            idle_sleepers.fetch_add(1, std::memory_order_seq_cst);

            wake_pending.store(false, std::memory_order_seq_cst);
            while (!stop.load(std::memory_order_seq_cst) && !HasWork() && !BatchOpen()) {
                wake.wait(lock);
                wake_pending.store(false, std::memory_order_seq_cst);
            }
            idle_sleepers.fetch_sub(1, std::memory_order_seq_cst);
        }
    }
    void Post(DeferredBuild& build, int rounding, BuildJob&& job) {
        outstanding.fetch_add(1, std::memory_order_seq_cst);
        if (!TryPush(build, rounding, job)) {

            RunInline(build, rounding, job);
            return;
        }

        if (idle_sleepers.load(std::memory_order_seq_cst) > 0 && spinners.load(std::memory_order_seq_cst) == 0 &&
            !wake_pending.exchange(true, std::memory_order_seq_cst)) {
            { std::lock_guard<PoolMutex> lock(mutex); }
            wake.notify_one();
        }
    }

    template <typename Ready, typename Arm, typename Disarm>
    void HelpUntil(Ready ready, Arm arm, Disarm disarm, bool help) {
        if (ready()) return;
        while (!ready()) {
            size_t at;
            if (help) {
                if (BuildCell* c = TryPop(at)) {
                    RunCell(c, at);
                    continue;
                }
            }
            std::unique_lock<PoolMutex> lock(mutex);
            arm();
            while (!ready() && !(help && HasWork())) finished.wait(lock);
            disarm();
        }
    }
    void Wait(DeferredBuild& build) {
        if (build.done.load(std::memory_order_acquire)) return;
        const auto ready = [&] { return build.done.load(std::memory_order_seq_cst); };
        const auto arm = [&] { build.waited.store(true, std::memory_order_seq_cst); };
        const auto disarm = [] {};

        const bool render = OnRenderThread();
        HelpUntil(ready, arm, disarm, !render);
    }
    void Drain() {
        if (threads.empty()) return;
        HelpUntil([&] { return outstanding.load(std::memory_order_seq_cst) == 0; },
                  [&] { drainers.fetch_add(1, std::memory_order_seq_cst); },
                  [&] { drainers.fetch_sub(1, std::memory_order_seq_cst); }, !OnRenderThread());
    }
    ~BuildPool() {
        stop.store(true, std::memory_order_seq_cst);
        { std::lock_guard<PoolMutex> lock(mutex); }
        wake.notify_all();
        for (auto& t : threads) t.join();
    }
};
BuildPool& Builds() {
    static BuildPool pool;
    if (!pool.started) pool.Start();
    return pool;
}
void StartBuilds() { Builds(); }

void PrimeThreadSync() {
    const auto prime = [](auto& m, auto& a, auto& b) {
        std::unique_lock<std::remove_reference_t<decltype(m)>> lock(m);
        a.wait_for(lock, std::chrono::milliseconds(0));
        b.wait_for(lock, std::chrono::milliseconds(0));
        lock.unlock();
        a.notify_all();
        b.notify_all();
    };
    BuildPool& pool = Builds();
    prime(pool.mutex, pool.wake, pool.finished);
    prime(pool.mutex, pool.joined, pool.finished);
    prime(g.rt.mutex, g.rt.wake, g.rt.done);
}

bool DeferringBuilds() { return !Builds().threads.empty(); }

DeferredBuild& ReserveDeferred(uint32_t bucket, const ps2e_draw_state& state) {
    auto& b = g.buckets[bucket < TE_GFX_BUCKET_COUNT ? bucket : TE_GFX_BUCKET_UI];
    if (b.used == b.draws.size()) b.draws.emplace_back();
    Draw& d = b.draws[b.used++];
    PrefetchNextDraw(b);
    d.state = state;
    d.verts.clear();
    d.copy_frame = false;
    d.mesh = 0;
    d.bones.clear();
    d.skin_disp = 0;
    d.tag = g.tag;
    d.has_transform = g.tag_has_transform;
    if (g.tag_has_transform) d.transform = g.tag_transform;
    if (g.deferred_used == g.deferred.size()) g.deferred.push_back(std::make_unique<DeferredBuild>());
    DeferredBuild& build = *g.deferred[g.deferred_used++];
    build.r3_picture = (g.rt.started ? g.pictures_handed : g.pictures.load(std::memory_order_relaxed)) + 1;
    build.r3_ordinal = g.deferred_used - 1;
    build.state = state;
    build.verts.clear();
    build.segments.clear();
    build.alternate.clear();
    build.done.store(false, std::memory_order_relaxed);
    build.waited.store(false, std::memory_order_relaxed);
    d.pending = &build;
    b.has_pending = true;
    return build;
}

bool ThirtyShows(uint64_t b, uint64_t ordinal) {
    (void)ordinal;
    return !g.shown30_any || b < g.shown30_boundary || b - g.shown30_boundary >= 2;
}

bool DroppedPicture() {
    if (!g.rt.started) return false;
    if (g.frame_rate.load(std::memory_order_relaxed) != int(Rate::Rate30)) return false;

    return !ThirtyShows(te::Services::Get().clock().BoundaryCount(), g.pictures_handed + 1);
}

bool SkipBuild(DeferredBuild& build) {
    if (DroppedPicture()) {
        g.picture_thin = true;
        build.done.store(true, std::memory_order_release);
        return true;
    }
    (void)build;
    return false;
}

void PostBuildJob(DeferredBuild& build, BuildJob&& job) {

    Builds().Post(build, std::fegetround(), std::move(job));
}

void DrainBuilds() { Builds().Drain(); }

constexpr size_t kBlendBatchVertices = 8190;
bool Joins(const Gfx::Bucket& out, const ps2e_draw_state& state, bool copy_frame, const ps2r::ps2e::InterpTag& tag, bool interp,
           bool blend_path_safe, size_t append_vertices) {
    if (!out.used || !BatchableState(state) || !blend_path_safe || copy_frame) return false;
    const Draw& last = out.draws[out.used - 1];
    if (last.mesh || last.pending || last.verts.size() < 3 || !SameState(last.state, state)) return false;
    if (state.blend && (last.verts.size() % 3 || append_vertices % 3 || last.verts.size() > kBlendBatchVertices ||
                        append_vertices > kBlendBatchVertices - last.verts.size())) return false;
    return !interp || (last.tag.id == tag.id && last.tag.kind == tag.kind &&
                       (tag.kind != ps2r::ps2e::InterpKind::Vertices || last.tag.part == tag.part));
}
Draw& NextDraw(Gfx::Bucket& out) {
    if (out.used == out.draws.size()) out.draws.emplace_back();
    Draw& d = out.draws[out.used++];
    PrefetchNextDraw(out);
    return d;
}

bool BatchFiniteQ(const ps2e_draw_vertex* vertices, size_t count) {

    for (size_t i = 0; i < count; ++i) if (!std::isfinite(vertices[i].q)) return false;
    return true;
}

void ExpandDeferred(Gfx::Bucket* buckets, std::vector<std::unique_ptr<DeferredBuild>>& deferred, size_t& used) {
    if (!used) return;
    Gfx::Bucket* scratch = ExpandScratch();
    for (int b = 0; b < TE_GFX_BUCKET_COUNT; ++b) ExpandBucket(buckets, b, scratch);
    for (size_t i = 0; i < used; ++i) Builds().Wait(*deferred[i]);
    used = 0;
}

Gfx::Bucket* ExpandScratch() {
    thread_local Gfx::Bucket scratch[TE_GFX_BUCKET_COUNT];
    return scratch;
}

void ExpandDeferredShared(Gfx::Bucket* buckets, std::vector<std::unique_ptr<DeferredBuild>>& deferred, size_t& used) {
    BuildPool& pool = Builds();
    if (!used || pool.threads.empty() || !OnRenderThread()) {
        ExpandDeferred(buckets, deferred, used);
        return;
    }
    struct Ctx { Gfx::Bucket* buckets; Gfx::Bucket* scratch; } ctx{buckets, ExpandScratch()};
    pool.RunBatch(TE_GFX_BUCKET_COUNT, [](void* c, size_t b) {
        const Ctx& x = *static_cast<const Ctx*>(c);
        ExpandBucket(x.buckets, int(b), x.scratch);
    }, &ctx);
    for (size_t i = 0; i < used; ++i) pool.Wait(*deferred[i]);
    used = 0;
}

void ExpandBucket(Gfx::Bucket* buckets, int b, Gfx::Bucket* scratch) {
    const bool interp = g.interp_active.load(std::memory_order_relaxed);

    {
        Gfx::Bucket& in = buckets[b];
        Gfx::Bucket& out = scratch[b];
        if (!in.has_pending) return;
        bool any = false;
        for (size_t i = 0; i < in.used && !any; ++i) any = in.draws[i].pending != nullptr;
        if (!any) {
            in.has_pending = false;
            return;
        }
        out.used = 0;
        bool last_blend_safe = false;
        for (size_t i = 0; i < in.used; ++i) {
            Draw& d = in.draws[i];
            if (i + 2 < in.used) PrefetchDraw(in.draws[i + 2]);
            if (!d.pending) {
                const bool blend_safe = BatchableState(d.state) && d.verts.size() >= 3 && d.verts.size() % 3 == 0 && d.verts.size() <= kBlendBatchVertices &&
                                        BatchFiniteQ(d.verts.data(), d.verts.size());
                if (!d.mesh && Joins(out, d.state, d.copy_frame, d.tag, interp, last_blend_safe && blend_safe, d.verts.size())) {
                    Draw& last = out.draws[out.used - 1];
                    last.verts.insert(last.verts.end(), d.verts.begin(), d.verts.end());
                } else {

                    Draw& o = NextDraw(out);
                    o.verts.swap(d.verts);
                    o.state = d.state;
                    o.copy_frame = d.copy_frame;
                    o.pending = nullptr;
                    o.tag = d.tag;
                    o.has_transform = d.has_transform;
                    if (d.has_transform) o.transform = d.transform;
                    o.mesh = d.mesh;
                    if (d.mesh) { o.mesh_params = d.mesh_params; o.mesh_box = d.mesh_box; o.mesh_above_far = d.mesh_above_far; }
                    o.bones.swap(d.bones);
                    o.skin_disp = d.skin_disp;
                    last_blend_safe = blend_safe;
                }
                continue;
            }
            DeferredBuild& build = *d.pending;
            Builds().Wait(build);

            const bool build_blend_safe = BatchableState(build.state) &&
                                          build.verts.size() >= 3 && build.verts.size() % 3 == 0 &&
                                          build.verts.size() <= kBlendBatchVertices &&
                                          std::all_of(build.segments.begin(), build.segments.end(),
                                                      [](uint32_t n) { return n % 3 == 0; }) &&
                                          BatchFiniteQ(build.verts.data(), build.verts.size());
            if (!build.segments.empty() && build.alternate.empty() && BatchableState(build.state) &&
                build_blend_safe) {
                if (Joins(out, build.state, false, d.tag, interp, last_blend_safe && build_blend_safe, build.verts.size())) {
                    Draw& last = out.draws[out.used - 1];
                    last.verts.insert(last.verts.end(), build.verts.begin(), build.verts.end());
                } else {
                    Draw& o = NextDraw(out);
                    o.state = build.state;
                    o.verts.swap(build.verts);
                    o.copy_frame = false;
                    o.pending = nullptr;
                    o.mesh = 0;
                    o.bones.clear();
                    o.skin_disp = 0;
                    o.tag = d.tag;
                    o.has_transform = d.has_transform;
                    if (d.has_transform) o.transform = d.transform;
                    last_blend_safe = build_blend_safe;
                }
                d.pending = nullptr;
                continue;
            }
            size_t at = 0;
            for (size_t k = 0; k < build.segments.size(); ++k) {
                const uint32_t count = build.segments[k];
                const ps2e_draw_state& state = k < build.alternate.size() && build.alternate[k] ? build.alt_state : build.state;
                const auto first = build.verts.begin() + std::ptrdiff_t(at), end = first + std::ptrdiff_t(count);
                at += count;
                const bool blend_safe = BatchableState(state) && count >= 3 && count % 3 == 0 && count <= kBlendBatchVertices &&
                                        BatchFiniteQ(build.verts.data() + at - count, count);
                if (Joins(out, state, false, d.tag, interp, last_blend_safe && blend_safe, count)) {
                    Draw& last = out.draws[out.used - 1];
                    last.verts.insert(last.verts.end(), first, end);
                } else {
                    Draw& o = NextDraw(out);
                    o.state = state;

                    if (build.segments.size() == 1 && count == build.verts.size()) {
                        o.verts.swap(build.verts);
                    } else {
                        o.verts.assign(first, end);
                    }
                    o.copy_frame = false;
                    o.pending = nullptr;
                    o.mesh = 0;
                    o.bones.clear();
                    o.skin_disp = 0;
                    o.tag = d.tag;
                    o.has_transform = d.has_transform;
                    if (d.has_transform) o.transform = d.transform;
                    last_blend_safe = blend_safe;
                }
            }
            d.pending = nullptr;
        }
        std::swap(in.draws, out.draws);
        std::swap(in.used, out.used);

        in.has_pending = out.has_pending = false;
    }
}

void ClampWholeTexture(ps2e_draw_state& st, float u0, float v0, float u1, float v1) {
    if (std::min(u0, u1) == 0.f && std::max(u0, u1) == 1.f) st.wrap_u = PS2E_DRAW_WRAP_CLAMP;
    if (std::min(v0, v1) == 0.f && std::max(v0, v1) == 1.f) st.wrap_v = PS2E_DRAW_WRAP_CLAMP;
}

void RecordQuad(uint32_t bucket, const ps2e_draw_state& st, float gx0, float gy0, float gx1, float gy1, float u0, float v0,
                float u1, float v1, uint32_t colour) {
    const float ox = GsOriginX(), oy = GsOriginY();
    const float x0 = gx0 - ox, y0 = gy0 - oy, x1 = gx1 - ox, y1 = gy1 - oy;
    Draw d;
    d.state = st;
    ClampWholeTexture(d.state, u0, v0, u1, v1);
    d.verts = {Vertex(x0, y0, u0, v0, colour), Vertex(x1, y0, u1, v0, colour), Vertex(x0, y1, u0, v1, colour),
               Vertex(x1, y0, u1, v0, colour), Vertex(x1, y1, u1, v1, colour), Vertex(x0, y1, u0, v1, colour)};
    ::Record(bucket, std::move(d));
}

void PumpWindow() {
    const bool closed = g.dev && ps2e_draw_pump(g.dev);
    if (closed) {
        te::RequestExit(0);
    }
}

void VBlank() {
    PumpWindow();
    te::Services::Get().clock().Boundary();
}

template <typename Vertices>
void TrimStorage(Vertices& verts, size_t keep) {
    if (verts.capacity() <= keep * 2 + 64) return;
    Vertices fresh;
    fresh.reserve(keep);
    verts.swap(fresh);
}

void ClearBuckets() {
    if (te::gfx::RecordTask(&ClearBuckets)) return;

    for (size_t i = 0; i < g.deferred_used; ++i) Builds().Wait(*g.deferred[i]);
    g.picture_thin = false;
    constexpr size_t kSpareDraws = 64;
    for (auto& b : g.buckets) {
        if (b.draws.size() > b.used + kSpareDraws) b.draws.resize(b.used + kSpareDraws);
        for (size_t i = 0; i < b.draws.size(); ++i) {
            if (i + 4 < b.draws.size()) __builtin_prefetch(&b.draws[i + 4].verts, 0, 3);
            TrimStorage(b.draws[i].verts, i < b.used ? b.draws[i].verts.size() : 0);
        }
        b.used = 0;
        b.has_pending = false;
    }

    const size_t kept = std::max(g.deferred_used, g.deferred_recent);
    for (size_t i = 0; i < g.deferred.size(); ++i) {
        if (i + 4 < g.deferred.size()) __builtin_prefetch(g.deferred[i + 4].get(), 0, 3);
        DeferredBuild& build = *g.deferred[i];
        const bool held = i < kept;
        TrimStorage(build.verts, held ? build.verts.size() : 0);
        TrimStorage(build.segments, held ? build.segments.size() : 0);
    }
    g.deferred_used = 0;
}

namespace {
bool ReadGameState(unsigned* state) {
    const uint8_t* mem = te::GuestMemory();
    if (!mem) return false;
    constexpr uint32_t kGameControllerPointer = 0x00309888;
    const uint32_t gc = uint32_t(mem[kGameControllerPointer]) | uint32_t(mem[kGameControllerPointer + 1]) << 8 |
                        uint32_t(mem[kGameControllerPointer + 2]) << 16 | uint32_t(mem[kGameControllerPointer + 3]) << 24;
    if (gc == 0 || uint64_t(gc) + 16 > te::GuestMemorySize()) return false;
    uint64_t v = 0;
    std::memcpy(&v, mem + gc + 8, 8);
    *state = unsigned((v >> 44) & 63);
    return true;
}
}

void BeginFrameTarget(int screen_w, int screen_h) {
    int fw = 0, fh = 0;
    ps2e_draw_frame_size(g.dev, &fw, &fh);
    if (g.have_frame && (fw != screen_w || fh != screen_h)) ps2e_draw_resize_frame(g.dev, screen_w, screen_h);
    ps2e_draw_begin_frame(g.dev, nullptr);
}

void BuildInterpItems(Gfx::Frame& f) {
    if (!f.items.empty()) return;
    for (int b = 0; b < TE_GFX_BUCKET_COUNT; ++b) {
        for (size_t i = 0; i < f.buckets[b].used; ++i) {
            const Draw& d = f.buckets[b].draws[i];
            ps2r::ps2e::InterpItem it;
            it.state = &d.state;
            it.verts = d.verts.data();
            it.count = uint32_t(d.verts.size());
            it.copy_frame = d.copy_frame;
            it.tag = d.tag;
            it.transform = d.has_transform ? &d.transform : nullptr;
            it.mesh = d.mesh;
            it.mesh_params = d.mesh ? &d.mesh_params : nullptr;
            it.bones = d.bones.empty() ? nullptr : d.bones.data();
            it.bone_vec4 = uint32_t(d.bones.size() / 4) - d.skin_disp;
            it.disp_vec4 = d.skin_disp;
            f.items.push_back(it);
        }
    }
}


bool FullFrameDepthClear(const Draw& d) {
    const ps2e_draw_state& st = d.state;
    if (!st.depth_write || st.texture || st.alpha_test || d.verts.size() != 6 || st.scissor_x0 || st.scissor_y0 ||
        st.scissor_x1 || st.scissor_y1)
        return false;
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (const auto& v : d.verts) {
        if (v.z != 0.0f) return false;
        x0 = std::min(x0, v.x); y0 = std::min(y0, v.y); x1 = std::max(x1, v.x); y1 = std::max(y1, v.y);
    }
    if (!(x0 <= 0.0f && y0 <= 0.0f && x1 >= float(g.display_w) && y1 >= float(g.display_h))) return false;
    int missing[2];
    for (int t = 0; t < 2; ++t) {
        int seen = 0;
        for (int k = 0; k < 3; ++k) {
            const auto& v = d.verts[size_t(t * 3 + k)];
            const bool left = v.x == x0, right = v.x == x1, top = v.y == y0, bottom = v.y == y1;
            if (!(left || right) || !(top || bottom)) return false;
            seen |= 1 << ((right ? 1 : 0) | (bottom ? 2 : 0));
        }
        if (__builtin_popcount(unsigned(seen)) != 3) return false;
        missing[t] = __builtin_ctz(unsigned(~seen & 15));
    }
    return (missing[0] ^ missing[1]) == 3;
}

bool MeshAboveFarPlane(const ps2e_draw_mesh_params& params, const std::array<float, 6>& b) {
    const float* m = params.screen;
    double zx[2], zy[2], zz[2], wx[2], wy[2], wz[2];
    for (int k = 0; k < 2; ++k) {
        const double px = b[k ? 3 : 0], py = b[k ? 4 : 1], pz = b[k ? 5 : 2];
        zx[k] = double(m[2]) * px; zy[k] = double(m[6]) * py; zz[k] = double(m[10]) * pz;
        wx[k] = double(m[3]) * px; wy[k] = double(m[7]) * py; wz[k] = double(m[11]) * pz;
    }
    for (int c = 0; c < 8; ++c) {
        const int i = c & 1, j = (c >> 1) & 1, k = (c >> 2) & 1;
        const double z = double(m[14]) + zx[i] + zy[j] + zz[k];
        const double w = double(m[15]) + wx[i] + wy[j] + wz[k];
        if (!(w > 0.0) || !(z >= 1024.0 * w)) return false;
    }
    return true;
}

bool AboveFarPlane(const Draw& d) {
    if (d.mesh) return d.mesh_above_far;
    for (const auto& v : d.verts)
        if (!(v.z >= 512.0f)) return false;
    return true;
}


bool SkyLateEligible(Gfx::Bucket* buckets, size_t* split) {
    bool clear = false;
    for (int b = 0; b <= 1; ++b)
        for (size_t i = 0; i < buckets[b].used; ++i) {
            const Draw& d = buckets[b].draws[i];
            const ps2e_draw_state& st = d.state;
            if (d.pending) return false;
            if (d.mesh) return false;
            if (d.copy_frame) return false;
            if (st.stencil != PS2E_DRAW_STENCIL_OFF) return false;
            if (st.depth_test != PS2E_DRAW_DEPTH_ALWAYS) return false;
            if (!st.no_colour_write) {
                if (st.depth_write) return false;
                continue;
            }
            if (FullFrameDepthClear(d)) clear = true;
            else return false;
        }
    if (!clear) return false;

    *split = buckets[2].used;
    for (size_t i = 0; i < buckets[2].used; ++i) {
        const Draw& d = buckets[2].draws[i];
        const ps2e_draw_state& st = d.state;
        if (d.pending) return false;
        if (d.copy_frame || st.blend || !st.depth_write || st.no_colour_write || st.stencil != PS2E_DRAW_STENCIL_OFF ||
            (st.alpha_test && st.alpha_fail != 0) || !AboveFarPlane(d)) {
            *split = i;
            break;
        }
    }
    return true;
}

bool SkyReplacedBy(const Draw& a, const Draw& b) {
    const auto& x = a.state;
    const auto& y = b.state;
    if (a.pending || b.pending || a.mesh || b.mesh || a.copy_frame || b.copy_frame ||
        x.no_colour_write || y.no_colour_write || x.depth_write || y.depth_write ||
        x.depth_test != PS2E_DRAW_DEPTH_ALWAYS || y.depth_test != PS2E_DRAW_DEPTH_ALWAYS ||
        x.stencil || y.stencil || y.blend || y.alpha_test || x.dest_alpha_test || y.dest_alpha_test ||
        x.texture == PS2E_DRAW_TEXTURE_FRAME_COPY || y.texture == PS2E_DRAW_TEXTURE_FRAME_COPY ||
        x.scissor_x0 != y.scissor_x0 || x.scissor_x1 != y.scissor_x1 ||
        x.scissor_y0 != y.scissor_y0 || x.scissor_y1 != y.scissor_y1 ||
        a.verts.size() < 3 || a.verts.size() % 3 || a.verts.size() > 1536 || a.verts.size() != b.verts.size()) return false;

    uint64_t census[3];
    if (y.texture && !ps2e_draw_texture_census(g.dev, y.texture, census)) return false;
    bool ordered = true;
    for (size_t i = 0; i < a.verts.size(); ++i) {
        const auto& u = a.verts[i];
        const auto& v = b.verts[i];
        if (!std::isfinite(u.x) || !std::isfinite(u.y) || !std::isfinite(v.x) || !std::isfinite(v.y) ||
            !(u.z >= 0.f && u.z <= 16777215.f) || !(v.z >= 0.f && v.z <= 16777215.f) ||
            !std::isfinite(u.q) || !std::isfinite(v.q)) return false;
        ordered = ordered && u.x == v.x && u.y == v.y;
    }
    if (ordered) return true;

    using Triangle = std::array<float, 6>;
    thread_local std::vector<Triangle> ca, cb;
    const auto canonical = [](const Draw& d, std::vector<Triangle>& out) {
        out.resize(d.verts.size() / 3);
        for (size_t t = 0; t < out.size(); ++t) {
            std::array<float, 2> xy[3];
            for (size_t k = 0; k < 3; ++k) xy[k] = {d.verts[t * 3 + k].x, d.verts[t * 3 + k].y};
            std::sort(xy, xy + 3);
            for (size_t k = 0; k < 3; ++k) {
                out[t][k * 2] = xy[k][0]; out[t][k * 2 + 1] = xy[k][1];
            }
        }
        std::sort(out.begin(), out.end());
    };
    canonical(a, ca); canonical(b, cb);
    return ca == cb;
}

bool SkyMayJoin(const Draw& a, const Draw& b) {
    const auto& st = a.state;

    if ((st.fba || st.blend_a != 0 || (st.blend_b != 1 && st.blend_b != 2) ||
                        st.blend_c != 0 || st.blend_d != 1)) return false;
    if (a.pending || b.pending || a.mesh || b.mesh || a.copy_frame || b.copy_frame ||
        !st.blend || st.stencil || st.dest_alpha_test || st.no_colour_write ||
        st.depth_write || st.depth_test != PS2E_DRAW_DEPTH_ALWAYS || st.alpha_test ||
        !SameState(st, b.state) || st.texture == PS2E_DRAW_TEXTURE_FRAME_COPY || a.verts.empty() || b.verts.empty() || a.verts.size() % 3 || b.verts.size() % 3) return false;
    uint64_t census[3];
    if (st.texture && !ps2e_draw_texture_census(g.dev, st.texture, census)) return false;
    for (const Draw* d : {&a, &b})
        for (const auto& v : d->verts)
            if (!std::isfinite(v.q)) return false;
    return true;
}

constexpr size_t kBlendJoinVertices = 4095;
bool BlendJoinEligible(const Draw& d) {
    if ((d.state.fba || d.state.blend_a != 0 || (d.state.blend_b != 1 && d.state.blend_b != 2) ||
                        d.state.blend_c != 0 || d.state.blend_d != 1)) return false;
    return d.state.blend && !d.copy_frame && !d.pending && !d.mesh &&
           d.state.texture != PS2E_DRAW_TEXTURE_FRAME_COPY && !d.state.dest_alpha_test &&
           d.state.stencil == PS2E_DRAW_STENCIL_OFF && d.verts.size() >= 3 &&
           d.verts.size() <= kBlendJoinVertices && d.verts.size() % 3 == 0 && BatchFiniteQ(d.verts.data(), d.verts.size());
}

bool BlendOutside(const Draw& d, int width, int height) {
    if (!d.state.blend || d.copy_frame || d.pending || d.mesh || d.state.dest_alpha_test ||
        d.state.texture == PS2E_DRAW_TEXTURE_FRAME_COPY || d.state.stencil != PS2E_DRAW_STENCIL_OFF ||
        d.verts.size() < 3 || d.verts.size() % 3) return false;
    bool left = true, right = true, top = true, bottom = true;
    for (const auto& v : d.verts) {
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z) || !std::isfinite(v.q)) return false;
        left &= v.x < -1.f;
        right &= v.x > float(width) + 1.f;
        top &= v.y < -1.f;
        bottom &= v.y > float(height) + 1.f;
    }
    return left || right || top || bottom;
}
bool BeachClip(const Draw& d, const ps2e_draw_vertex* vertices, size_t count, int width, int height,
               std::vector<ps2e_draw_vertex>& clipped) {
    const auto& st = d.state;
    if (d.pending || d.mesh || d.copy_frame || st.no_colour_write || st.stencil || st.dest_alpha_test ||
        st.texture == PS2E_DRAW_TEXTURE_FRAME_COPY || count < 3 || count % 3) return false;
    for (size_t i = 0; i < count; ++i) {
        const auto& v = vertices[i];
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z) || !std::isfinite(v.q)) return false;
    }
    bool changed = false;
    clipped.clear();
    for (size_t at = 0; at < count; at += 3) {
        bool left = true, right = true, top = true, bottom = true;
        for (size_t k = at; k < at + 3; ++k) {
            const auto& v = vertices[k];
            left &= v.x < -1.f; right &= v.x > float(width) + 1.f;
            top &= v.y < -1.f; bottom &= v.y > float(height) + 1.f;
        }
        if (left || right || top || bottom) {
            if (!changed) {

                try { clipped.reserve(count); }
                catch (const std::bad_alloc&) { return false; }
                clipped.insert(clipped.end(), vertices, vertices + at);
                changed = true;
            }
        } else if (changed) clipped.insert(clipped.end(), vertices + at, vertices + at + 3);
    }
    return changed;
}

struct ShadowBounds {
    bool valid = false;
    size_t composite = 0, verts = 0;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
ShadowBounds FindShadowBounds(const Gfx::Bucket& bucket, size_t first, int w, int h) {
    ShadowBounds out;
    if (w <= 0 || h <= 0) return out;
    float x0 = float(w), y0 = float(h), x1 = 0.f, y1 = 0.f;
    for (size_t i = first; i < bucket.used; ++i) {
        const Draw& d = bucket.draws[i];
        const auto& st = d.state;
        if (st.stencil == PS2E_DRAW_STENCIL_INSIDE) {
            if (!out.verts || st.scissor_x0 || st.scissor_y0 || st.scissor_x1 || st.scissor_y1) return out;

            for (size_t j = i + 1; j < bucket.used; ++j) {
                const int mode = bucket.draws[j].state.stencil;
                if (mode == PS2E_DRAW_STENCIL_CLEAR) break;
                if (mode == PS2E_DRAW_STENCIL_COUNT_UP || mode == PS2E_DRAW_STENCIL_COUNT_DOWN) return ShadowBounds{};
            }
            out.x0 = int(std::floor(std::clamp(x0 - 1.f, 0.f, float(w))));
            out.y0 = int(std::floor(std::clamp(y0 - 1.f, 0.f, float(h))));
            out.x1 = int(std::ceil(std::clamp(x1 + 1.f, 0.f, float(w))));
            out.y1 = int(std::ceil(std::clamp(y1 + 1.f, 0.f, float(h))));
            out.valid = out.x1 > out.x0 && out.y1 > out.y0;
            out.composite = i;
            return out;
        }
        if ((st.stencil != PS2E_DRAW_STENCIL_COUNT_UP && st.stencil != PS2E_DRAW_STENCIL_COUNT_DOWN) ||
            d.mesh || d.pending || d.copy_frame || !st.no_colour_write || st.depth_write) return out;
        const size_t n = d.verts.size() / 3 * 3;
        out.verts += n;
        for (size_t v = 0; v < n; ++v) {
            const float x = d.verts[v].x, y = d.verts[v].y;
            if (!std::isfinite(x) || !std::isfinite(y)) return ShadowBounds{};
            x0 = std::min(x0, x); y0 = std::min(y0, y);
            x1 = std::max(x1, x); y1 = std::max(y1, y);
        }
    }
    return out;
}

constexpr size_t kShadowBatchVertices = 4095;
bool ShadowBatchEligible(const Draw& d) {
    const auto& st = d.state;
    if (d.mesh || d.pending || d.copy_frame || !st.no_colour_write || st.depth_write || st.texture ||
        st.blend || st.alpha_test || st.depth_test != PS2E_DRAW_DEPTH_GEQUAL ||
        (st.stencil != PS2E_DRAW_STENCIL_COUNT_UP && st.stencil != PS2E_DRAW_STENCIL_COUNT_DOWN) ||
        d.verts.size() < 3 || d.verts.size() % 3 || d.verts.size() > kShadowBatchVertices) return false;
    for (const auto& v : d.verts)
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z) || !std::isfinite(v.q)) return false;
    return true;
}
bool ShadowSameCounterState(const ps2e_draw_state& a, const ps2e_draw_state& b) {
    ps2e_draw_state same_sign = b;
    same_sign.stencil = a.stencil;
    return SameState(a, same_sign);
}
void SelectBeforeSky(const Gfx::Bucket& bucket, size_t split, int w, int h, std::vector<uint8_t>& selected) {
    size_t draws = 0, vertices = 0;
    selected.clear();
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024 || split >= bucket.used) return;
    selected.assign(std::min<size_t>(bucket.used - split, 256), 0);
    std::array<uint64_t, 4> blocked{};
    for (size_t i = split; i < bucket.used && draws < 256; ++i) {
        const Draw& d = bucket.draws[i]; const auto& st = d.state;
        if (d.pending || d.mesh || d.copy_frame || st.texture == PS2E_DRAW_TEXTURE_FRAME_COPY ||
            st.stencil || st.dest_alpha_test || d.verts.size() > 32768 - vertices) break;
        ++draws;
        vertices += d.verts.size();
        if (d.verts.size() < 3) continue;
        float x0 = 16385.f, y0 = 16385.f, x1 = -16385.f, y1 = -16385.f;
        bool finite = true, positive = true;
        for (const auto& v : d.verts) {
            finite = finite && std::isfinite(v.q) && std::isfinite(v.x) && std::isfinite(v.y) && std::abs(v.x) <= 16384.f && std::abs(v.y) <= 16384.f;
            positive = positive && v.z >= 512.f && v.z <= 16777215.f;
            x0 = std::min(x0, v.x); y0 = std::min(y0, v.y); x1 = std::max(x1, v.x); y1 = std::max(y1, v.y);
        }
        if (!finite) break;

        x0 -= 1.f; y0 -= 1.f; x1 += 1.f; y1 += 1.f;
        std::array<uint64_t, 4> mask{};
        if (x1 >= 0.f && y1 >= 0.f && x0 <= w && y0 <= h) {
            const int tx0 = std::clamp(int(std::floor(x0 * 16.f / w)), 0, 15);
            const int ty0 = std::clamp(int(std::floor(y0 * 16.f / h)), 0, 15);
            const int tx1 = std::clamp(int(std::floor(x1 * 16.f / w)), 0, 15);
            const int ty1 = std::clamp(int(std::floor(y1 * 16.f / h)), 0, 15);
            const uint64_t span = (uint64_t(1) << (tx1 - tx0 + 1)) - 1;
            for (int y = ty0; y <= ty1; ++y) {
                mask[size_t(y / 4)] |= span << ((y % 4) * 16 + tx0);
            }
        }
        bool disjoint = true;
        for (size_t k = 0; k < 4; ++k) disjoint = disjoint && !(mask[k] & blocked[k]);

        bool opaque = !st.blend && st.depth_write && !st.no_colour_write && st.depth_test == PS2E_DRAW_DEPTH_GEQUAL &&
                      (!st.alpha_test || st.alpha_fail == 0) && positive && d.verts.size() % 3 == 0;
        uint64_t census[3];
        if (opaque && st.texture && !ps2e_draw_texture_census(g.dev, st.texture, census)) opaque = false;
        if (opaque && disjoint) selected[i - split] = 1;
        else for (size_t k = 0; k < 4; ++k) blocked[k] |= mask[k];
        if (std::all_of(blocked.begin(), blocked.end(), [](uint64_t v) { return v == ~uint64_t(0); })) break;
    }
}

#include "menu_backdrop.inc"
#include "menu_scene_skip.inc"

void SubmitBuckets(Gfx::Bucket* buckets, int screen_w, int screen_h, bool count_as_picture,
                   bool menu_backdrop_candidate = false, const SkipSnap* skip = nullptr) {
    const uint64_t dropped_before = g.dropped_draws.load();
    BeginFrameTarget(screen_w, screen_h);
    bool sky_late = false;
    size_t sky_split = 0;

    const bool scene_skipped = skip && skip->skipped;
    if (!scene_skipped) {
        for (int b = 0; b <= 2; ++b) ExpandBucket(buckets, b, ExpandScratch());
        sky_late = SkyLateEligible(buckets, &sky_split);
    }
    if (!g.backdrop) g.backdrop = std::make_shared<MenuBackdrop>();
    auto* backdrop = g.backdrop.get();
    const bool cache_on = count_as_picture && menu_backdrop_candidate && backdrop && !(skip && skip->skipped);
    bool backdrop_hit = false;
    if (scene_skipped) {
        const bool exact = backdrop && backdrop->SkipUsable(screen_w, screen_h);
        const bool resources_moved = !exact && backdrop && backdrop->valid && backdrop->have_key && backdrop->key_w == screen_w && backdrop->key_h == screen_h;
        backdrop_hit = (exact || resources_moved) && ps2e_draw_backdrop(g.dev, 1);
        if (backdrop_hit && exact) { if (count_as_picture) SceneSkipPublishSeq(skip->seq); }
        else if (backdrop_hit) {
            backdrop->valid = false; backdrop->have_key = false; backdrop->admit = false;
            if (count_as_picture) SceneSkipPublish(0, skip->seq);
        } else {
            if (backdrop) { backdrop->valid = false; backdrop->have_key = false; backdrop->admit = false; }
            if (count_as_picture) SceneSkipPublish(0, skip->seq);
        }
    }
    if (cache_on) {
        for (int b = 0; b < TE_GFX_BUCKET_UI; ++b) ExpandBucket(buckets, b, ExpandScratch());
        backdrop_hit = backdrop->Observe(buckets, screen_w, screen_h, sky_late, sky_split);
        if (skip) {
            bool ui_mesh = false;
            if (backdrop_hit)
                for (int b = TE_GFX_BUCKET_UI; b < TE_GFX_BUCKET_COUNT && !ui_mesh; ++b) {
                    ExpandBucket(buckets, b, ExpandScratch());
                    for (size_t i = 0; i < buckets[b].used; ++i)
                        if (buckets[b].draws[i].mesh || !buckets[b].draws[i].bones.empty()) { ui_mesh = true; break; }
                }
            if (backdrop_hit && !ui_mesh && skip->key) SceneSkipPublish(skip->key, skip->seq);
            else SceneSkipPublish(0, skip->seq);
        }
    } else if (skip && count_as_picture && !scene_skipped) {
        SceneSkipPublish(0, skip->seq);
    }

    thread_local std::vector<uint8_t> sky_replaced[2];
    for (int b = 0; b <= 1; ++b) {
        if (backdrop_hit) break;
        ExpandBucket(buckets, b, ExpandScratch());
        auto& mask = sky_replaced[b];
        mask.assign(buckets[b].used, 0);
        for (size_t i = 0; i + 1 < buckets[b].used; ++i)
            if (SkyReplacedBy(buckets[b].draws[i], buckets[b].draws[i + 1])) {
                mask[i] = 1;
            }
    }

    const auto submit_raw = [&](const Draw& source, const ps2e_draw_state& st,
                                const ps2e_draw_vertex* vertices, size_t count, int bucket) {
        thread_local std::vector<ps2e_draw_vertex> clipped;
        if ((bucket <= 1 || (bucket >= 5 && bucket <= 19)) &&
            BeachClip(source, vertices, count, screen_w, screen_h, clipped)) {
            vertices = clipped.data(); count = clipped.size();
            if (!count) return;
        }
        if (ps2e_draw_submit(g.dev, &st, vertices, int(count)) < 0) g.dropped_draws++;
    };
    thread_local std::vector<uint8_t> before_sky;
    if (sky_late && sky_split < buckets[2].used) SelectBeforeSky(buckets[2], sky_split, screen_w, screen_h, before_sky);
    else before_sky.clear();
    bool spatial_done = false;
    const auto submit_spatial = [&]() {
        if (spatial_done) return;
        spatial_done = true;
        for (size_t i = 0; i < before_sky.size(); ++i) if (before_sky[i]) {
            const Draw& d = buckets[2].draws[sky_split + i];
            if (ps2e_draw_submit(g.dev, &d.state, d.verts.data(), int(d.verts.size())) < 0) g.dropped_draws++;
        }
    };
    bool sky_done = false;
    const auto submit_sky = [&]() {

        if (sky_done) return;
        sky_done = true;
        thread_local std::vector<ps2e_draw_vertex> zero;
        for (int sb = 0; sb <= 1; ++sb)
            for (size_t i = 0; i < buckets[sb].used; ++i) {
                const Draw& d = buckets[sb].draws[i];
                if (d.state.no_colour_write || sky_replaced[sb][i]) continue;
                ps2e_draw_state st = d.state;
                st.depth_test = PS2E_DRAW_DEPTH_GEQUAL;
                st.depth_write = 0;
                zero.assign(d.verts.begin(), d.verts.end());
                while (i + 1 < buckets[sb].used && !sky_replaced[sb][i + 1] &&
                       zero.size() + buckets[sb].draws[i + 1].verts.size() <= 4096 &&
                       SkyMayJoin(buckets[sb].draws[i], buckets[sb].draws[i + 1])) {
                    ++i;
                    const auto& more = buckets[sb].draws[i].verts;
                    zero.insert(zero.end(), more.begin(), more.end());
                }
                for (auto& v : zero) v.z = 0.0f;
                submit_raw(d, st, zero.data(), zero.size(), sb);
            }
    };
    for (int b = 0; b < TE_GFX_BUCKET_COUNT; ++b) {
        ExpandBucket(buckets, b, ExpandScratch());
        if (backdrop_hit && b < TE_GFX_BUCKET_UI) continue;
        if (cache_on && !backdrop_hit && b == TE_GFX_BUCKET_UI) {
            if (g.dropped_draws.load() == dropped_before) backdrop->Save();
            else { backdrop->valid = false; backdrop->have_key = false; backdrop->admit = false; }
        }
        ShadowBounds shadow_bounds;
        if (sky_late && b == 3) submit_sky();
        for (size_t k = 0; k < 4 && k < buckets[b].used; ++k) PrefetchDraw(buckets[b].draws[k]);
        for (size_t i = 0; i < buckets[b].used; ++i) {
            const Draw& d = buckets[b].draws[i];

            if (i + 4 < buckets[b].used) PrefetchDraw(buckets[b].draws[i + 4]);
            if (i + 2 < buckets[b].used) {
                const Draw& ahead = buckets[b].draws[i + 2];
                ps2e_draw_prefetch(g.dev, ahead.state.texture, ahead.mesh);
            }
            const ps2e_draw_state* submit_state = &d.state;
            ps2e_draw_state bounded;
            if (d.state.stencil == PS2E_DRAW_STENCIL_CLEAR)
                shadow_bounds = FindShadowBounds(buckets[b], i + 1, screen_w, screen_h);
            if (shadow_bounds.valid &&
                (d.state.stencil == PS2E_DRAW_STENCIL_CLEAR || i == shadow_bounds.composite)) {
                bounded = d.state;
                bounded.scissor_x0 = shadow_bounds.x0; bounded.scissor_y0 = shadow_bounds.y0;
                bounded.scissor_x1 = shadow_bounds.x1 - 1; bounded.scissor_y1 = shadow_bounds.y1 - 1;
                submit_state = &bounded;
            }
            if (b <= 1 && sky_replaced[b][i]) continue;
            if (sky_late && b <= 1 && !d.state.no_colour_write) continue;
            if (sky_late && b == 2 && i == sky_split) { submit_spatial(); submit_sky(); }
            if (b == 2 && i >= sky_split && i - sky_split < before_sky.size() && before_sky[i - sky_split]) continue;

            if (d.copy_frame) {
                ps2e_draw_copy_frame(g.dev);
            }
            if (ShadowBatchEligible(d)) {
                size_t end = i + 1, count = d.verts.size();
                while (end < buckets[b].used && ShadowBatchEligible(buckets[b].draws[end]) &&
                       ShadowSameCounterState(d.state, buckets[b].draws[end].state) &&
                       count + buckets[b].draws[end].verts.size() <= kShadowBatchVertices) {
                    count += buckets[b].draws[end].verts.size();
                    ++end;
                }
                thread_local std::array<ps2e_draw_vertex, kShadowBatchVertices> signs[2];
                size_t used[2] = {};
                for (size_t j = i; j < end; ++j) {
                    const auto& src = buckets[b].draws[j];
                    const int sign = src.state.stencil == PS2E_DRAW_STENCIL_COUNT_UP ? 0 : 1;
                    for (size_t v = 0; v < src.verts.size(); v += 3) {
                        const auto* tri = src.verts.data() + v;
                        std::memcpy(signs[sign].data() + used[sign], tri, 3 * sizeof(*tri));
                        used[sign] += 3;
                    }
                }
                for (int sign = 0; sign < 2; ++sign) {
                    if (!used[sign]) continue;
                    ps2e_draw_state st = d.state;
                    st.stencil = sign == 0 ? PS2E_DRAW_STENCIL_COUNT_UP : PS2E_DRAW_STENCIL_COUNT_DOWN;
                    if (ps2e_draw_submit(g.dev, &st, signs[sign].data(), int(used[sign])) < 0) g.dropped_draws++;
                }
                i = end - 1;
                continue;
            }
            if (((b >= 5 && b <= 19) || b == 22) && BlendOutside(d, screen_w, screen_h)) continue;
            if (((b >= 5 && b <= 19) || b == 22 || b == TE_GFX_BUCKET_UI || b == TE_GFX_BUCKET_FONTS) && BlendJoinEligible(d)) {
                size_t end = i + 1, count = d.verts.size();
                while (end < buckets[b].used) {
                    const Draw& next = buckets[b].draws[end];
                    if (!BlendJoinEligible(next) || !SameState(d.state, next.state) ||
                        count + next.verts.size() > kBlendJoinVertices) break;
                    count += next.verts.size();
                    ++end;
                }
                if (end > i + 1) {
                    thread_local std::array<ps2e_draw_vertex, kBlendJoinVertices> joined;
                    size_t offset = 0;
                    for (size_t j = i; j < end; ++j) {
                        const auto& source = buckets[b].draws[j].verts;
                        std::memcpy(joined.data() + offset, source.data(), source.size() * sizeof(source[0]));
                        offset += source.size();
                    }
                    submit_raw(d, d.state, joined.data(), count, b);
                    i = end - 1;
                    continue;
                }
            }
            if (d.mesh) {
                const int r = d.bones.empty() ? ps2e_draw_submit_mesh(g.dev, submit_state, d.mesh, &d.mesh_params)
                            : d.skin_disp ? ps2e_draw_submit_blend_skin(g.dev, submit_state, d.mesh, &d.mesh_params, d.bones.data(),
                                                                        int(d.bones.size() / 4 - d.skin_disp), int(d.skin_disp))
                                          : ps2e_draw_submit_skin(g.dev, submit_state, d.mesh, &d.mesh_params, d.bones.data(),
                                                                  int(d.bones.size() / 4));
                if (r < 0) g.dropped_draws++;
                continue;
            }
            submit_raw(d, *submit_state, d.verts.data(), d.verts.size(), b);
        }
    }
}

void RenderFrame(Gfx::Frame& f, Show show) {
    ++g.pictures;
    auto& rt = g.rt;
    if (show == Show::None) {
        if (f.skip.seq) {
            if (f.skip.skipped) SceneSkipPublishSeq(f.skip.seq); else SceneSkipPublish(0, f.skip.seq);
        }
        rt.target_slot = -1;
        return;
    }
    if (f.send) SubmitBuckets(f.buckets, f.screen_w, f.screen_h, true, f.menu_backdrop_candidate, &f.skip);
    rt.target_slot = int(&f - rt.slots);
    rt.target_fraction = 1.0;
    ps2e_draw_present(g.dev);
}

void PoolRunBatch(void* pool, size_t chunks, void (*job)(void*, size_t), void* ctx) {
    static_cast<BuildPool*>(pool)->RunBatch(chunks, job, ctx);
}
void ShareInterpolation(ps2r::ps2e::SceneInterpolator& si) {
    BuildPool& pool = Builds();
    if (pool.threads.empty() || !OnRenderThread()) si.SetParallel(nullptr, nullptr, 1);
    else si.SetParallel(&PoolRunBatch, &pool, unsigned(pool.threads.size() + 1));
}

void RenderBetween(Gfx::Frame& prev, Gfx::Frame& cur, double fraction, int64_t slot_ns) {
    (void)slot_ns;
    auto& rt = g.rt;
    const int cur_slot = int(&cur - rt.slots);


    if (fraction >= 1.0 || !rt.interpolate || cur.skip.skipped || prev.skip.skipped) {

        if (rt.target_slot == cur_slot && rt.target_fraction == 1.0) {
        } else {
            if (cur.send) SubmitBuckets(cur.buckets, cur.screen_w, cur.screen_h, false, false, &cur.skip);
            rt.target_slot = cur_slot;
            rt.target_fraction = 1.0;
        }
        ps2e_draw_present(g.dev);
        if (rt.interpolate && rt.last_noted_picture != cur.picture) {
            BuildInterpItems(cur);
            rt.last_noted_picture = cur.picture;
        }
        return;
    }

    BuildInterpItems(prev);
    BuildInterpItems(cur);
    auto scene_of = [](const Gfx::Frame& f) {
        ps2r::ps2e::InterpScene sc;
        sc.items = f.items.data();
        sc.count = f.items.size();
        sc.camera = f.camera;
        sc.off_x = kGsMiddle - float(f.screen_w >> 1);
        sc.off_y = kGsMiddle - float(f.screen_h >> 1);
        sc.screen_scale = 16.f;
        sc.ordinal = f.picture;
        return sc;
    };
    ps2r::ps2e::InterpScene sp = scene_of(prev), sc = scene_of(cur);
    sc.cut = prev.camera_object != cur.camera_object;
    ShareInterpolation(rt.interpolator);
    const bool interpolated = rt.interpolator.Interpolate(sp, sc, fraction, rt.interp_out);
    BeginFrameTarget(cur.screen_w, cur.screen_h);
    for (const auto& o : rt.interp_out) {
        if (o.mesh && o.mesh_params) {
            const int r = !o.bones ? ps2e_draw_submit_mesh(g.dev, o.state, o.mesh, o.mesh_params)
                        : o.disp_vec4 ? ps2e_draw_submit_blend_skin(g.dev, o.state, o.mesh, o.mesh_params, o.bones, int(o.bone_vec4), int(o.disp_vec4))
                                      : ps2e_draw_submit_skin(g.dev, o.state, o.mesh, o.mesh_params, o.bones, int(o.bone_vec4));
            if (r < 0) g.dropped_draws++;
            continue;
        }
        if (o.count < 3) continue;
        if (o.copy_frame) ps2e_draw_copy_frame(g.dev);
        if (ps2e_draw_submit(g.dev, o.state, o.verts, int(o.count)) < 0) g.dropped_draws++;
    }
    rt.target_slot = cur_slot;
    rt.target_fraction = interpolated ? fraction : 1.0;
    ps2e_draw_present(g.dev);
}

void PresentFrame(bool send) {
    if (!EnsureDevice()) {
        ClearBuckets();
        return;
    }
    auto& rt = g.rt;

    const uint64_t boundary_key = te::Services::Get().clock().BoundaryCount() + 1;
    const SkipSnap skip_snap = SceneSkipTake();

    unsigned controller_state = 0;

    const bool menu_backdrop_candidate =
        ReadGameState(&controller_state) &&
        (controller_state == 8 || controller_state == 15);
    te::gfx::FlushNativeMeshes();
    if (rt.started) {
        {
            std::unique_lock<std::mutex> lock(rt.mutex);
            if (!rt.queue.empty() || rt.free.empty())
                rt.done.wait(lock, [&] { return rt.queue.empty() && !rt.free.empty(); });
            const int slot = rt.free.back();
            rt.free.pop_back();
            Gfx::Frame& f = rt.slots[slot];
            for (int b = 0; b < TE_GFX_BUCKET_COUNT; ++b) {
                std::swap(g.buckets[b].draws, f.buckets[b].draws);
                std::swap(g.buckets[b].used, f.buckets[b].used);
                std::swap(g.buckets[b].has_pending, f.buckets[b].has_pending);
            }
            g.deferred_recent = g.deferred_used;
            std::swap(g.deferred, f.deferred);
            std::swap(g.deferred_used, f.deferred_used);
            f.retire.insert(f.retire.end(), g.mesh_retired.begin(), g.mesh_retired.end());
            g.mesh_retired.clear();
            f.retire_tex.insert(f.retire_tex.end(), g.texture_retired.begin(), g.texture_retired.end());
            g.texture_retired.clear();
            f.send = send;
            f.menu_backdrop_candidate = menu_backdrop_candidate;
            f.skip = skip_snap;
            f.screen_w = ScreenW();
            f.screen_h = ScreenH();
            f.boundary_key = boundary_key;
            f.items.clear();
            f.picture = ++g.pictures_handed;
            f.thin = g.picture_thin;
            g.picture_thin = false;

            const bool due30 = ThirtyShows(boundary_key - 1, f.picture);
            f.shown30 = !f.thin && due30;
            if (f.shown30 && g.frame_rate.load(std::memory_order_relaxed) == int(Rate::Rate30)) {
                g.shown30_boundary = boundary_key - 1;
                g.shown30_any = true;
            }

            f.have_edge = te::Services::Get().clock().LatestFieldEdge(&f.edge_ns, &f.field);
            f.camera = ps2r::ps2e::InterpCamera{};
            f.camera_object = 0;
            te::gfx::CurrentCamera(f.camera, &f.camera_object);
            rt.queue.push_back(slot);
        }
        rt.wake.notify_one();

    } else {

        static Gfx::Frame sync;
        for (int b = 0; b < TE_GFX_BUCKET_COUNT; ++b) {
            std::swap(g.buckets[b].draws, sync.buckets[b].draws);
            std::swap(g.buckets[b].used, sync.buckets[b].used);
            std::swap(g.buckets[b].has_pending, sync.buckets[b].has_pending);
        }
        sync.send = send;
        sync.menu_backdrop_candidate = menu_backdrop_candidate;
        sync.skip = skip_snap;
        sync.screen_w = ScreenW();
        sync.screen_h = ScreenH();
        sync.boundary_key = boundary_key;
        sync.picture = 0;
        ExpandDeferred(sync.buckets, g.deferred, g.deferred_used);
        RenderFrame(sync, Show::Present);
        for (uint32_t mesh : g.mesh_retired) DestroyMesh(mesh);
        g.mesh_retired.clear();
        for (uint32_t tex : g.texture_retired) DestroyTexture(tex);
        g.texture_retired.clear();
        for (int b = 0; b < TE_GFX_BUCKET_COUNT; ++b) {
            std::swap(g.buckets[b].draws, sync.buckets[b].draws);
            std::swap(g.buckets[b].used, sync.buckets[b].used);
            std::swap(g.buckets[b].has_pending, sync.buckets[b].has_pending);
        }
    }
    ClearBuckets();
    te::gfx::RecordFastBoundary();
    te::gfx::ResetDrawOrdinals();
    VBlank();
}

}

namespace te::gfx {
bool EnsureDevice() { return ::EnsureDevice(); }
bool g_sceneSkipNow = false;
void SceneSkipDecide() { ::SceneSkipDecide(); }
bool TexelsAt(uint32_t address, DecodedTexture& out) {
    if (texpack::Enabled()) return false;
    const auto it = g.texture_sources.find(address);
    if (it == g.texture_sources.end()) return false;
    std::string error;
    return DecodeTextureResource(it->second.header, it->second.packet.data(), it->second.packet.size(), out, &error);
}
MaterialView ReadMaterial(uint32_t address) { return ::ReadMaterial(address); }
ps2e_draw_state StateOf(const MaterialView& m) { return ::StateOf(m); }
bool FiniteQ(const ps2e_draw_vertex* vertices, size_t count) { return ::BatchFiniteQ(vertices, count); }
Draw& OpenDraw(uint32_t bucket, const ps2e_draw_state& state, bool copy_frame, bool finite_q) { return ::OpenDraw(bucket, state, copy_frame, finite_q); }
MaterialView ShaderView(const MaterialView& m, uint32_t k) { return ::ShaderView(m, k); }
void CloseDraw(uint32_t bucket) { ::CloseDraw(bucket); }
void SetDrawTag(const ps2r::ps2e::InterpTag& tag, const float* world16, const float* screen16) { ::SetDrawTag(tag, world16, screen16); }
void ClearDrawTag() { ::ClearDrawTag(); }
bool Interpolating() { return g.interp_active.load(std::memory_order_relaxed); }
bool NativeMeshes() {
    if (g.native_meshes < 0) g.native_meshes = 1;
    return g.native_meshes == 1;
}

bool g_mesh_async_ok = false, g_skin_async_ok = false;
bool MeshesAsync(bool ok) {
    return ok && g.rt.started && !OnRenderThread();
}
void CreateSkins(std::vector<SkinRequest>& requests) {
    if (requests.empty() || !::EnsureDevice()) return;
    if (MeshesAsync(g_skin_async_ok)) {
        struct Job { uint32_t id; std::vector<ps2e_draw_mesh_vertex> vertices; std::vector<ps2e_draw_skin_binding> bones; };
        auto jobs = std::make_shared<std::vector<Job>>();
        for (SkinRequest& r : requests) {
            r.handle = ps2e_draw_mesh_reserve(g.dev);
            jobs->push_back(Job{r.handle, std::move(r.vertices), std::move(r.bones)});
        }
        PostRender([jobs] {
            for (Job& j : *jobs) ps2e_draw_skin_create_at(g.dev, j.id, j.vertices.data(), j.bones.data(), int(j.vertices.size()));
        });
        return;
    }
    OnRender([&] {
        for (SkinRequest& r : requests)
            r.handle = ps2e_draw_skin_create(g.dev, r.vertices.data(), r.bones.data(), int(r.vertices.size()));
    });
    bool all = true;
    for (const SkinRequest& r : requests) all = all && r.handle;
    if (all) g_skin_async_ok = true;
}
void CreateMeshes(std::vector<MeshRequest>& requests) {
    if (requests.empty() || !::EnsureDevice()) return;
    if (MeshesAsync(g_mesh_async_ok && g.native_meshes == 1)) {
        struct Job { uint32_t id; std::vector<ps2e_draw_mesh_vertex> vertices; };
        auto jobs = std::make_shared<std::vector<Job>>();
        for (MeshRequest& r : requests) {
            if (r.vertices.size() < 3) { r.handle = 0; continue; }
            r.handle = ps2e_draw_mesh_reserve(g.dev);
            jobs->push_back(Job{r.handle, std::move(r.vertices)});
        }
        PostRender([jobs] {
            for (Job& j : *jobs) ps2e_draw_mesh_create_at(g.dev, j.id, j.vertices.data(), int(j.vertices.size()));
        });
        return;
    }
    bool declined = false;
    OnRender([&] {
        for (MeshRequest& r : requests) {
            r.handle = ps2e_draw_mesh_create(g.dev, r.vertices.data(), int(r.vertices.size()));
            if (!r.handle && r.vertices.size() >= 3) declined = true;
        }
    });
    if (declined) g.native_meshes = 0;
    else {
        bool all = true;
        for (const MeshRequest& r : requests) all = all && (r.handle || r.vertices.size() < 3);
        if (all) g_mesh_async_ok = true;
    }
}
void RecordMesh(uint32_t bucket, const ps2e_draw_state& state, uint32_t mesh, const ps2e_draw_mesh_params& params,
                const std::array<float, 6>& box) {
    auto& b = g.buckets[bucket < TE_GFX_BUCKET_COUNT ? bucket : TE_GFX_BUCKET_UI];
    if (b.used == b.draws.size()) b.draws.emplace_back();
    Draw& d = b.draws[b.used++];
    PrefetchNextDraw(b);
    d.state = state;
    d.verts.clear();
    d.copy_frame = false;
    d.pending = nullptr;
    d.tag = g.tag;
    d.has_transform = g.tag_has_transform;
    if (g.tag_has_transform) d.transform = g.tag_transform;
    d.mesh = mesh;
    d.mesh_params = params;
    d.mesh_box = box;
    d.mesh_above_far = MeshAboveFarPlane(params, box);
    d.bones.clear();
    d.skin_disp = 0;
}
void RecordSkin(uint32_t bucket, const ps2e_draw_state& state, uint32_t skin, const ps2e_draw_mesh_params& params,
                const float* bones, uint32_t vec4s, const float* disp, uint32_t disp_vec4s) {
    const float unknown = std::numeric_limits<float>::quiet_NaN();
    RecordMesh(bucket, state, skin, params, {unknown, unknown, unknown, unknown, unknown, unknown});
    auto& b = g.buckets[bucket < TE_GFX_BUCKET_COUNT ? bucket : TE_GFX_BUCKET_UI];
    Draw& d = b.draws[b.used - 1];
    d.bones.assign(bones, bones + size_t(vec4s) * 4);
    if (disp && disp_vec4s) d.bones.insert(d.bones.end(), disp, disp + size_t(disp_vec4s) * 4);
    d.skin_disp = disp ? disp_vec4s : 0;
}
ps2e_draw_mesh_params SkinParams(const std::array<std::array<uint32_t, 4>, 4>& to_screen, const std::array<float, 2>& scroll,
                                 const std::array<std::array<uint32_t, 4>, 3>& lights, const std::array<std::array<uint32_t, 4>, 3>& colours,
                                 const std::array<uint32_t, 4>& ambient) {
    const auto F = [](uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; };
    ps2e_draw_mesh_params mp{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) mp.screen[r * 4 + c] = F(to_screen[size_t(r)][size_t(c)]);
    mp.offset[0] = ::GsOriginX();
    mp.offset[1] = ::GsOriginY();
    mp.scroll[0] = scroll[0];
    mp.scroll[1] = scroll[1];
    mp.lit = 1;
    for (int l = 0; l < 3; ++l)
        for (int k = 0; k < 3; ++k) {
            mp.light_dir[l][k] = F(lights[size_t(l)][size_t(k)]);
            mp.light_colour[l][k] = F(colours[size_t(l)][size_t(k)]);
        }
    for (int k = 0; k < 3; ++k) mp.ambient[k] = F(ambient[size_t(k)]);
    mp.box[0] = mp.box[1] = -1e9f;
    mp.box[2] = mp.box[3] = 1e9f;
    return mp;
}
bool GpuSkins() {
    return g.native_meshes;
}
bool GpuShadows() {
    return g.native_meshes;
}
void RetireMesh(uint32_t mesh) {
    if (mesh) g.mesh_retired.push_back(mesh);
}
void Record(uint32_t bucket, Draw&& d) { ::Record(bucket, std::move(d)); }
bool DeferringBuilds() { return ::DeferringBuilds(); }
DeferredBuild& ReserveDeferred(uint32_t bucket, const ps2e_draw_state& state) { return ::ReserveDeferred(bucket, state); }
bool SkipBuild(DeferredBuild& build) { return ::SkipBuild(build); }
void PostBuildJob(DeferredBuild& build, BuildJob&& job) { ::PostBuildJob(build, std::move(job)); }
void DrainBuilds() { DrainRecordLane(); ::DrainBuilds(); }
float GsOriginX() { return ::GsOriginX(); }
float GsOriginY() { return ::GsOriginY(); }
int ScreenW() { return ::ScreenW(); }
int ScreenH() { return ::ScreenH(); }
const RenderTargetDescription* CurrentTarget() { return ::CurrentTarget(); }
}



extern "C" void te_graphics_shutdown() {
    te::gfx::StopRecordLane();
    StopRenderThread();
    DrainBuilds();
    g.shut_down = true;
}

extern "C" void te_graphics_release_device() {
    if (!g.shut_down || !g.dev) return;
    ps2e_draw_destroy(g.dev);
    g.dev = nullptr;
}

namespace te::gfx {
void SetPresentation(const ps2e_draw_presentation& p) {
    g.presentation = p;
    if (g.dev) ps2e_draw_set_presentation(g.dev, &g.presentation);
}

void SetFrameRate(int rate) {
    g.frame_rate.store(std::clamp(rate, 0, 5), std::memory_order_relaxed);
    if (g.rt.started) g.rt.wake.notify_one();
}

}

extern "C" {

void ps2e_graphics_reset_devices(void) {
    te::gfx::DrainRecordLane(); }
void ps2e_graphics_wait_v_sync(void) {
    te::gfx::DrainRecordLane(); EnsureDevice(); VBlank(); }
ps2e_s32 ps2e_graphics_is_pal_display(void) {
    te::gfx::DrainRecordLane(); return g.pal ? 1 : 0; }
void ps2e_graphics_wait_idle(void) {
    te::gfx::DrainRecordLane(); }

void ps2e_graphics_start_renderer(ps2e_s32 height, ps2e_s32 pal) {
    te::gfx::DrainRecordLane();
    g.pal = pal != 0;
    g.display_w = 512;
    g.display_h = height > 0 && height <= 1024 ? height : (g.pal ? 512 : 448);

    if (uint32_t* w = GuestMutable<uint32_t>(kDisplayWidth)) *w = uint32_t(g.display_w);
    if (uint32_t* h = GuestMutable<uint32_t>(kDisplayHeight)) *h = uint32_t(g.display_h);
    if (g.dev) {
        OnRender([] { ps2e_draw_resize_frame(g.dev, g.display_w, g.display_h); });
    } else {
        EnsureDevice();
    }
}
void ps2e_graphics_finish_renderer_start(void) {
    te::gfx::DrainRecordLane(); }

void ps2e_graphics_move_display(ps2e_u32 offset) {
    te::gfx::DrainRecordLane();
    const Vector2* o = Guest<Vector2>(offset);
    if (!o) return;
    te::display::MoveDisplay(o->x, o->y);
}

void ps2e_graphics_set_up_render_target(ps2e_u32 target) {
    te::gfx::DrainRecordLane();
    auto* t = GuestMutable<RenderTargetDescription>(target);
    if (!t) return;
    const float left = kGsMiddle - float(t->displayWidth) * 0.5f + float(t->offsetX);
    const float top = kGsMiddle - float(t->displayHeight) * 0.5f + float(t->offsetY);
    const float height = float(t->height), width = float(t->width);
    for (auto& row : t->clipToScreen)
        for (float& v : row) v = 0.f;
    t->clipToScreen[0][0] = width * 8.0f;
    t->clipToScreen[1][1] = -height * 8.0f;
    t->clipToScreen[2][2] = -8388607.0f;
    t->clipToScreen[3][0] = (left + width * 0.5f) * 16.0f;
    t->clipToScreen[3][1] = (top + height * 0.5f) * 16.0f;
    t->clipToScreen[3][2] = 8388607.0f;
    t->clipToScreen[3][3] = 1.0f;
    t->x = int32_t(left);
    t->y = int32_t(top);
}

void ps2e_graphics_present(ps2e_u32 commands) {
    te::gfx::DrainRecordLane(); PresentFrame(commands != 0); }
void ps2e_graphics_present_from_interrupt(ps2e_u32 commands) {
    te::gfx::DrainRecordLane();
    (void)commands;
    PresentFrame(true);
}
void ps2e_graphics_wait_sent(void) {
    te::gfx::DrainRecordLane(); }

void ps2e_graphics_finish_frame(void) {
    te::gfx::DrainRecordLane(); }

void RecordColourFilter() {
    constexpr uint32_t kOn = 0x00309C31, kBlends = 0x00309C32, kAmount = 0x00309C34, kPalette = 0x00309C38, kSecond = 0x00309C3C,
                       kEffect = 0x00309C51, kTable = 0x002F6FF8;
    const uint8_t* effect = Guest<uint8_t>(kEffect);
    const uint8_t* on = Guest<uint8_t>(kOn);
    const uint8_t* blends = Guest<uint8_t>(kBlends);
    const float* amount = Guest<float>(kAmount);
    const uint32_t* palette = Guest<uint32_t>(kPalette);
    const uint32_t* second = Guest<uint32_t>(kSecond);
    const uint32_t* table = reinterpret_cast<const uint32_t*>(te::gfx::RecordGuestPointer(kTable, 8 * 256 * 4));
    if (!effect || !on || !blends || !amount || !palette || !second || !table) return;
    if (*effect == 0 || *on == 0) return;
    uint32_t first = *palette, other = *second;
    ps2e_draw_state st{};
    st.gouraud = 0;
    st.depth_test = PS2E_DRAW_DEPTH_ALWAYS;
    st.depth_write = 0;
    if (*blends != 0) {

        if (first > 7 || other > 7) return;
        const uint8_t* from = reinterpret_cast<const uint8_t*>(table + first * 256);
        const uint8_t* to = reinterpret_cast<const uint8_t*>(table + other * 256);
        const float rest = 1.0f - *amount;
        uint32_t rgb = 0;
        for (uint32_t channel = 0; channel < 3; ++channel) {
            int32_t value = static_cast<int32_t>(static_cast<float>(from[channel]) * *amount + rest * static_cast<float>(to[channel]));
            value = value > 0xFF ? 0xFF : value < 0 ? 0 : value;
            rgb |= static_cast<uint32_t>(value) << (channel * 8);
        }
        st.haze = 1;
        st.haze_flat = 0x01000000u | rgb;
    } else {
        if (first > 7) return;

        bool identity = true;
        for (uint32_t i = 0; i < 256 && identity; ++i) identity = (table[first * 256 + i] >> 24) >= 0x80;
        if (identity) return;
        st.haze = int(first) + 1;
    }
    static bool palettes_taken = false, haze_unsupported = false;
    if (haze_unsupported) return;
    if (!palettes_taken) {
        EnsureDevice();
        if (!g.dev) return;
        if (!ps2e_draw_haze_supported(g.dev)) {
            palettes_taken = true;
            haze_unsupported = true;
            return;
        }
        ps2e_draw_haze_palettes(g.dev, table);
        palettes_taken = true;
    }
    const FrameStart& f = g.frame;
    const float ox = GsOriginX(), oy = GsOriginY();
    const float x0 = float(f.x) - ox, y0 = float(f.y) - oy, x1 = float(f.x + f.width) - ox, y1 = float(f.y + f.height) - oy;
    Draw d;
    d.state = st;
    auto V = [&](float x, float y) {
        ps2e_draw_vertex v = Vertex(x, y, 0, 0, 0x80808080u);
        v.z = 0.f;
        return v;
    };
    d.verts = {V(x0, y0), V(x1, y0), V(x0, y1), V(x1, y0), V(x1, y1), V(x0, y1)};
    ::Record(4, std::move(d));
}

void ps2e_graphics_finish_scene(ps2e_s32 effects) {
    te::gfx::DrainRecordLane();
    if (effects && g.have_frame) RecordColourFilter();
}

void ps2e_gfx2_frame_reset(ps2e_s32 movie) {
    te::gfx::DrainRecordLane();
    (void)movie;
    ClearBuckets();
}

void ps2e_graphics_start_frame(ps2e_u32 frame) {
    te::gfx::DrainRecordLane();
    const FrameStart* f = Guest<FrameStart>(frame);
    if (!f) return;
    g.frame = *f;
    g.have_frame = true;
    SceneSkipDecide();
    EnsureDevice();
    if (!f->clearColor && !f->clearDepth) return;
    ps2e_draw_state st{};
    st.gouraud = 0;
    st.depth_test = PS2E_DRAW_DEPTH_ALWAYS;
    st.depth_write = f->clearDepth ? 1 : 0;
    st.no_colour_write = f->clearColor ? 0 : 1;
    const uint32_t colour = uint32_t(f->red) | uint32_t(f->green) << 8 | uint32_t(f->blue) << 16 | 0x80u << 24;
    const float ox = GsOriginX(), oy = GsOriginY();
    const float x0 = float(f->x) - ox, y0 = float(f->y) - oy, x1 = float(f->x + f->width) - ox, y1 = float(f->y + f->height) - oy;
    Draw d;
    d.state = st;
    auto V = [&](float x, float y) {
        ps2e_draw_vertex v = Vertex(x, y, 0, 0, colour);
        v.z = 0.f;
        return v;
    };
    d.verts = {V(x0, y0), V(x1, y0), V(x0, y1), V(x1, y0), V(x1, y1), V(x0, y1)};
    ::Record(TE_GFX_BUCKET_SKY, std::move(d));
}

void ps2e_graphics_use_helper_programs(ps2e_s32 set, ps2e_s32 wait) {
    te::gfx::DrainRecordLane();
    (void)set;
    (void)wait;
}

void ps2e_graphics_draw_sprite(ps2e_u32 material, ps2e_s32 colour, ps2e_u32 place, ps2e_u32 texture) {
    te::gfx::DrainRecordLane();
    const Rectangle* p = Guest<Rectangle>(place);
    const Rectangle* t = Guest<Rectangle>(texture);
    if (!p || !t) return;
    const MaterialView m = ::ReadMaterial(material);
    const float sw = GameScreenW(), sh = GameScreenH();
    const float left = p->x * sw + FrameCornerX(TargetOffsetX());
    const float top = p->y * sh + FrameCornerY(TargetOffsetY());
    const float right = left + p->width * sw, bottom = top + p->height * sh;
    RecordQuad(m.bucket, ::StateOf(m), left, top, right, bottom, t->x, t->y, t->x + t->width, t->y + t->height, uint32_t(colour));
}

void ps2e_graphics_draw_turned_sprite(ps2e_u32 material, ps2e_s32 colour, ps2e_u32 corners, ps2e_u32 texture) {
    te::gfx::DrainRecordLane();
    const Vector2* c = reinterpret_cast<const Vector2*>(te::gfx::RecordGuestPointer(corners, sizeof(Vector2) * 4));
    const Rectangle* t = Guest<Rectangle>(texture);
    if (!c || !t) return;
    const MaterialView m = ::ReadMaterial(material);
    const float sw = GameScreenW(), sh = GameScreenH();
    const float left = FrameCornerX(0), top = FrameCornerY(0);
    const float ox = GsOriginX(), oy = GsOriginY();

    const Vector2 uv[4] = {{t->x, t->y}, {t->x, t->y + t->height}, {t->x + t->width, t->y}, {t->x + t->width, t->y + t->height}};
    ps2e_draw_vertex v[4];
    for (int i = 0; i < 4; ++i) v[i] = Vertex(c[i].x * sw + left - ox, c[i].y * sh + top - oy, uv[i].x, uv[i].y, uint32_t(colour));
    Draw d;
    d.state = ::StateOf(m);
    ClampWholeTexture(d.state, t->x, t->y, t->x + t->width, t->y + t->height);
    d.verts = {v[0], v[1], v[2], v[1], v[3], v[2]};
    ::Record(m.bucket, std::move(d));
}

void ps2e_graphics_draw_strip(ps2e_u32 material, ps2e_s32 count, ps2e_u32 places, ps2e_u32 colours) {
    te::gfx::DrainRecordLane();
    if (count < 3 || count > 4096) return;
    const Vector2* p = reinterpret_cast<const Vector2*>(te::gfx::RecordGuestPointer(places, sizeof(Vector2) * size_t(count)));
    const uint32_t* c = reinterpret_cast<const uint32_t*>(te::gfx::RecordGuestPointer(colours, 4 * size_t(count)));
    if (!p || !c) return;
    const MaterialView m = ::ReadMaterial(material);
    const float left = FrameCornerX(TargetOffsetX()) - GsOriginX(), top = FrameCornerY(TargetOffsetY()) - GsOriginY();
    Draw d;
    d.state = ::StateOf(m);
    d.state.texture = 0;
    d.verts.reserve(size_t(count - 2) * 3);
    for (int i = 2; i < count; ++i) {

        for (int k = i - 2; k <= i; ++k) d.verts.push_back(Vertex(p[k].x + left, p[k].y + top, 0, 0, c[k]));
    }
    ::Record(m.bucket, std::move(d));
}

void ps2e_gfx2_text_item(ps2e_u32 text, ps2e_f32 x, ps2e_f32 y, ps2e_s32 alignment) {
    te::gfx::DrainRecordLane();
    (void)text; (void)x; (void)y; (void)alignment;
}

void ps2e_gfx2_text_quads(ps2e_u32 material, ps2e_u32 quads, ps2e_s32 count) {
    te::gfx::DrainRecordLane();
    if (count <= 0 || count > 4096) return;
    const auto* q = reinterpret_cast<const te_gfx_text_quad*>(te::gfx::RecordGuestPointer(quads, sizeof(te_gfx_text_quad) * size_t(count)));
    if (!q) return;
    const MaterialView m = ::ReadMaterial(material);
    ps2e_draw_state st = ::StateOf(m);
    float tw = 1.f, th = 1.f;
    if (m.texture) {
        tw = float(1 << m.texture->texture.widthPower);
        th = float(1 << m.texture->texture.heightPower);
    }
    const float ox = GsOriginX(), oy = GsOriginY();
    Draw d;
    d.state = st;
    d.verts.reserve(size_t(count) * 6);
    for (int i = 0; i < count; ++i) {
        const float x0 = q[i].x0 / 16.f - ox, y0 = q[i].y0 / 16.f - oy, x1 = q[i].x1 / 16.f - ox, y1 = q[i].y1 / 16.f - oy;
        const float u0 = q[i].u0 / 16.f / tw, v0 = q[i].v0 / 16.f / th, u1 = q[i].u1 / 16.f / tw, v1 = q[i].v1 / 16.f / th;
        const uint32_t colour = q[i].colour;
        d.verts.push_back(Vertex(x0, y0, u0, v0, colour));
        d.verts.push_back(Vertex(x1, y0, u1, v0, colour));
        d.verts.push_back(Vertex(x0, y1, u0, v1, colour));
        d.verts.push_back(Vertex(x1, y0, u1, v0, colour));
        d.verts.push_back(Vertex(x1, y1, u1, v1, colour));
        d.verts.push_back(Vertex(x0, y1, u0, v1, colour));
    }
    ::Record(m.bucket, std::move(d));
}

ps2e_s32 ps2e_gfx2_texture_loaded(ps2e_u32 texture, ps2e_u32 header, ps2e_u32 image, ps2e_s32 size) {
    te::gfx::DrainRecordLane();
    const auto* h = Guest<te_gfx_texture_header>(header);
    const uint8_t* bytes = te::gfx::RecordGuestPointer(image, size_t(size));
    if (!h || !bytes || size <= 0 || !EnsureDevice()) return 0;
    const uint64_t key = TexCacheKey(*h, bytes, size_t(size));

    if (const uint32_t cached = TexCacheFind(key, *h, bytes, size_t(size))) {
        auto old = g.textures.find(texture);
        if (old != g.textures.end()) {
            ReleaseReplacement(old->second);
            ReleaseTextureHandle(old->second);
            g.textures.erase(old);
            InvalidateTextureLookup();
        }
        g.textures[texture] = cached;
        InvalidateTextureLookup();
        g.texture_sources[texture] = Gfx::TextureSource{*h, std::vector<uint8_t>(bytes, bytes + size)};
        return ps2e_s32(cached);
    }

    if (te::gfx::texpack::Enabled() && PackAsync()) {
        auto old = g.textures.find(texture);
        if (old != g.textures.end()) {
            ReleaseReplacement(old->second);
            ReleaseTextureHandle(old->second);
            g.textures.erase(old);
            InvalidateTextureLookup();
        }
        const uint32_t id = ps2e_draw_texture_reserve(g.dev);
        auto load = std::make_shared<Gfx::PackLoad>();
        g.pack_loads[id] = load;
        struct Posted { te_gfx_texture_header header; std::vector<uint8_t> packet; };
        auto job = std::make_shared<Posted>(Posted{*h, std::vector<uint8_t>(bytes, bytes + size)});
        te::gfx::texpack::Hold();
        PostRender([job, id, load] {
            te::gfx::DecodedTexture decoded;
            std::string error;
            const bool ok = te::gfx::DecodeTextureResource(job->header, job->packet.data(), job->packet.size(), decoded, &error);
            if (!ok) {
                te::gfx::texpack::Unhold();
                return;
            }
            auto texels = std::make_shared<std::vector<uint32_t>>(decoded.rgba);
            const int width = decoded.width, height = decoded.height;
            const uint32_t made = CreateTexture(decoded);
            if (made) RenameTexture(made, id);
            StartPackRead(id, load, std::move(texels), width, height);
            te::gfx::texpack::Unhold();
        });
        g.textures[texture] = id;
        InvalidateTextureLookup();
        g.texture_sources[texture] = Gfx::TextureSource{*h, std::vector<uint8_t>(bytes, bytes + size)};
        return ps2e_s32(id);
    }

    if (PostsTextures() && !te::gfx::texpack::Enabled()) {
        auto old = g.textures.find(texture);
        if (old != g.textures.end()) {
            ReleaseReplacement(old->second);
            ReleaseTextureHandle(old->second);
            g.textures.erase(old);
            InvalidateTextureLookup();
        }
        const uint32_t id = ps2e_draw_texture_reserve(g.dev);
        struct Posted { te_gfx_texture_header header; std::vector<uint8_t> packet; };
        auto job = std::make_shared<Posted>(Posted{*h, std::vector<uint8_t>(bytes, bytes + size)});
        PostRender([job, id] {
            te::gfx::DecodedTexture decoded;
            std::string error;
            const bool ok = te::gfx::DecodeTextureResource(job->header, job->packet.data(), job->packet.size(), decoded, &error);
            if (!ok) return;
            const uint32_t made = CreateTexture(decoded);
            if (made) RenameTexture(made, id);
        });
        uint64_t texel_bytes = 0;
        for (unsigned i = 0; i < std::max<unsigned>(std::min<unsigned>(h->levels, 7), 1); ++i)
            texel_bytes += uint64_t(1u << (h->widthPower > i ? h->widthPower - i : 0)) * (1u << (h->heightPower > i ? h->heightPower - i : 0)) * 4u;
        TexCacheAdd(id, key, *h, bytes, size_t(size), texel_bytes);
        g.textures[texture] = id;
        InvalidateTextureLookup();
        g.texture_sources[texture] = Gfx::TextureSource{*h, std::vector<uint8_t>(bytes, bytes + size)};
        return ps2e_s32(id);
    }
    te::gfx::DecodedTexture decoded;
    std::string error;
    if (!te::gfx::DecodeTextureResource(*h, bytes, size_t(size), decoded, &error)) return 0;
    auto old = g.textures.find(texture);
    if (old != g.textures.end()) {
        ReleaseReplacement(old->second);
        ReleaseTextureHandle(old->second);
        g.textures.erase(old);
        InvalidateTextureLookup();
    }
    const uint64_t texel_bytes = DecodedBytes(decoded);
    const uint32_t handle = LoadTexture(decoded, true);
    if (!handle) return 0;
    TexCacheAdd(handle, key, *h, bytes, size_t(size), texel_bytes);
    g.textures[texture] = handle;
    InvalidateTextureLookup();
    g.texture_sources[texture] = Gfx::TextureSource{*h, std::vector<uint8_t>(bytes, bytes + size)};
    return ps2e_s32(handle);
}

void ps2e_gfx2_texture_released(ps2e_u32 texture) {
    te::gfx::DrainRecordLane();
    auto it = g.textures.find(texture);
    if (it != g.textures.end()) {
        ReleaseReplacement(it->second);
    }
    g.texture_sources.erase(texture);
    if (it == g.textures.end()) return;
    const uint32_t handle = it->second;
    if (g.dev) ReleaseTextureHandle(handle);
    g.textures.erase(it);
    InvalidateTextureLookup();
}

}

extern "C" ps2e_draw* te_graphics_device(void) { return EnsureDevice() ? g.dev : nullptr; }

extern "C" void te_graphics_on_render(void (*fn)(void* user), void* user) { OnRender([fn, user] { fn(user); }); }
extern "C" void te_graphics_post_render(void (*fn)(void* user), void* user) {
    PostRender([fn, user] { fn(user); });
}
extern "C" void te_graphics_record_movie_quad(uint32_t texture, float x0, float y0, float x1, float y1, float u0, float v0,
                                              float u1, float v1) {
    te::gfx::DrainRecordLane();
    ps2e_draw_state st{};
    st.texture = texture;
    st.gouraud = 0;
    st.depth_test = PS2E_DRAW_DEPTH_ALWAYS;
    st.depth_write = 1;
    st.blend = 0;
    st.tfx = PS2E_DRAW_TFX_DECAL;
    st.tcc = 1;
    st.linear = 1;
    st.wrap_u = PS2E_DRAW_WRAP_CLAMP;
    st.wrap_v = PS2E_DRAW_WRAP_CLAMP;
    Draw d;
    d.state = st;
    const uint32_t white = 0x80FFFFFFu;
    d.verts = {Vertex(x0, y0, u0, v0, white), Vertex(x1, y0, u1, v0, white), Vertex(x0, y1, u0, v1, white),
               Vertex(x1, y0, u1, v0, white), Vertex(x1, y1, u1, v1, white), Vertex(x0, y1, u0, v1, white)};
    ::Record(TE_GFX_BUCKET_MOVIES, std::move(d));
}

namespace te::gfx {

}

extern "C" ps2e_u32 ps2e_gfx2_tick_options() {
    te::gfx::DrainRecordLane();
    return 1;
}
