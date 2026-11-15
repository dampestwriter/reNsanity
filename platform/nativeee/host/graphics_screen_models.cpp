#include "record_lane.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#include "graphics_internal.h"
#include "ps2e_seam.h"
#include "te_host.h"

namespace {
using te::gfx::Draw;
using te::gfx::MaterialView;

struct Vertex {
    float x, y, z;
    uint8_t rgba[4];
};
struct Model {
    uint32_t material = 0;
    std::vector<Vertex> verts;
};

constexpr uint32_t kFirstHandle = 0xF0000001u;

struct Builder {
    bool open = false;
    uint32_t material = 0;
    uint32_t colour = 0;
    bool coloured = false;
    std::vector<Vertex> verts;
} g_builder;

std::map<uint32_t, Model> g_models;
uint32_t g_next = kFirstHandle;

const float* Matrix(uint32_t address) { return reinterpret_cast<const float*>(te::GuestPointer(address, 64)); }

struct Clip {
    float x, y, z, w;
    float sx, sy, sz, sw;
    const uint8_t* rgba;
};

void Transform(const float* m, float x, float y, float z, float out[4]) {
    for (int c = 0; c < 4; ++c) out[c] = x * m[c] + y * m[4 + c] + z * m[8 + c] + m[12 + c];
}

constexpr float kNear = 1e-3f;
Clip Lerp(const Clip& a, const Clip& b, float t, uint8_t out_rgba[4]) {
    Clip r;
    r.x = a.x + (b.x - a.x) * t; r.y = a.y + (b.y - a.y) * t; r.z = a.z + (b.z - a.z) * t; r.w = a.w + (b.w - a.w) * t;
    r.sx = a.sx + (b.sx - a.sx) * t; r.sy = a.sy + (b.sy - a.sy) * t; r.sz = a.sz + (b.sz - a.sz) * t; r.sw = a.sw + (b.sw - a.sw) * t;
    for (int k = 0; k < 4; ++k) out_rgba[k] = uint8_t(std::lround(a.rgba[k] + (b.rgba[k] - a.rgba[k]) * t));
    r.rgba = out_rgba;
    return r;
}

void Emit(const Clip& c, float offX, float offY, std::vector<ps2e_draw_vertex>& out) {
    ps2e_draw_vertex v{};
    const float inv = c.sw != 0.f ? 1.f / c.sw : 0.f;

    v.x = std::floor(c.sx * inv) / 16.f - offX;
    v.y = std::floor(c.sy * inv) / 16.f - offY;
    v.z = std::floor(c.sz * inv);
    v.r = c.rgba[0]; v.g = c.rgba[1]; v.b = c.rgba[2]; v.a = c.rgba[3];
    out.push_back(v);
}

void ClipTriangle(const Clip* tri, float offX, float offY, std::vector<ps2e_draw_vertex>& out) {
    Clip poly[4];
    uint8_t colours[4][4];
    int n = 0;
    for (int i = 0; i < 3; ++i) {
        const Clip& a = tri[i];
        const Clip& b = tri[(i + 1) % 3];
        const bool ain = a.w > kNear, bin = b.w > kNear;
        if (ain) poly[n++] = a;
        if (ain != bin) {
            const float t = (kNear - a.w) / (b.w - a.w);
            poly[n] = Lerp(a, b, t, colours[n]);
            n++;
        }
    }
    for (int i = 1; i + 1 < n; ++i) {
        Emit(poly[0], offX, offY, out);
        Emit(poly[i], offX, offY, out);
        Emit(poly[i + 1], offX, offY, out);
    }
}
}

namespace te::gfx {

}

extern "C" {

void ps2e_graphics_begin_screen_model(void) {
    te::gfx::DrainRecordLane();
    g_builder = Builder{};
    g_builder.open = true;
}

void ps2e_graphics_set_screen_model_material(ps2e_u32 material) {
    te::gfx::DrainRecordLane();
    if (material) g_builder.material = material;
}

void ps2e_graphics_set_screen_model_colour(ps2e_s32 colour) {
    te::gfx::DrainRecordLane();
    g_builder.colour = uint32_t(colour);
    g_builder.coloured = true;
}

void ps2e_graphics_add_screen_model_vertex(ps2e_u32 placeAddress) {
    te::gfx::DrainRecordLane();
    const float* place = reinterpret_cast<const float*>(te::GuestPointer(placeAddress, 16));
    if (!place) return;
    Vertex v{};
    v.x = place[0]; v.y = place[1]; v.z = place[2];
    for (int k = 0; k < 4; ++k) v.rgba[k] = uint8_t(g_builder.colour >> (8 * k));
    g_builder.verts.push_back(v);
}

ps2e_u32 ps2e_graphics_end_screen_model(void) {
    te::gfx::DrainRecordLane();
    Model m;
    m.material = g_builder.material;
    m.verts = std::move(g_builder.verts);
    g_builder = Builder{};
    const uint32_t handle = g_next++;
    if (g_next < kFirstHandle) g_next = kFirstHandle;
    g_models[handle] = std::move(m);
    return handle;
}

void ps2e_graphics_delete_screen_model(ps2e_u32 model) {
    te::gfx::DrainRecordLane();
    g_models.erase(model);
}

void ps2e_graphics_draw_screen_model(ps2e_u32 modelHandle, ps2e_u32 toScreenAddress, ps2e_u32 toClipAddress, ps2e_u32 clip) {
    te::gfx::DrainRecordLane();
    (void)clip;
    auto it = g_models.find(modelHandle);
    if (it == g_models.end()) return;
    const Model& m = it->second;
    const float* toScreen = Matrix(toScreenAddress);
    const float* toClip = Matrix(toClipAddress);
    if (!toScreen || !toClip || m.verts.size() < 3 || !m.material || !te::gfx::EnsureDevice()) return;
    const MaterialView mv = te::gfx::ReadMaterial(m.material);
    ps2e_draw_state state = te::gfx::StateOf(mv);
    state.texture = 0;
    const uint32_t bucket = mv.material ? mv.bucket : TE_GFX_BUCKET_UI;
    const float offX = te::gfx::GsOriginX(), offY = te::gfx::GsOriginY();
    static std::vector<Clip> clipped;
    static std::vector<ps2e_draw_vertex> verts;
    clipped.clear();
    verts.clear();
    for (const Vertex& v : m.verts) {
        Clip c;
        float cl[4], sc[4];
        Transform(toClip, v.x, v.y, v.z, cl);
        Transform(toScreen, v.x, v.y, v.z, sc);
        c.x = cl[0]; c.y = cl[1]; c.z = cl[2]; c.w = cl[3];
        c.sx = sc[0]; c.sy = sc[1]; c.sz = sc[2]; c.sw = sc[3];
        c.rgba = v.rgba;
        clipped.push_back(c);
    }
    for (size_t i = 0; i + 2 < clipped.size(); ++i) {

        const Clip tri[3] = {clipped[i], clipped[i + 1], clipped[i + 2]};
        ClipTriangle(tri, offX, offY, verts);
    }
    if (verts.empty()) return;
    Draw& d = te::gfx::OpenDraw(bucket, state, false, te::gfx::FiniteQ(verts.data(), verts.size()));
    d.verts.insert(d.verts.end(), verts.begin(), verts.end());
    te::gfx::CloseDraw(bucket);
}

}
