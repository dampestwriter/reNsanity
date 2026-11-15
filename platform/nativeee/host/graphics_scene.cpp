#include "record_lane.h"
#include "record_math.h"

#include <algorithm>
#include <memory>
#include <new>
#include <array>
#include <atomic>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_geometry.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/mesh_geometry.h"
#include "games/slus_20909_crash_twinsanity/hle/math/clipping_math.h"
#include "games/slus_20909_crash_twinsanity/hle/math/culling_math.h"
#include "games/slus_20909_crash_twinsanity/hle/math/culling_planes.h"
#include "alpha_trim.h"
#include "graphics_internal.h"
#include "ps2e_seam.h"
#include "te_gfx_layout.h"
#include "te_host.h"
#include "te_mesh_builder.h"
#include "builder_reuse.h"
#include "cull_incremental.h"

#include "games/slus_20909_crash_twinsanity/hle/graphics/skin_geometry.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/skinned_lit_strip.h"
#include "engine/vu/vu_chop_round_guard.h"

namespace {

using te::gfx::Draw;
using te::gfx::MaterialView;
using Vector = title::CullingMath::Vector;
using Matrix = title::CullingMath::Matrix;
using Geometry = title::MeshGeometry;

struct RenderView {
    float projection[4][4], toCamera[4][4], toClip[4][4], toScreen[4][4], cameraToScreen[4][4];
    float clip[4], forward[4];
    uint32_t cameraObject;
    float inverseAspect;
    uint8_t unused168[8];
};
struct ChunkView {
    float toClip[4][4];
    float clipped[4][4];
    float toScreen[4][4];
    float clip[4];
    float origin[4];
    float camera[4];
    int32_t distance;
    uint32_t view;
    uint32_t visibility;
    uint32_t lastVisibility;
    uint32_t matrices;
    uint32_t vtable;
};
struct ModelLights {
    uint32_t directions, colours, ambient;
};

constexpr uint32_t kRenderViewPointer = 0x00309BE0;
constexpr uint32_t kViewPlanes = 0x003B4900;
constexpr uint32_t kFarViewPlanes = 0x003B4960;
constexpr uint32_t kShadowToCamera = 0x003B0770;
constexpr uint32_t kShadowToScreen = 0x003B07B0;
constexpr uint32_t DrawUnclipped = 1;
constexpr uint32_t BucketSky = 0, BucketOpaque = 2, BucketShadows = 21;

template <typename T>
const T* Guest(uint32_t address) {
    return reinterpret_cast<const T*>(te::gfx::RecordGuestPointer(address, sizeof(T)));
}
template <typename T>
T* GuestMutable(uint32_t address) {
    return reinterpret_cast<T*>(te::gfx::RecordGuestPointer(address, sizeof(T)));
}

void Flat(const Matrix& m, float out[16]) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[r * 4 + c] = m[r][c];
}

std::optional<Matrix> WorldOfScreen(const Matrix& screen);
void TagRigid(uint64_t id, const Matrix& world, const Matrix& screen) {
    float w[16], sc[16];
    Flat(world, w);
    Flat(screen, sc);
    te::gfx::SetDrawTag(ps2r::ps2e::InterpTag{id, 0, ps2r::ps2e::InterpKind::Rigid}, w, sc);
}
uint64_t TagId(uint32_t a, uint32_t b) { return (uint64_t(a) << 32) | uint64_t(b); }

std::unordered_map<uint64_t, uint32_t> g_drawOrdinals;
uint64_t OrdinalId(uint64_t base) {
    const uint32_t k = g_drawOrdinals[base]++;
    return base ^ (uint64_t(k) * 0xD6E8FEB86659FD93ull);
}

std::optional<Matrix> ReadMatrix(uint32_t address) {
    const auto* m = reinterpret_cast<const float(*)[4]>(te::gfx::RecordGuestPointer(address, 64));
    if (!m) return std::nullopt;
    Matrix out;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[r][c] = m[r][c];
    return out;
}
Matrix ToMatrix(const float (*m)[4]) {
    Matrix out;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[r][c] = m[r][c];
    return out;
}
std::optional<Vector> ReadVector(uint32_t address) {
    const float* v = reinterpret_cast<const float*>(te::gfx::RecordGuestPointer(address, 16));
    if (!v) return std::nullopt;
    return Vector{v[0], v[1], v[2], v[3]};
}
void WriteMatrix(uint32_t address, const Matrix& m) {
    auto* out = reinterpret_cast<float(*)[4]>(te::gfx::RecordGuestPointer(address, 64));
    if (!out) return;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[r][c] = m[r][c];
}

Matrix Multiply(const Matrix& a, const Matrix& b) {
    Matrix out{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            float s = 0.f;
            for (int k = 0; k < 4; ++k) s += a[r][k] * b[k][c];
            out[r][c] = s;
        }
    return out;
}
Matrix Identity() { return Matrix{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}}; }
Matrix Translation(const Vector& t) {
    Matrix m = Identity();
    m[3] = {t[0], t[1], t[2], 1.f};
    return m;
}

std::optional<Matrix> AffineInverse(const Matrix& m) {
    const float a = m[0][0], b = m[0][1], c = m[0][2], d = m[1][0], e = m[1][1], f = m[1][2], g = m[2][0], h = m[2][1], i = m[2][2];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!std::isfinite(det) || std::fabs(det) < 1e-12f) return std::nullopt;
    const float r = 1.f / det;
    Matrix o = Identity();
    o[0][0] = (e * i - f * h) * r; o[0][1] = (c * h - b * i) * r; o[0][2] = (b * f - c * e) * r;
    o[1][0] = (f * g - d * i) * r; o[1][1] = (a * i - c * g) * r; o[1][2] = (d * c - a * f) * r;
    o[2][0] = (d * h - e * g) * r; o[2][1] = (b * g - a * h) * r; o[2][2] = (a * e - b * d) * r;
    for (int k = 0; k < 3; ++k) {
        o[3][k] = -(m[3][0] * o[0][k] + m[3][1] * o[1][k] + m[3][2] * o[2][k]);
        o[k][3] = 0.f;
    }
    o[3][3] = 1.f;
    return o;
}

std::optional<Vector> EyeIn(const Matrix& world, const float (*toCamera)[4]) {
    const auto invCam = AffineInverse(ToMatrix(toCamera));
    const auto invWorld = AffineInverse(world);
    if (!invCam || !invWorld) return std::nullopt;
    Vector eye{};
    for (int k = 0; k < 3; ++k) eye[k] = (*invCam)[3][k];
    eye[3] = 1.f;
    Vector o{};
    for (int k = 0; k < 3; ++k) o[k] = eye[0] * (*invWorld)[0][k] + eye[1] * (*invWorld)[1][k] + eye[2] * (*invWorld)[2][k] + (*invWorld)[3][k];
    o[3] = 1.f;
    for (int k = 0; k < 3; ++k) if (!std::isfinite(o[k])) return std::nullopt;
    return o;
}

bool Finite(const Matrix& m) {
    for (const auto& row : m)
        for (float v : row)
            if (!std::isfinite(v)) return false;
    return true;
}

const RenderView* CurrentView() {
    const uint32_t* p = Guest<uint32_t>(kRenderViewPointer);
    return p && *p ? Guest<RenderView>(*p) : nullptr;
}

std::optional<Matrix> WorldOfScreen(const Matrix& screen) {
    const RenderView* view = CurrentView();
    if (!view) return std::nullopt;

    struct ViewInverse { float toCamera[4][4], cameraToScreen[4][4]; double inv[16]; int rounding = -1; bool valid = false, invertible = false; };
    thread_local ViewInverse cache;
    const int rounding = std::fegetround();
    if (!cache.valid || cache.rounding != rounding || std::memcmp(cache.toCamera, view->toCamera, sizeof cache.toCamera) != 0 ||
        std::memcmp(cache.cameraToScreen, view->cameraToScreen, sizeof cache.cameraToScreen) != 0) {
        double cam[16], c2s[16], ws[16];
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) {
                cam[r * 4 + c] = view->toCamera[r][c];
                c2s[r * 4 + c] = view->cameraToScreen[r][c];
            }
        std::memcpy(cache.toCamera, view->toCamera, sizeof cache.toCamera);
        std::memcpy(cache.cameraToScreen, view->cameraToScreen, sizeof cache.cameraToScreen);
        ps2r::ps2e::SceneInterpolator::Multiply4(cam, c2s, ws);
        cache.invertible = ps2r::ps2e::SceneInterpolator::Invert4(ws, cache.inv);
        cache.rounding = rounding;
        cache.valid = true;
    }
    if (!cache.invertible) return std::nullopt;
    double sc[16], w[16];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) sc[r * 4 + c] = screen[r][c];
    ps2r::ps2e::SceneInterpolator::Multiply4(sc, cache.inv, w);
    Matrix out;
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) out[r][c] = float(w[r * 4 + c]);
    if (!Finite(out)) return std::nullopt;
    return out;
}

struct BuildHint {
    mutable std::atomic<uint32_t> vertices{0};
    BuildHint() = default;
    BuildHint(const BuildHint& o) : vertices(o.vertices.load(std::memory_order_relaxed)) {}
    BuildHint& operator=(const BuildHint& o) {
        vertices.store(o.vertices.load(std::memory_order_relaxed), std::memory_order_relaxed);
        return *this;
    }
    uint32_t Load() const { return vertices.load(std::memory_order_relaxed); }

    void Store(size_t n) const {
        const uint32_t last = vertices.load(std::memory_order_relaxed);
        vertices.store(std::max(uint32_t(n), last - last / 4), std::memory_order_relaxed);
    }
};

void ReserveVertices(te::gfx::DeferredBuild& build, const BuildHint& hint) {
    const size_t want = build.verts.size() + hint.Load();
    if (build.verts.capacity() < want) build.verts.reserve(want);
}

uint64_t g_part_revision = 0;
struct Part {
    uint64_t revision = 0;
    Geometry::Mesh mesh;
    te::gfx::MeshPrepared prepared;
    std::vector<std::vector<std::array<float, 3>>> normals;
    std::vector<std::vector<std::array<uint8_t, 4>>> colours;
    bool decoded = false;
    uint32_t vertices = 0;
    uint32_t native = 0;
    std::array<float, 6> nativeBox{};
    mutable uint8_t meshState = 0;
    BuildHint hint;
};

struct PartSource {
    int32_t vertices = 0;
    std::vector<uint8_t> packet;
};
struct Model {
    std::vector<Part> parts;
    std::vector<PartSource> sources;
};
std::unordered_map<uint32_t, Model> g_models;

struct ModelLookupEntry { uint64_t epoch=0; uint32_t address=0; const Model* model=nullptr; };
ModelLookupEntry g_modelLookup[1024];
uint64_t g_modelLookupEpoch=1;
void InvalidateModelLookup() {
    if(++g_modelLookupEpoch==0) {for(auto& e:g_modelLookup)e.epoch=0;g_modelLookupEpoch=1;}
}
const Model* FindModel(uint32_t address) {

    auto& e=g_modelLookup[((address>>4)^(address>>15))&1023];
    if(e.epoch==g_modelLookupEpoch && e.address==address){
        return e.model;
    }
    auto it=g_models.find(address);e.model=it==g_models.end()?nullptr:&it->second;
    e.address=address;e.epoch=g_modelLookupEpoch;return e.model;
}

struct ShadowModel { uint64_t revision = ++g_part_revision; std::vector<title::ShadowGeometry::Mesh> parts; std::vector<bool> decoded; bool tried = false; std::vector<BuildHint> hints;

                     std::vector<uint8_t> gpuState; std::vector<uint32_t> gpu; std::vector<std::array<float, 6>> gpuBox; };
std::unordered_map<uint32_t, ShadowModel> g_shadowModels;
std::vector<std::pair<uint32_t, uint32_t>> g_shadowRequests;
void RetireGpuShadow(ShadowModel& sm) {
    for (uint32_t& h : sm.gpu) { if (h) te::gfx::RetireMesh(h); h = 0; }
    std::fill(sm.gpuState.begin(), sm.gpuState.end(), 0);
}
void LoadModelPart(uint32_t model, int32_t index, int32_t count, const uint8_t* bytes, int32_t size, int32_t vertices);

void RetireTrims(uint64_t revision);
void RetireMesh(Part& part) {
    RetireTrims(part.revision);
    te::gfx::RetireMesh(part.native);
    part.native = 0;
    part.meshState = 0;
}

std::vector<ps2e_draw_mesh_vertex> MeshVertices(const Part& part) {
    std::vector<ps2e_draw_mesh_vertex> out;
    const auto& prepared = part.prepared;
    std::vector<ps2e_draw_mesh_vertex> strip;
    for (size_t si = 0; si < prepared.strips.size(); ++si) {
        const auto& range = prepared.strips[si];
        const bool normals = si < part.normals.size() && part.normals[si].size() == range.count;
        const bool colours = si < part.colours.size() && part.colours[si].size() == range.count;
        strip.assign(range.count, ps2e_draw_mesh_vertex{});
        for (uint32_t i = 0; i < range.count; ++i) {
            const size_t at = range.first + i;
            ps2e_draw_mesh_vertex& v = strip[i];
            v.x = prepared.positions[at][0];
            v.y = prepared.positions[at][1];
            v.z = prepared.positions[at][2];
            v.s = prepared.stq[at][0];
            v.t = prepared.stq[at][1];
            v.q = prepared.stq[at][2];
            const auto& c = prepared.colours[at];
            v.r = c[0]; v.g = c[1]; v.b = c[2]; v.a = c[3];
            if (normals) {
                v.nx = part.normals[si][i][0];
                v.ny = part.normals[si][i][1];
                v.nz = part.normals[si][i][2];
            }
            const auto& lc = colours ? part.colours[si][i] : c;
            v.lr = lc[0]; v.lg = lc[1]; v.lb = lc[2]; v.la = lc[3];
        }
        unsigned parity = 0;
        for (uint32_t i = 0; i < range.count; ++i) {
            if (prepared.restart[range.first + i]) { parity = 0; continue; }
            if (i < 2) continue;
            out.push_back(strip[i - 2 + (parity & 1)]);
            out.push_back(strip[i - 1 - (parity & 1)]);
            out.push_back(strip[i]);
            ++parity;
        }
    }
    return out;
}

std::vector<std::pair<uint32_t, uint32_t>> g_meshRequests;
std::vector<std::pair<uint32_t, uint32_t>> g_skinRequests;

bool UnpackWords(uint32_t cmd, uint32_t num, size_t& words) {
    const uint32_t vn = (cmd >> 2) & 3, vl = cmd & 3;
    const uint32_t fields = vn + 1;
    if (num == 0) num = 256;
    switch (vl) {
        case 0: words = size_t(num) * fields; return true;
        case 1: words = (size_t(num) * fields * 2 + 3) / 4; return true;
        case 2: words = (size_t(num) * fields + 3) / 4; return true;
        case 3: if (vn != 3) return false; words = (size_t(num) * 2 + 3) / 4; return true;
    }
    return false;
}

bool DecodePacket(const uint8_t* bytes, size_t size, uint32_t vertices, Part& part, std::string* why) {
    auto word = [&](size_t at) {
        uint32_t v;
        std::memcpy(&v, bytes + at, 4);
        return v;
    };
    if (size < 16 || (size & 3)) { if (why) *why = "size"; return false; }
    size_t cursor = 0;
    std::vector<float> pos, stq, nrm;
    std::vector<uint32_t> stqBits, col8;
    bool havePos = false, haveStq = false;
    uint32_t total = 0;
    auto flushStrip = [&]() -> bool {
        if (!havePos) return true;
        const size_t n = pos.size() / 3;
        if (n < 1) { havePos = haveStq = false; pos.clear(); stq.clear(); stqBits.clear(); nrm.clear(); col8.clear(); return true; }
        Geometry::Strip strip;
        strip.vertices.resize(n);
        std::vector<std::array<float, 3>> normals;
        std::vector<std::array<uint8_t, 4>> colours;
        for (size_t i = 0; i < n; ++i) {
            auto& v = strip.vertices[i];
            for (int k = 0; k < 3; ++k) {
                v.position[k] = pos[i * 3 + k];
                if (!std::isfinite(v.position[k])) v.position[k] = 0.f;
            }
            if (haveStq && stq.size() >= (i + 1) * 4) {
                for (int k = 0; k < 3; ++k) {
                    v.stq[k] = stq[i * 4 + k];
                    if (!std::isfinite(v.stq[k])) v.stq[k] = k == 2 ? 1.f : 0.f;
                }
                for (int k = 0; k < 4; ++k) v.colour[k] = uint8_t(stqBits[i * 4 + k]);
                v.restart = (stqBits[i * 4 + 3] & 0x8000) != 0;
            } else {
                v.stq = {0.f, 0.f, 1.f};
                v.colour = {0x80, 0x80, 0x80, 0x80};
            }
            if (nrm.size() >= (i + 1) * 3) normals.push_back({nrm[i * 3], nrm[i * 3 + 1], nrm[i * 3 + 2]});
            if (col8.size() > i) {
                const uint32_t c = col8[i];
                colours.push_back({uint8_t(c), uint8_t(c >> 8), uint8_t(c >> 16), uint8_t(c >> 24)});
            }
        }
        if (normals.size() == n) part.normals.push_back(std::move(normals)); else part.normals.push_back({});
        if (colours.size() == n) part.colours.push_back(std::move(colours)); else part.colours.push_back({});
        total += uint32_t(n);
        part.mesh.strips.push_back(std::move(strip));
        havePos = haveStq = false;
        pos.clear(); stq.clear(); stqBits.clear(); nrm.clear(); col8.clear();
        return true;
    };

    auto vif = [&](size_t from, size_t to) -> bool {
        size_t at = from;
        while (at + 4 <= to) {
            const uint32_t code = word(at);
            at += 4;
            const uint32_t cmd = (code >> 24) & 0x7F;
            const uint32_t num = (code >> 16) & 0xFF;
            const uint32_t imm = code & 0xFFFF;
            if (cmd >= 0x60) {
                size_t words;
                if (!UnpackWords(cmd, num, words)) { if (why) *why = "unpack format"; return false; }
                if (at + words * 4 > to) { if (why) *why = "unpack overruns"; return false; }
                const uint32_t addr = imm & 0x3FF;
                const uint32_t vn = (cmd >> 2) & 3, vl = cmd & 3;
                const uint32_t count = num ? num : 256;
                if (vl == 0 && vn == 2) {
                    if (!havePos || addr == 3) {
                        if (havePos && addr == 3) { if (!flushStrip()) return false; }
                        pos.resize(size_t(count) * 3);
                        std::memcpy(pos.data(), bytes + at, size_t(count) * 12);
                        havePos = true;
                    } else {
                        nrm.resize(size_t(count) * 3);
                        std::memcpy(nrm.data(), bytes + at, size_t(count) * 12);
                    }
                } else if (vl == 0 && vn == 3 && count > 1) {
                    stq.resize(size_t(count) * 4);
                    stqBits.resize(size_t(count) * 4);
                    std::memcpy(stq.data(), bytes + at, size_t(count) * 16);
                    std::memcpy(stqBits.data(), bytes + at, size_t(count) * 16);
                    haveStq = true;
                } else if (vl == 2 && vn == 3) {
                    col8.resize(count);
                    std::memcpy(col8.data(), bytes + at, size_t(count) * 4);
                }
                at += words * 4;
                continue;
            }
            switch (cmd) {
                case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
                case 0x10: case 0x11: case 0x13: case 0x15: case 0x17:
                    break;
                case 0x14:
                    if (!flushStrip()) return false;
                    break;
                case 0x20: at += 4; break;
                case 0x30: case 0x31: at += 16; break;
                case 0x4A: at += size_t(num ? num : 256) * 8; break;
                case 0x50: case 0x51: at += size_t(imm ? imm : 65536) * 16; break;
                default:
                    if (why) *why = "vif code";
                    return false;
            }
        }
        return true;
    };

    while (cursor + 16 <= size) {
        const uint32_t tag0 = word(cursor);
        const uint32_t qwc = tag0 & 0xFFFF;
        const uint32_t id = (tag0 >> 28) & 7;
        if (!vif(cursor + 8, cursor + 16)) return false;
        cursor += 16;
        const size_t end = cursor + size_t(qwc) * 16;
        if (id == 3 || id == 4) {
            continue;
        }
        if (end > size) { if (why) *why = "tag overruns"; return false; }
        if (!vif(cursor, end)) return false;
        cursor = end;
        if (id == 6 || id == 7) break;
        if (id == 2 || id == 5) break;
    }
    if (!flushStrip()) return false;
    part.vertices = total;
    part.decoded = !part.mesh.strips.empty();
    if (!part.decoded && why) *why = "no strips";
    (void)vertices;
    return part.decoded;
}

struct Lights {
    std::array<Vector, 3> directions;
    std::array<Vector, 3> colours;
    Vector ambient;
};

void LitColours(const Part& part, const Lights& lights, std::vector<std::array<uint8_t, 4>>& out) {
    out.resize(part.prepared.vertex_count);
    size_t at = 0;
    for (size_t s = 0; s < part.mesh.strips.size(); ++s) {
        const auto& strip = part.mesh.strips[s];
        const bool normals = s < part.normals.size() && part.normals[s].size() == strip.vertices.size();
        const bool colours = s < part.colours.size() && part.colours[s].size() == strip.vertices.size();
        for (size_t i = 0; i < strip.vertices.size(); ++i, ++at) {
            const auto& v = strip.vertices[i];
            std::array<uint8_t, 4> col = colours ? part.colours[s][i] : v.colour;
            float lit[3] = {lights.ambient[0], lights.ambient[1], lights.ambient[2]};
            if (normals) {
                const auto& n = part.normals[s][i];
                for (int l = 0; l < 3; ++l) {
                    const auto& d = lights.directions[l];
                    const float dot = std::max(0.f, n[0] * d[0] + n[1] * d[1] + n[2] * d[2]);
                    for (int k = 0; k < 3; ++k) lit[k] += lights.colours[l][k] * dot;
                }
            }
            auto& o = out[at];
            for (int k = 0; k < 3; ++k) {
                const float value = float(col[k]) * lit[k];
                o[k] = uint8_t(std::min(255.f, std::max(0.f, value)));
            }
            o[3] = col[3];
        }
    }
}

ps2r::gs::DrawIr BuildState() {
    ps2r::gs::DrawIr st{};
    st.prim_type = 4;
    st.tex_enabled = true;
    st.off_x = te::gfx::GsOriginX();
    st.off_y = te::gfx::GsOriginY();
    return st;
}

ps2e_draw_vertex DrawVertex(const ps2r::gs::VertexRec& v, bool textured) {
    ps2e_draw_vertex o{};
    o.x = v.x;
    o.y = v.y;
    o.z = v.z;
    if (textured && v.q != 0.f) {
        o.u = v.s;
        o.v = v.t;
        o.q = v.q;
    }
    o.r = v.r;
    o.g = v.g;
    o.b = v.b;
    o.a = v.a;
    return o;
}

struct Emitter {
    uint32_t bucket = 0;
    ps2e_draw_state state{};
    bool textured = false;

    bool screenCopy = false;
    bool copyPending = false;
    std::array<float, 2> copyScroll{};
    void Begin(uint32_t b, const ps2e_draw_state& st) {
        bucket = b;
        state = st;
        textured = state.texture != 0;
        screenCopy = false;
        copyPending = false;
    }
    void BeginScreenCopy(uint32_t b, const ps2e_draw_state& st, std::array<float, 2> scroll) {
        Begin(b, st);
        screenCopy = textured;
        copyPending = screenCopy;
        copyScroll = scroll;
    }
    void Append(const ps2r::gs::VertexRec* verts, size_t count) {
        count = count / 3 * 3;
        if (!count) return;
        Draw& d = te::gfx::OpenDraw(bucket, state, copyPending, !textured || screenCopy ||
            std::all_of(verts, verts + count, [](const auto& v) { return std::isfinite(v.q); }));
        copyPending = false;
        const size_t base = d.verts.size();
        d.verts.resize(base + count);
        const float invW = 1.f / float(std::max(1, te::gfx::ScreenW())), invH = 1.f / float(std::max(1, te::gfx::ScreenH()));
        for (size_t i = 0; i < count; ++i) {
            const auto& v = verts[i];
            ps2e_draw_vertex& o = d.verts[base + i];
            o = DrawVertex(v, textured && !screenCopy);
            if (screenCopy) {
                const float ds = v.q != 0.f ? v.s / v.q : 0.f, dt = v.q != 0.f ? v.t / v.q : 0.f;
                o.u = std::min(1.f, std::max(0.f, v.x * invW + ds)) + copyScroll[0];
                o.v = std::min(1.f, std::max(0.f, v.y * invH + dt)) + copyScroll[1];
            }
        }
        te::gfx::CloseDraw(bucket);
    }
    static void Sink(void* user, const ps2r::gs::VertexRec* verts, size_t count, bool fan) {
        (void)fan;
        static_cast<Emitter*>(user)->Append(verts, count);
    }

    void Build(const Part& part, const Geometry::View& view, const std::array<uint8_t, 4>* colours, Geometry::Result* result) {
        const Geometry::Result r = te::gfx::MeshBuild(part.prepared, view, BuildState(), colours, &Sink, this);
        if (result) *result = r;
    }
};
std::vector<std::array<uint8_t, 4>> g_colourScratch;

struct Collector {
    te::gfx::DeferredBuild* out = nullptr;
    bool textured = false;
    size_t first_vertex = 0, first_segment = 0;
    Collector(te::gfx::DeferredBuild* build, bool textured_)
        : out(build), textured(textured_), first_vertex(build->verts.size()), first_segment(build->segments.size()) {}
    void Append(const ps2r::gs::VertexRec* verts, size_t count) {
        count = count / 3 * 3;
        if (!count) return;

        ps2e_draw_vertex* to = Grow(count);
        const bool tex = textured;
        for (size_t i = 0; i < count; ++i) to[i] = DrawVertex(verts[i], tex);
        out->segments.push_back(uint32_t(count));
    }

    void AppendOrdered(const ps2r::gs::VertexRec* verts, const uint8_t* order, size_t count) {
        count = count / 3 * 3;
        if (!count) return;
        ps2e_draw_vertex* to = Grow(count);
        const bool tex = textured;
        for (size_t i = 0; i < count; ++i) to[i] = DrawVertex(verts[order[i]], tex);
        out->segments.push_back(uint32_t(count));
    }
    ps2e_draw_vertex* Grow(size_t count) {
        const size_t base = out->verts.size();
        if (out->verts.capacity() < base + count) out->verts.reserve(std::max(base + count, base * 2));
        out->verts.resize(base + count);
        return out->verts.data() + base;
    }

    void Done(const BuildHint& hint) const { hint.Store(out->verts.size() - first_vertex); }

    void Discard() {
        out->verts.resize(first_vertex);
        out->segments.resize(first_segment);
    }
    static void Sink(void* user, const ps2r::gs::VertexRec* verts, size_t count, bool fan) {
        (void)fan;
        static_cast<Collector*>(user)->Append(verts, count);
    }
    static void Body(void* user, const ps2r::gs::VertexRec* verts, const uint8_t* order, size_t count) {
        static_cast<Collector*>(user)->AppendOrdered(verts, order, count);
    }
};

constexpr size_t kMemoBudget = 128 * 1024 * 1024, kMemoEntryLimit = 256 * 1024;
constexpr uint32_t kMemoMissesToRelease = 4, kMemoRetryEvery = 16;
std::atomic<size_t> g_memo_bytes{0};
struct RigidMemo {
    uint64_t revision = 0;
    Geometry::View view{};
    std::optional<Lights> lights;
    std::optional<Vector> environment_eye;
    Matrix environment_clip{};
    float off_x = 0, off_y = 0;
    int rounding = 0;
    te::ExactFp fp;
    bool textured = false, valid = false;
    size_t vertices = 0, segments = 0, capacity = 0;
    uint32_t misses = 0;
    std::unique_ptr<uint8_t[]> bytes;
    void Release() {
        if (!capacity) return;
        g_memo_bytes.fetch_sub(capacity, std::memory_order_relaxed);
        bytes.reset();
        capacity = 0;
        valid = false;
    }
    ~RigidMemo() { g_memo_bytes.fetch_sub(capacity, std::memory_order_relaxed); }
    bool Matches(uint64_t partRevision, const Geometry::View& v, const std::optional<Lights>& l,
                 const ps2r::gs::DrawIr& st, bool tex, const std::optional<Vector>& eye = {}, const Matrix* reflectionClip = nullptr) const {
        if (!valid || revision != partRevision || rounding != std::fegetround() || !(fp == te::ExactFp::Read()) || textured != tex ||
            environment_eye.has_value() != eye.has_value() ||
            lights.has_value() != l.has_value() || view.clipped != v.clipped) return false;
        if (eye && (!reflectionClip || std::memcmp(&*eye, &*environment_eye, sizeof(Vector)) ||
                    std::memcmp(reflectionClip, &environment_clip, sizeof(Matrix)))) return false;
        if (std::memcmp(&off_x, &st.off_x, sizeof(float)) || std::memcmp(&off_y, &st.off_y, sizeof(float)) ||
            std::memcmp(view.screen.data(), v.screen.data(), sizeof(v.screen)) ||
            std::memcmp(view.clip.data(), v.clip.data(), sizeof(v.clip)) ||
            std::memcmp(view.guard.data(), v.guard.data(), sizeof(v.guard)) ||
            std::memcmp(view.scroll.data(), v.scroll.data(), sizeof(v.scroll))) return false;
        return !l || (!std::memcmp(lights->directions.data(), l->directions.data(), sizeof(l->directions)) &&
                      !std::memcmp(lights->colours.data(), l->colours.data(), sizeof(l->colours)) &&
                      !std::memcmp(lights->ambient.data(), l->ambient.data(), sizeof(l->ambient)));
    }
    void Load(te::gfx::DeferredBuild& build) const {
        build.verts.resize(vertices);
        build.segments.resize(segments);
        if (vertices) std::memcpy(build.verts.data(), bytes.get(), vertices * sizeof(ps2e_draw_vertex));
        if (segments) std::memcpy(build.segments.data(), bytes.get() + vertices * sizeof(ps2e_draw_vertex), segments * sizeof(uint32_t));
    }
    void Store(uint64_t partRevision, const Geometry::View& v, const std::optional<Lights>& l,
               const ps2r::gs::DrawIr& st, bool tex, const te::gfx::DeferredBuild& build, size_t entryLimit = kMemoEntryLimit,
               const std::optional<Vector>& eye = {}, const Matrix* reflectionClip = nullptr) {
        valid = false;
        if (++misses >= kMemoMissesToRelease) {
            Release();
            if ((misses - kMemoMissesToRelease) % kMemoRetryEvery != 0) return;
        }
        const size_t need = build.verts.size() * sizeof(ps2e_draw_vertex) + build.segments.size() * sizeof(uint32_t);
        if (need > entryLimit) return;
        if (need > capacity) {
            const size_t extra = need > capacity ? need - capacity : 0;
            size_t total = g_memo_bytes.load(std::memory_order_relaxed);
            do { if (total + extra > kMemoBudget) return; }
            while (!g_memo_bytes.compare_exchange_weak(total, total + extra, std::memory_order_relaxed));

            std::unique_ptr<uint8_t[]> fresh(new (std::nothrow) uint8_t[need]);
            if (!fresh) {
                g_memo_bytes.fetch_sub(extra, std::memory_order_relaxed);
                return;
            }
            bytes.swap(fresh);
            if (capacity > need) {
                g_memo_bytes.fetch_sub(capacity - need, std::memory_order_relaxed);
            }
            capacity = need;
        }
        vertices = build.verts.size(); segments = build.segments.size();
        if (vertices) std::memcpy(bytes.get(), build.verts.data(), vertices * sizeof(ps2e_draw_vertex));
        if (segments) std::memcpy(bytes.get() + vertices * sizeof(ps2e_draw_vertex), build.segments.data(), segments * sizeof(uint32_t));
        revision = partRevision; view = v; lights = l; off_x = st.off_x; off_y = st.off_y;
        environment_eye = eye;
        if (eye && reflectionClip) environment_clip = *reflectionClip;
        rounding = std::fegetround(); fp = te::ExactFp::Read(); textured = tex; valid = fp.valid;
    }
};

bool Deferrable() {
    return te::gfx::DeferringBuilds();
}

struct BoxCorners { float o[8][4]; };
void CornersThrough(const Part& part, const Geometry::View& view, BoxCorners& out, bool fast) {
    const auto& b = part.nativeBox;

    if (fast && std::all_of(b.begin(),b.end(),[](float f){ return std::isfinite(f); })) {
        te::gfx::recordmath::Corners(b.data(),view.screen,out.o);
        return;
    }
    for (int c = 0; c < 8; ++c) {
        const float px = b[(c & 1) ? 3 : 0], py = b[(c & 2) ? 4 : 1], pz = b[(c & 4) ? 5 : 2];
        for (int k = 0; k < 4; ++k)
            out.o[c][k] = view.screen[3][size_t(k)] + view.screen[0][size_t(k)] * px + view.screen[1][size_t(k)] * py +
                          view.screen[2][size_t(k)] * pz;
    }
}
bool NativeInFront(const Part& part, const BoxCorners& corners) {
    if (!part.native) return false;
    for (const auto& o : corners.o) {
        if (!(o[3] > 0.f)) return false;
        const float z = o[2] / o[3];
        if (!(z >= 0.f && z < 2147483648.f)) return false;
    }
    return true;
}

struct TrimKey {
    uint64_t revision; uint32_t texture, handle; int atst, aref, tfx, wrapU, wrapV;
    bool operator==(const TrimKey& o) const {
        return revision == o.revision && texture == o.texture && handle == o.handle && atst == o.atst && aref == o.aref &&
               tfx == o.tfx && wrapU == o.wrapU && wrapV == o.wrapV;
    }
};
struct TrimKeyHash {
    size_t operator()(const TrimKey& k) const {
        uint64_t h = k.revision * 0x9E3779B97F4A7C15ull ^ (uint64_t(k.texture) << 20) ^ k.handle;
        h ^= uint64_t(k.atst | k.aref << 4 | k.tfx << 12 | k.wrapU << 16 | k.wrapV << 17) * 0xBF58476D1CE4E5B9ull;
        return size_t(h ^ (h >> 31));
    }
};
struct TrimEntry {
    std::vector<ps2e_draw_mesh_vertex> triangles;
    std::vector<uint8_t> failing;
    size_t flagged = 0;
    uint32_t mesh = 0;
    uint8_t meshState = 0;
    uint32_t model = 0, part = 0;
};
std::unordered_map<TrimKey, TrimEntry, TrimKeyHash> g_trims;
std::vector<TrimEntry*> g_trimRequests;

TrimEntry* TrimFor(uint32_t model, uint32_t index, const Part& part, const MaterialView& m, const ps2e_draw_state& state) {
    if (!te::alphatrim::Applies(state) || !m.shader || !m.shader->texture || !m.texture_handle) return nullptr;
    const TrimKey key{part.revision, m.shader->texture, m.texture_handle, state.alpha_method, state.alpha_reference, state.tfx,
                      state.wrap_u, state.wrap_v};
    auto it = g_trims.find(key);
    if (it == g_trims.end()) {
        TrimEntry e;
        e.model = model; e.part = index;
        e.triangles = MeshVertices(part);

        static std::unordered_map<uint64_t, std::shared_ptr<te::gfx::DecodedTexture>> decoded;
        const uint64_t texKey = uint64_t(m.shader->texture) << 32 | m.texture_handle;
        std::shared_ptr<te::gfx::DecodedTexture> held;
        if (const auto found = decoded.find(texKey); found != decoded.end()) held = found->second;
        else {
            held = std::make_shared<te::gfx::DecodedTexture>();
            if (!te::gfx::TexelsAt(m.shader->texture, *held)) held->width = 0;
            if (decoded.size() >= 32) decoded.clear();
            decoded.emplace(texKey, held);
        }
        const te::gfx::DecodedTexture& tex = *held;
        if (tex.width > 0) {
            e.flagged = te::alphatrim::FailingTriangles(e.triangles, tex, state, e.failing);
        } else e.failing.assign(e.triangles.size() / 3, 0);
        it = g_trims.emplace(key, std::move(e)).first;
    }
    return &it->second;
}
void RetireTrims(uint64_t revision) {
    if (g_trims.empty()) return;
    for (auto it = g_trims.begin(); it != g_trims.end();) {
        if (it->first.revision != revision) { ++it; continue; }
        if (it->second.mesh) te::gfx::RetireMesh(it->second.mesh);
        g_trimRequests.erase(std::remove(g_trimRequests.begin(), g_trimRequests.end(), &it->second), g_trimRequests.end());
        it = g_trims.erase(it);
    }
}

struct InstanceState {
    const ps2e_draw_state* overrideState = nullptr;
    uint32_t overrideBucket = 0;
    const Lights* lights = nullptr;
    std::optional<Vector> eye;
};

struct ScreenCopyScratch {
    Geometry::Mesh mesh;
    te::gfx::MeshPrepared prepared;
} g_screenCopyScratch;
bool PrepareScreenCopyMesh(const Part& part, const Matrix& clip, float cornerValue) {
    auto& sc = g_screenCopyScratch;
    sc.mesh = part.mesh;
    for (size_t st = 0; st < sc.mesh.strips.size(); ++st) {
        auto& strip = sc.mesh.strips[st];
        const bool normals = st < part.normals.size() && part.normals[st].size() == strip.vertices.size();
        for (size_t i = 0; i < strip.vertices.size(); ++i) {
            auto& v = strip.vertices[i];
            float dx = 0.f, dy = 0.f;
            if (normals) {
                const auto& n = part.normals[st][i];
                dx = (n[0] * clip[0][0] + n[1] * clip[1][0] + n[2] * clip[2][0]) * cornerValue;
                dy = (n[0] * clip[0][1] + n[1] * clip[1][1] + n[2] * clip[2][1]) * cornerValue;
            }
            v.stq = {std::isfinite(dx) ? dx : 0.f, std::isfinite(dy) ? dy : 0.f, 1.f};
        }
    }
    return te::gfx::MeshPrepare(sc.mesh, sc.prepared) == Geometry::Result::Ok;
}

bool PrepareEnvMesh(const Part& part, const Matrix& clip, const Vector& eye, ScreenCopyScratch& sc = g_screenCopyScratch) {
    sc.mesh = part.mesh;
    for (size_t st = 0; st < sc.mesh.strips.size(); ++st) {
        auto& strip = sc.mesh.strips[st];
        const bool normals = st < part.normals.size() && part.normals[st].size() == strip.vertices.size();
        for (size_t i = 0; i < strip.vertices.size(); ++i) {
            auto& v = strip.vertices[i];
            float u = 0.5f, t = 0.5f;
            if (normals) {
                const auto& n = part.normals[st][i];
                float e[3], h2[3], env[3];
                float d2 = 0.f;
                for (int k = 0; k < 3; ++k) { e[k] = eye[k] - v.position[k]; d2 += e[k] * e[k]; }
                const float rsq = d2 > 0.f ? 1.f / std::sqrt(d2) : 0.f;
                for (int k = 0; k < 3; ++k) h2[k] = (e[k] * rsq + n[k]) * 0.5f;
                for (int k = 0; k < 3; ++k) env[k] = h2[0] * clip[0][k] + h2[1] * clip[1][k] + h2[2] * clip[2][k];
                const float d2v = env[0] * env[0] + env[1] * env[1] + env[2] * env[2];
                const float rsqv = d2v > 0.f ? 1.f / std::sqrt(d2v) : 0.f;
                u = 0.5f * env[0] * rsqv + 0.5f;
                t = -0.5f * env[1] * rsqv + 0.5f;
            }
            u = std::isfinite(u) ? std::min(1.f, std::max(0.f, u)) : 0.5f;
            t = std::isfinite(t) ? std::min(1.f, std::max(0.f, t)) : 0.5f;
            v.stq = {u, t, 1.f};
        }
    }
    return te::gfx::MeshPrepare(sc.mesh, sc.prepared) == Geometry::Result::Ok;
}
bool EnvType(uint32_t type) { return type == 0x0C || type == 0x0F; }

bool PassDrawable(uint32_t type, bool haveEye) {
    switch (type) {
        case 0x00: case 0x01: case 0x02: case 0x04: case 0x0A: case 0x0D: case 0x0E: case 0x10: case 0x11: case 0x12:
        case 0x13: case 0x15: case 0x16: case 0x19: case 0x1B: case 0x1E: return true;
        case 0x0C: case 0x0F: return haveEye;
        default: return false;
    }
}
bool ScreenCopyType(uint32_t type) { return type == 0x10 || type == 0x11; }

void DrawInstance(const te_gfx_rigid_model* rigid, const Geometry::View& view, const InstanceState& inst) {
    const te_gfx_model* data = rigid->model ? Guest<te_gfx_model>(rigid->model) : nullptr;
    if (!data) return;
    const Model* model = FindModel(rigid->model);
    if (!model) {
        return;
    }
    const bool recordFast = te::gfx::RecordFast();
    const uint32_t count = std::min<uint32_t>(rigid->count, data->subModelCount);
    const uint32_t* materials = count ? reinterpret_cast<const uint32_t*>(te::gfx::RecordGuestPointer(rigid->materials, 4 * size_t(count))) : nullptr;
    static Emitter emitter;
    for (uint32_t i = 0; i < count && i < model->parts.size(); ++i) {
        const Part& part = model->parts[i];

        if (!recordFast && materials && i + 1 < count && materials[i + 1] && !inst.overrideState)
            if (const uint32_t* next = Guest<uint32_t>(materials[i + 1] + 8)) __builtin_prefetch(next, 0, 3);
        if (!part.decoded) continue;
        const uint32_t resource = materials ? materials[i] : 0;
        const uint32_t* materialAddress = (!inst.overrideState && resource) ? Guest<uint32_t>(resource + 8) : nullptr;
        const MaterialView material = te::gfx::ReadMaterial(materialAddress ? *materialAddress : 0);

        const uint32_t passes = inst.overrideState || !material.material
                                    ? 1u : std::max<uint32_t>(1u, std::min<uint32_t>(material.material->shaderCount, TE_GFX_MAX_SHADERS));

        bool cpuPassFollows = false;
        if (passes > 1)
            for (uint32_t k = 0; k < passes && !cpuPassFollows; ++k) {
                const MaterialView mk = k ? te::gfx::ShaderView(material, k) : material;
                cpuPassFollows = mk.shader && (k == 0 || PassDrawable(mk.shader->type, inst.eye.has_value())) &&
                                 ((k && EnvType(mk.shader->type) && inst.eye.has_value()) || ScreenCopyType(mk.shader->type));
            }
        for (uint32_t pass = 0; pass < passes; ++pass) {
            ps2e_draw_state state;
            uint32_t bucket;
            bool lit = false, screenCopy = false, envMap = false;
            MaterialView m = material;
            Geometry::View sub = view;
            std::array<float, 2> scroll{0.f, 0.f};
            if (inst.overrideState) {
                state = *inst.overrideState;
                bucket = inst.overrideBucket;
            } else {
                if (pass) {
                    m = te::gfx::ShaderView(material, pass);
                    if (!m.shader || !PassDrawable(m.shader->type, inst.eye.has_value())) continue;
                }
                state = te::gfx::StateOf(m);
                bucket = m.material ? m.bucket : BucketOpaque;

                lit = inst.lights && m.shader && (m.shader->type == 0x02 || m.shader->type == 0x10 || m.shader->type == 0x16 ||
                                                  (pass && EnvType(m.shader->type)));
                envMap = pass && m.shader && EnvType(m.shader->type) && inst.eye.has_value();
                if (m.shader && std::isfinite(m.shader->scroll[0]) && std::isfinite(m.shader->scroll[1])) scroll = {m.shader->scroll[0], m.shader->scroll[1]};
                screenCopy = m.shader && ScreenCopyType(m.shader->type);
                if (screenCopy) {

                    state.texture = PS2E_DRAW_TEXTURE_FRAME_COPY;
                    state.tfx = PS2E_DRAW_TFX_MODULATE;
                    state.tcc = 1;
                    state.wrap_u = state.wrap_v = PS2E_DRAW_WRAP_CLAMP;
                } else {
                    sub.scroll = scroll;
                }
            }

            BoxCorners corners;
            bool cornersMade = false;
            const auto inFront = [&] {
                if (!part.native) return false;
                CornersThrough(part, sub, corners, recordFast);
                cornersMade = true;
                return NativeInFront(part, corners);
            };
            if ((!view.clipped || inFront()) && !screenCopy && !envMap && !cpuPassFollows &&
                te::gfx::NativeMeshes()) {
                if (part.meshState == 0) {
                    part.meshState = 1;
                    g_meshRequests.emplace_back(rigid->model, i);
                }
                if (part.native) {
                    ps2e_draw_mesh_params mp{};
                    for (int r = 0; r < 4; ++r)
                        for (int c = 0; c < 4; ++c) mp.screen[r * 4 + c] = sub.screen[size_t(r)][size_t(c)];
                    mp.offset[0] = te::gfx::GsOriginX();
                    mp.offset[1] = te::gfx::GsOriginY();
                    mp.scroll[0] = sub.scroll[0];
                    mp.scroll[1] = sub.scroll[1];
                    {

                        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
                        if (!cornersMade) CornersThrough(part, sub, corners, recordFast);
                        for (const auto& o : corners.o) {
                            if (!(o[3] > 0.f)) { x0 = y0 = -1e30f; x1 = y1 = 1e30f; break; }
                            const float r = 0.0625f / o[3];
                            const float sx = o[0] * r - mp.offset[0], sy = o[1] * r - mp.offset[1];
                            x0 = std::min(x0, sx); y0 = std::min(y0, sy); x1 = std::max(x1, sx); y1 = std::max(y1, sy);
                        }
                        mp.box[0] = std::max(x0 - 1.f, -1e9f);
                        mp.box[1] = std::max(y0 - 1.f, -1e9f);
                        mp.box[2] = std::min(x1 + 1.f, 1e9f);
                        mp.box[3] = std::min(y1 + 1.f, 1e9f);
                    }
                    if (lit) {
                        mp.lit = 1;
                        const Lights& L = *inst.lights;
                        for (int l = 0; l < 3; ++l)
                            for (int k = 0; k < 3; ++k) {
                                mp.light_dir[l][k] = L.directions[size_t(l)][size_t(k)];
                                mp.light_colour[l][k] = L.colours[size_t(l)][size_t(k)];
                            }
                        for (int k = 0; k < 3; ++k) mp.ambient[k] = L.ambient[size_t(k)];
                    }
                    uint32_t mesh = part.native;
                    if (sub.scroll[0] == 0.f && sub.scroll[1] == 0.f) {
                        if (TrimEntry* trim = TrimFor(rigid->model, i, part, m, state)) {
                            if (trim->flagged) {
                                if (trim->flagged == trim->failing.size()) continue;
                                if (trim->meshState == 0) { trim->meshState = 1; g_trimRequests.push_back(trim); }
                                if (trim->mesh) mesh = trim->mesh;
                            }
                        }
                    }
                    te::gfx::RecordMesh(bucket, state, mesh, mp, part.nativeBox);
                    continue;
                }
            }

            const bool deferred = !screenCopy && Deferrable();
            const std::array<uint8_t, 4>* colours = nullptr;
            if (lit && !deferred) {
                LitColours(part, *inst.lights, g_colourScratch);
                colours = g_colourScratch.data();
            }
            if (screenCopy) {
                if (!PrepareScreenCopyMesh(part, view.clip, m.shader->extra[0])) continue;
                emitter.BeginScreenCopy(bucket, state, scroll);
            } else {
                if (envMap && !deferred && !PrepareEnvMesh(part, view.clip, *inst.eye, g_screenCopyScratch)) continue;
                emitter.Begin(bucket, state);
            }
            if (deferred) {
                te::gfx::DeferredBuild& build = te::gfx::ReserveDeferred(bucket, emitter.state);
                std::optional<Lights> lights;
                if (lit) lights = *inst.lights;

                std::optional<Vector> envEye;
                if (envMap) envEye = *inst.eye;
                te::gfx::PostBuild(build, [&build, mesh = &part, sub, lights, envEye, clip = view.clip, state = BuildState(),
                                           textured = emitter.textured, model = rigid->model, i, pass] {
                    thread_local std::vector<std::array<uint8_t, 4>> litScratch;
                    thread_local ScreenCopyScratch envScratch;
                    RigidMemo* memo = nullptr;
                    te::r3::Probe probe(build, envEye ? te::r3::Env : te::r3::Rigid);
                    if (mesh->revision) {
                        if (!build.rigid_memo) build.rigid_memo = std::make_shared<RigidMemo>();
                        memo = static_cast<RigidMemo*>(build.rigid_memo.get());
                        if (memo->Matches(mesh->revision, sub, lights, state, textured, envEye, envEye ? &clip : nullptr)) {
                            memo->misses = 0;
                            memo->Load(build);
                            mesh->hint.Store(build.verts.size());
                            if (probe.TypedHit()) return;
                        }
                    }
                    if (envEye && !PrepareEnvMesh(*mesh, clip, *envEye, envScratch)) return;
                    if (lights) LitColours(*mesh, *lights, litScratch);
                    ReserveVertices(build, mesh->hint);
                    Collector collector{&build, textured};
                    const Geometry::Result r = te::gfx::MeshBuild(envEye ? envScratch.prepared : mesh->prepared, sub, state,
                                                                  lights ? litScratch.data() : nullptr, &Collector::Sink, &Collector::Body,
                                                                  &collector);
                    collector.Done(mesh->hint);
                    if (memo && !probe.HasHit() && r == Geometry::Result::Ok)
                        memo->Store(mesh->revision, sub, lights, state, textured, build, kMemoEntryLimit, envEye, envEye ? &clip : nullptr);
                });
                continue;
            }
            Geometry::Result result;
            if (screenCopy || envMap) result = te::gfx::MeshBuild(g_screenCopyScratch.prepared, sub, BuildState(), colours, &Emitter::Sink, &emitter);
            else emitter.Build(part, sub, colours, &result);
            if (result != Geometry::Result::Ok) continue;
        }
    }
}

struct Culling {
    std::optional<title::CullingMath::View> view;
    std::optional<title::CullingMath::Link> link;
    std::optional<title::CullingMath::Frustum> planes;
    std::optional<title::CullingMath::Model> model;
    std::optional<title::CullingMath::Classification> cell;
    std::optional<title::CullingMath::BoxView> box;
    Matrix toScreen = Identity(), toClip = Identity();
} g_cull;

std::optional<title::CullingPlanes::Planes> ReadPlanes(uint32_t address) {
    const float* p = reinterpret_cast<const float*>(te::gfx::RecordGuestPointer(address, 6 * 16));
    if (!p) return std::nullopt;
    title::CullingPlanes::Planes planes;
    for (int i = 0; i < 6; ++i) planes[i] = {p[i * 4], p[i * 4 + 1], p[i * 4 + 2], p[i * 4 + 3]};
    return planes;
}

}

extern "C" {

void ps2e_gfx2_model_loaded(ps2e_u32 model, ps2e_s32 index, ps2e_s32 count, ps2e_u32 packet, ps2e_s32 size, ps2e_s32 vertices) {
    te::gfx::DrainRecordLane();
    const uint8_t* bytes = size > 0 ? te::gfx::RecordGuestPointer(packet, size_t(size)) : nullptr;
    LoadModelPart(model, index, count, bytes, size, vertices);
}

void ps2e_gfx2_model_released(ps2e_u32 model) {
    te::gfx::DrainRecordLane();
    te::gfx::DrainBuilds();
    if (auto it = g_models.find(model); it != g_models.end())
        for (Part& part : it->second.parts) RetireMesh(part);
    InvalidateModelLookup();
    g_models.erase(model);
    if (auto it = g_shadowModels.find(model); it != g_shadowModels.end()) RetireGpuShadow(it->second);
    g_shadowModels.erase(model);
}

}

namespace {
void LoadModelPart(uint32_t model, int32_t index, int32_t count, const uint8_t* bytes, int32_t size, int32_t vertices) {
    if (index < 0 || count <= 0 || index >= count || count > 4096) return;

    const auto loaded = g_models.find(model);
    if (loaded != g_models.end() &&
        (loaded->second.parts.size() != size_t(count) || loaded->second.parts[size_t(index)].decoded))
        te::gfx::DrainBuilds();
    InvalidateModelLookup();
    Model& m = g_models[model];
    if (m.parts.size() != size_t(count)) {
        for (Part& old : m.parts) RetireMesh(old);
        m.parts.assign(size_t(count), Part{});
        m.sources.assign(size_t(count), PartSource{});
    }
    PartSource& source = m.sources[size_t(index)];
    source.vertices = vertices;
    if (bytes && size > 0 && source.packet.data() != bytes) source.packet.assign(bytes, bytes + size);
    Part& part = m.parts[size_t(index)];
    RetireMesh(part);
    part = Part{};
    std::string why;
    if (!bytes || !DecodePacket(bytes, size_t(size), uint32_t(vertices), part, &why)) return;

    const Geometry::Result prepared = te::gfx::MeshPrepare(part.mesh, part.prepared);
    if (prepared != Geometry::Result::Ok) {
        part.decoded = false;
        return;
    }
    part.revision = ++g_part_revision;
}
}

namespace te::gfx {
void ResetDrawOrdinals() { g_drawOrdinals.clear(); }

bool CurrentCamera(ps2r::ps2e::InterpCamera& out, uint32_t* camera_object) {
    const RenderView* view = CurrentView();
    if (!view) return false;
    const Matrix cam = ToMatrix(view->toCamera), c2s = ToMatrix(view->cameraToScreen);
    if (!Finite(cam) || !Finite(c2s)) return false;
    Flat(cam, out.to_camera);
    Flat(c2s, out.camera_to_screen);
    out.valid = true;
    if (camera_object) *camera_object = view->cameraObject;
    return true;
}
void FlushGpuSkins();
void FlushTrimMeshes() {
    if (g_trimRequests.empty()) return;
    std::vector<MeshRequest> requests;
    for (TrimEntry* e : g_trimRequests) {
        MeshRequest r;
        for (size_t t = 0; t + 2 < e->triangles.size(); t += 3)
            if (!e->failing[t / 3]) r.vertices.insert(r.vertices.end(), e->triangles.begin() + long(t), e->triangles.begin() + long(t) + 3);
        requests.push_back(std::move(r));
    }
    CreateMeshes(requests);
    for (size_t k = 0; k < requests.size(); ++k) { g_trimRequests[k]->mesh = requests[k].handle; g_trimRequests[k]->meshState = 2; }
    g_trimRequests.clear();
}
void FlushNativeMeshes() {
    FlushGpuSkins();
    FlushTrimMeshes();
    if (g_meshRequests.empty()) return;
    std::vector<MeshRequest> requests;
    std::vector<Part*> parts;
    for (const auto& [model, index] : g_meshRequests) {
        auto it = g_models.find(model);
        if (it == g_models.end() || index >= it->second.parts.size()) continue;
        Part& part = it->second.parts[index];
        if (part.meshState != 1 || !part.decoded) continue;
        part.meshState = 2;
        MeshRequest r;
        r.vertices = MeshVertices(part);
        if (r.vertices.size() < 3) continue;
        std::array<float, 6> box{1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f};
        for (const auto& v : r.vertices) {
            box[0] = std::min(box[0], v.x); box[1] = std::min(box[1], v.y); box[2] = std::min(box[2], v.z);
            box[3] = std::max(box[3], v.x); box[4] = std::max(box[4], v.y); box[5] = std::max(box[5], v.z);
        }
        part.nativeBox = box;
        requests.push_back(std::move(r));
        parts.push_back(&part);
    }
    g_meshRequests.clear();
    CreateMeshes(requests);
    for (size_t k = 0; k < parts.size(); ++k) parts[k]->native = requests[k].handle;
}
}

extern "C" {

void ps2e_graphics_draw_rigid_model(ps2e_u32 model, ps2e_u32 matrix, ps2e_u32 lights, ps2e_s32 mode) {
    const uint32_t record_args[] = {model, matrix, lights, uint32_t(mode)};
    if (te::gfx::SceneSkipNow()) return;
    if (te::gfx::CaptureRecord(te::gfx::RecordCall::Rigid, record_args)) return;
    const auto* rigid = Guest<te_gfx_rigid_model>(model);
    const auto world = ReadMatrix(matrix);
    const RenderView* view = CurrentView();
    if (!rigid || !world || !view || !te::gfx::EnsureDevice()) return;
    Geometry::View v;
    v.screen = Multiply(*world, ToMatrix(view->toScreen));
    v.clip = Multiply(*world, ToMatrix(view->toClip));
    v.guard = {view->clip[0], view->clip[1], view->clip[2], view->clip[3]};
    v.scroll = {0.f, 0.f};
    v.clipped = uint32_t(mode) != DrawUnclipped;
    if (!Finite(v.screen) || !Finite(v.clip)) return;
    Lights l{};
    bool haveLights = false;
    if (const auto* ml = Guest<ModelLights>(lights)) {
        const float* dirs = reinterpret_cast<const float*>(te::gfx::RecordGuestPointer(ml->directions, 48));
        const float* cols = reinterpret_cast<const float*>(te::gfx::RecordGuestPointer(ml->colours, 48));
        const float* amb = reinterpret_cast<const float*>(te::gfx::RecordGuestPointer(ml->ambient, 16));
        if (dirs && cols && amb) {
            haveLights = true;
            for (int i = 0; i < 3; ++i) {

                for (int k = 0; k < 3; ++k) {
                    float s = 0.f;
                    for (int r = 0; r < 3; ++r) s += (*world)[k][r] * dirs[i * 4 + r];
                    l.directions[i][k] = s;
                }
                l.directions[i][3] = 0.f;
                l.colours[i] = {cols[i * 4], cols[i * 4 + 1], cols[i * 4 + 2], cols[i * 4 + 3]};
            }
            l.ambient = {amb[0], amb[1], amb[2], amb[3]};
        }
    }
    InstanceState inst;
    inst.lights = haveLights ? &l : nullptr;
    if (rigid->needsEye) inst.eye = EyeIn(*world, view->toCamera);

    if (te::gfx::Interpolating())
        if (const auto w = WorldOfScreen(v.screen)) TagRigid(OrdinalId(TagId(model, 0)), *w, v.screen);
    DrawInstance(rigid, v, inst);
    te::gfx::ClearDrawTag();
}

void ps2e_graphics_draw_placed_model(ps2e_u32 model, ps2e_u32 viewAddress, ps2e_u32 world) {
    const uint32_t record_args[] = {model, viewAddress, world};
    if (te::gfx::SceneSkipNow()) return;
    if (te::gfx::CaptureRecord(te::gfx::RecordCall::Placed, record_args)) return;
    const auto* rigid = Guest<te_gfx_rigid_model>(model);
    const auto* cv = Guest<ChunkView>(viewAddress);
    if (!rigid || !cv || !te::gfx::EnsureDevice()) return;
    Geometry::View v;
    v.screen = ToMatrix(cv->toScreen);
    v.clip = ToMatrix(cv->clipped);
    v.guard = {cv->clip[0], cv->clip[1], cv->clip[2], cv->clip[3]};
    v.scroll = {0.f, 0.f};
    v.clipped = cv->visibility != DrawUnclipped;
    if (!Finite(v.screen) || !Finite(v.clip)) return;
    InstanceState inst;
    if (rigid->needsEye) {
        const auto w = ReadMatrix(world);
        const RenderView* view = CurrentView();
        if (w && view) inst.eye = EyeIn(*w, view->toCamera);
    }

    if (te::gfx::Interpolating())
        if (const auto w = WorldOfScreen(v.screen)) TagRigid(OrdinalId(TagId(model, viewAddress)), *w, v.screen);
    DrawInstance(rigid, v, inst);
    te::gfx::ClearDrawTag();
}

void ps2e_gfx2_draw_sky(ps2e_u32 skyAddress, ps2e_u32 viewAddress, ps2e_u32 cameraAddress) {
    te::gfx::DrainRecordLane();
    te::gfx::SceneSkipDecide();
    if (te::gfx::SceneSkipNow()) return;
    const auto* sky = Guest<te_gfx_sky>(skyAddress);
    const auto* view = Guest<RenderView>(viewAddress);
    const auto camera = ReadVector(cameraAddress);
    const auto* target = te::gfx::CurrentTarget();
    if (!sky || !view || !camera || !te::gfx::EnsureDevice()) return;
    if (sky->count == 0 || sky->count > 64) return;
    const uint32_t* models = reinterpret_cast<const uint32_t*>(te::gfx::RecordGuestPointer(sky->models, 4 * size_t(sky->count)));
    if (!models) return;

    Matrix clipToScreen = target ? ToMatrix(target->clipToScreen) : Identity();
    if (!target) {
        const float w = float(te::gfx::ScreenW()), h = float(te::gfx::ScreenH());
        clipToScreen = Matrix{{{w * 8.f, 0, 0, 0}, {0, -h * 8.f, 0, 0}, {0, 0, -8388607.f, 0}, {2048.f * 16.f, 2048.f * 16.f, 8388607.f, 1.f}}};
    }
    const Matrix translation = Translation(*camera);
    const Matrix toClip = Multiply(translation, ToMatrix(view->toClip));
    const Matrix toScreen = Multiply(toClip, clipToScreen);
    Geometry::View v;
    v.screen = toScreen;
    v.clip = toClip;
    v.guard = {view->clip[0], view->clip[1], view->clip[2], view->clip[3]};
    v.scroll = {0.f, 0.f};
    v.clipped = true;
    if (!Finite(v.screen) || !Finite(v.clip)) return;
    for (uint32_t i = 0; i < sky->count; ++i) {
        const auto* rigid = Guest<te_gfx_rigid_model>(models[i]);
        if (!rigid) continue;

        if (te::gfx::Interpolating())
            if (const auto w = WorldOfScreen(toScreen)) TagRigid(TagId(skyAddress, models[i]), *w, toScreen);

        const te_gfx_model* data = rigid->model ? Guest<te_gfx_model>(rigid->model) : nullptr;
        auto it = g_models.find(rigid->model);
        if (!data || it == g_models.end()) continue;
        const uint32_t count = std::min<uint32_t>(rigid->count, data->subModelCount);
        const uint32_t* materials = count ? reinterpret_cast<const uint32_t*>(te::gfx::RecordGuestPointer(rigid->materials, 4 * size_t(count))) : nullptr;
        static Emitter emitter;
        for (uint32_t p = 0; p < count && p < it->second.parts.size(); ++p) {
            const Part& part = it->second.parts[p];
            if (!part.decoded) continue;
            const uint32_t resource = materials ? materials[p] : 0;
            const uint32_t* materialAddress = resource ? Guest<uint32_t>(resource + 8) : nullptr;
            const MaterialView m = te::gfx::ReadMaterial(materialAddress ? *materialAddress : 0);
            ps2e_draw_state state = te::gfx::StateOf(m);
            state.depth_test = PS2E_DRAW_DEPTH_ALWAYS;
            state.depth_write = 0;
            emitter.Begin(BucketSky, state);
            Geometry::View sub = v;
            if (m.shader && std::isfinite(m.shader->scroll[0]) && std::isfinite(m.shader->scroll[1])) sub.scroll = {m.shader->scroll[0], m.shader->scroll[1]};
            if (Deferrable()) {

                te::gfx::DeferredBuild& build = te::gfx::ReserveDeferred(BucketSky, emitter.state);
                te::gfx::PostBuild(build, [&build, mesh = &part, sub, state = BuildState(), textured = emitter.textured] {
                    te::r3::Probe probe(build,te::r3::Sky);

                    RigidMemo* memo = nullptr;
                    if (mesh->revision) {
                        if (!build.rigid_memo) build.rigid_memo = std::make_shared<RigidMemo>();
                        memo = static_cast<RigidMemo*>(build.rigid_memo.get());
                        if (memo->Matches(mesh->revision, sub, std::nullopt, state, textured)) {
                            memo->misses = 0;
                            memo->Load(build);
                            if (probe.TypedHit()) return;
                        }
                    }
                    Collector collector{&build, textured};
                    if (te::gfx::MeshBuild(mesh->prepared, sub, state, nullptr, &Collector::Sink, &Collector::Body, &collector) == Geometry::Result::Ok) {
                        if (memo && !probe.HasHit()) memo->Store(mesh->revision, sub, std::nullopt, state, textured, build);
                    }
                });
                continue;
            }
            Geometry::Result result;
            emitter.Build(part, sub, nullptr, &result);
            if (result != Geometry::Result::Ok) continue;
        }
    }
    te::gfx::ClearDrawTag();

    ps2e_draw_state clear{};
    clear.depth_test = PS2E_DRAW_DEPTH_ALWAYS;
    clear.depth_write = 1;
    clear.no_colour_write = 1;
    const float w = float(te::gfx::ScreenW()), h = float(te::gfx::ScreenH());
    auto V = [](float x, float y) {
        ps2e_draw_vertex o{};
        o.x = x; o.y = y; o.z = 0.f; o.a = 0x80;
        return o;
    };
    Draw d;
    d.state = clear;
    d.verts = {V(0, 0), V(w, 0), V(0, h), V(w, 0), V(w, h), V(0, h)};
    te::gfx::Record(BucketSky, std::move(d));
}

}

namespace {

constexpr uint8_t kWindingUp = 8, kWindingDown = 248;
constexpr uint8_t kShadowMask = 0x84;
constexpr uint8_t kShadowFix = 0x20;

void SetStencil(ps2e_draw_state& st, int value) {
    st.stencil = value;
}

struct ShadowEmitter {
    ps2e_draw_state up{}, down{};
    size_t triangles = 0;
    void Begin() {
        up = ps2e_draw_state{};
        up.gouraud = 0;
        up.depth_test = PS2E_DRAW_DEPTH_GEQUAL;
        up.depth_write = 0;
        up.no_colour_write = 1;
        up.tcc = 1;

        up.dest_alpha_test = 1;
        down = up;
        SetStencil(up, PS2E_DRAW_STENCIL_COUNT_UP);
        SetStencil(down, PS2E_DRAW_STENCIL_COUNT_DOWN);
    }

    static void Convert(const ps2r::gs::VertexRec* verts, size_t count, uint8_t colour, ps2e_draw_vertex* out) {
        for (size_t i = 0; i < count; ++i) {
            const auto& v = verts[i];
            ps2e_draw_vertex& o = out[i];
            o = ps2e_draw_vertex{};
            o.x = v.x;
            o.y = v.y;
            o.z = v.z > 0.f ? 4294967296.f - v.z : 0.f;
            o.r = o.g = o.b = colour;
            o.a = 0x80;
        }
    }
    void Append(const ps2r::gs::VertexRec* verts, size_t count) {
        count = count / 3 * 3;
        if (!count) return;
        const uint8_t colour = verts[0].r;
        const ps2e_draw_state& state = colour == kWindingDown ? down : up;
        Draw& d = te::gfx::OpenDraw(BucketShadows, state, false, true);
        const size_t base = d.verts.size();
        d.verts.resize(base + count);
        Convert(verts, count, colour, d.verts.data() + base);
        te::gfx::CloseDraw(BucketShadows);
        triangles += count / 3;
    }
    static void Sink(void* user, const ps2r::gs::VertexRec* verts, size_t count, bool fan) {
        (void)fan;
        static_cast<ShadowEmitter*>(user)->Append(verts, count);
    }
};
ShadowEmitter g_shadowEmitter;
bool g_shadowsOpen = false;

ShadowModel& ShadowModelOf(uint32_t model, const Model& m) {
    ShadowModel& sm = g_shadowModels[model];
    if (sm.tried) return sm;
    sm.tried = true;
    sm.parts.resize(m.sources.size());
    sm.decoded.assign(m.sources.size(), false);
    sm.hints.resize(m.sources.size());
    sm.gpuState.assign(m.sources.size(), 0);
    sm.gpu.assign(m.sources.size(), 0);
    sm.gpuBox.assign(m.sources.size(), {});
    for (size_t i = 0; i < m.sources.size(); ++i) {
        const PartSource& src = m.sources[i];
        if (src.packet.empty()) continue;
        const auto r = title::ShadowGeometry::Decode(src.packet.data(), src.packet.size(), uint32_t(src.vertices), sm.parts[i]);
        sm.decoded[i] = r == title::ShadowGeometry::Result::Ok;
    }
    return sm;
}
size_t g_shadowVolumes = 0;

void ShadowQuad(const ps2e_draw_state& state, uint8_t colour, uint8_t alpha) {
    const float w = float(te::gfx::ScreenW()), h = float(te::gfx::ScreenH());
    auto V = [&](float x, float y) {
        ps2e_draw_vertex o{};
        o.x = x; o.y = y; o.z = 0.f;
        o.r = o.g = o.b = colour; o.a = alpha;
        return o;
    };
    Draw d;
    d.state = state;
    d.verts = {V(0, 0), V(w, 0), V(0, h), V(w, 0), V(w, h), V(0, h)};
    te::gfx::Record(BucketShadows, std::move(d));
}
}

extern "C" {

void ps2e_graphics_set_up_shadow_pass(void) {
    te::gfx::DrainRecordLane(); }

void ps2e_graphics_begin_shadows(void) {
    te::gfx::DrainRecordLane();
    const RenderView* view = CurrentView();
    const auto* target = te::gfx::CurrentTarget();
    if (!view) return;
    const Matrix toCamera = ToMatrix(view->toClip);
    const Matrix toScreen = Multiply(toCamera, target ? ToMatrix(target->clipToScreen) : Identity());
    WriteMatrix(kShadowToCamera, toCamera);
    WriteMatrix(kShadowToScreen, toScreen);
    g_shadowsOpen = true;
    g_shadowVolumes = 0;
    g_shadowEmitter.Begin();
    if (te::gfx::EnsureDevice()) {

        ps2e_draw_state clear{};
        clear.depth_test = PS2E_DRAW_DEPTH_ALWAYS;
        clear.depth_write = 0;
        clear.no_colour_write = 1;
        SetStencil(clear, PS2E_DRAW_STENCIL_CLEAR);
        ShadowQuad(clear, 0, 0);
    }
}

void ps2e_graphics_end_shadows(void) {
    te::gfx::DrainRecordLane();
    if (!g_shadowsOpen) return;
    g_shadowsOpen = false;
    if (!g_shadowVolumes || !te::gfx::EnsureDevice()) return;

    ps2e_draw_state composite{};
    composite.gouraud = 0;
    composite.depth_test = PS2E_DRAW_DEPTH_ALWAYS;
    composite.depth_write = 0;
    composite.blend = 1;
    composite.blend_a = 2; composite.blend_b = 0; composite.blend_c = 2; composite.blend_d = 1;
    composite.blend_fix = kShadowFix;
    composite.tcc = 1;
    SetStencil(composite, PS2E_DRAW_STENCIL_INSIDE);
    ShadowQuad(composite, kShadowMask, 0x80);
}

void ps2e_graphics_draw_shadow_mesh(ps2e_u32 mesh, ps2e_u32 toScreen, ps2e_u32 toCamera, ps2e_u32 world) {
    te::gfx::DrainRecordLane();
    (void)world;
    const auto* rigid = Guest<te_gfx_rigid_model>(mesh);
    const auto screen = ReadMatrix(toScreen);
    const auto camera = ReadMatrix(toCamera);
    const RenderView* view = CurrentView();
    if (!rigid || !screen || !camera || !view || !te::gfx::EnsureDevice()) return;
    if (te::gfx::SceneSkipNow()) return;
    Geometry::View v;
    v.screen = *screen;
    v.clip = *camera;
    v.guard = {view->clip[0], view->clip[1], view->clip[2], view->clip[3]};
    v.scroll = {0.f, 0.f};
    v.clipped = true;
    if (!Finite(v.screen) || !Finite(v.clip)) return;
    const te_gfx_model* data = rigid->model ? Guest<te_gfx_model>(rigid->model) : nullptr;
    auto it = g_models.find(rigid->model);
    if (!data || it == g_models.end()) return;
    if (!g_shadowsOpen) g_shadowEmitter.Begin();
    const uint32_t count = std::min<uint32_t>(rigid->count ? rigid->count : data->subModelCount, data->subModelCount);
    const size_t before = g_shadowEmitter.triangles;
    ShadowModel& sm = ShadowModelOf(rigid->model, it->second);
    title::ShadowGeometry::View sv;
    sv.screen = v.screen;
    sv.clip = v.clip;
    sv.guard = v.guard;
    sv.offset = {te::gfx::GsOriginX(), te::gfx::GsOriginY()};

    if (te::gfx::Interpolating())
        if (const auto w = WorldOfScreen(*screen)) TagRigid(OrdinalId(TagId(mesh, 0)), *w, *screen);
    if (Deferrable()) {

        bool posted = false;
        for (uint32_t p = 0; p < count && p < sm.parts.size(); ++p) {
            if (!sm.decoded[p]) continue;

            if (te::gfx::GpuShadows() && p < sm.gpu.size()) {
                if (sm.gpuState[p] == 0) {
                    sm.gpuState[p] = 1;
                    g_shadowRequests.emplace_back(rigid->model, p);
                }
                if (sm.gpu[p] && title::ShadowGeometry::InsideGuard(sm.gpuBox[p], sv)) {
                    ps2e_draw_mesh_params mp{};
                    for (int r = 0; r < 4; ++r)
                        for (int c = 0; c < 4; ++c) mp.screen[r * 4 + c] = sv.screen[size_t(r)][size_t(c)];
                    mp.offset[0] = sv.offset[0];
                    mp.offset[1] = sv.offset[1];
                    mp.lit = 2;
                    mp.box[0] = mp.box[1] = -1e9f;
                    mp.box[2] = mp.box[3] = 1e9f;
                    mp.cull = 1;
                    te::gfx::RecordMesh(BucketShadows, g_shadowEmitter.up, sm.gpu[p], mp, sm.gpuBox[p]);
                    mp.cull = 2;
                    te::gfx::RecordMesh(BucketShadows, g_shadowEmitter.down, sm.gpu[p], mp, sm.gpuBox[p]);
                    posted = true;
                    continue;
                }
            }
            te::gfx::DeferredBuild& build = te::gfx::ReserveDeferred(BucketShadows, g_shadowEmitter.up);
            build.alt_state = g_shadowEmitter.down;
            te::gfx::PostBuild(build, [&build, part = &sm.parts[p], hint = &sm.hints[p], sv, mesh, revision = sm.revision, p] {
                te::r3::Probe probe(build,te::r3::Shadow);
                if(probe.Active()){te::r3::Key k;k.Add(revision);k.Add(p);k.Add(sv.screen);k.Add(sv.clip);k.Add(sv.guard);k.Add(sv.offset);
                    if(probe.Match(std::move(k))){hint->Store(build.verts.size());return;}}

                struct Volumes {
                    te::gfx::DeferredBuild& build;
                    static void Sink(void* user, const ps2r::gs::VertexRec* verts, size_t count, bool) {
                        Volumes& to = *static_cast<Volumes*>(user);
                        const size_t n = count / 3 * 3;
                        if (!n) return;
                        const uint8_t colour = verts[0].r;
                        const size_t base = to.build.verts.size();
                        if (to.build.verts.capacity() < base + n) to.build.verts.reserve(std::max(base + n, base * 2));
                        to.build.verts.resize(base + n);
                        ShadowEmitter::Convert(verts, n, colour, to.build.verts.data() + base);
                        to.build.segments.push_back(uint32_t(n));
                        to.build.alternate.push_back(colour == kWindingDown ? 1 : 0);
                    }
                } volumes{build};
                ReserveVertices(build, *hint);
                const size_t verts0 = build.verts.size(), segments0 = build.segments.size(), alternate0 = build.alternate.size();
                const auto r = title::ShadowGeometry::Build(*part, &sv, 1, &Volumes::Sink, &volumes);
                if (r != title::ShadowGeometry::Result::Ok) {
                    build.verts.resize(verts0);
                    build.segments.resize(segments0);
                    build.alternate.resize(alternate0);
                    return;
                }
                hint->Store(build.verts.size() - verts0);
            });
            posted = true;
        }
        te::gfx::ClearDrawTag();
        if (posted) g_shadowVolumes++;
        return;
    }
    static std::vector<ps2r::gs::DrawIr> volumes;
    for (uint32_t p = 0; p < count && p < sm.parts.size(); ++p) {
        if (!sm.decoded[p]) continue;
        volumes.clear();
        const auto r = title::ShadowGeometry::Build(sm.parts[p], {sv}, volumes);
        if (r != title::ShadowGeometry::Result::Ok) continue;
        for (const auto& d : volumes) g_shadowEmitter.Append(d.verts.data(), d.verts.size());
    }
    te::gfx::ClearDrawTag();
    const size_t triangles = g_shadowEmitter.triangles - before;
    if (triangles) g_shadowVolumes++;
}

void ps2e_graphics_load_chunk_planes(ps2e_u32 place) {
    const auto matrix = ReadMatrix(place);
    const auto view = ReadPlanes(kViewPlanes), far = ReadPlanes(kFarViewPlanes);
    if (!matrix || !view || !far) return;
    g_cull.planes = title::CullingPlanes::InChunk(*view, *far, *matrix);
    te::cullincremental::Frustum(place, 0, *g_cull.planes);
    g_cull.cell.reset();
    g_cull.box.reset();
}
void ps2e_graphics_load_portal_planes(ps2e_u32 place, ps2e_u32 portal) {
    const auto matrix = ReadMatrix(place);
    const auto view = ReadPlanes(kViewPlanes), far = ReadPlanes(kFarViewPlanes), sides = ReadPlanes(portal);
    if (!matrix || !view || !far || !sides) return;
    g_cull.planes = title::CullingPlanes::ThroughPortal(*view, *far, *matrix, *sides);
    te::cullincremental::Frustum(place, portal, *g_cull.planes);
    g_cull.cell.reset();
    g_cull.box.reset();
}

void ps2e_graphics_load_culling_view(ps2e_u32 toClip, ps2e_u32 toScreen, ps2e_u32 camera) {
    const auto clip = ReadMatrix(toClip), screen = ReadMatrix(toScreen);
    const auto eye = ReadVector(camera);
    if (!clip || !screen || !eye) return;
    g_cull.view = title::CullingMath::View{*clip, *screen, *eye};
    g_cull.link = title::CullingMath::LoadLink(*g_cull.view, Identity(), Identity());
    te::cullincremental::Link(toClip, camera, *g_cull.link);
    g_cull.model.reset();
    g_cull.cell.reset();
    g_cull.box.reset();
}
void ps2e_graphics_cull_load_link(ps2e_u32 chunk, ps2e_u32 object) {
    const auto c = ReadMatrix(chunk), o = ReadMatrix(object);
    if (!c || !o || !g_cull.view) return;
    g_cull.link = title::CullingMath::LoadLink(*g_cull.view, *c, *o);
    te::cullincremental::Link(chunk, object, *g_cull.link);
}
void ps2e_graphics_cull_load_model(ps2e_u32 model) {
    const auto m = ReadMatrix(model);
    if (!m || !g_cull.link) return;
    g_cull.model = te::cullincremental::LoadModel(model, *g_cull.link, *m);
    g_cull.toScreen = g_cull.model->to_screen;
    g_cull.box.reset();
    g_cull.cell.reset();
}
void ps2e_graphics_cull_test_box(ps2e_u32 corners) {
    const float* c = reinterpret_cast<const float*>(te::gfx::RecordGuestPointer(corners, 32));
    if (!c || !g_cull.planes) return;
    title::CullingMath::Box box{{c[0], c[1], c[2], c[3]}, {c[4], c[5], c[6], c[7]}};
    g_cull.cell = te::cullincremental::TestBox(corners, *g_cull.planes, box);
    g_cull.box.reset();
}
void ps2e_graphics_cull_test_box_at(ps2e_u32 boxAddress, ps2e_u32 model) {
    const float* c = reinterpret_cast<const float*>(te::gfx::RecordGuestPointer(boxAddress, 32));
    const auto m = ReadMatrix(model);
    if (!c || !m || !g_cull.planes || !g_cull.link) return;
    title::CullingMath::Box box{{c[0], c[1], c[2], c[3]}, {c[4], c[5], c[6], c[7]}};
    g_cull.box = te::cullincremental::TestBoxAt(boxAddress, model, *g_cull.planes, *g_cull.link, box, *m);
    g_cull.cell.reset();
    if (g_cull.box->to_screen) g_cull.toScreen = *g_cull.box->to_screen;
    if (g_cull.box->to_clip) g_cull.toClip = *g_cull.box->to_clip;
}

void ps2e_graphics_cull_result(ps2e_u32 out) {
    uint32_t* o = reinterpret_cast<uint32_t*>(te::gfx::RecordGuestPointer(out, 12));
    if (!o) return;
    uint32_t outside = 0, clipped = 0;
    int32_t distance = g_cull.model ? g_cull.model->distance : 0;
    if (g_cull.box) {
        outside = g_cull.box->visibility.outside;
        clipped = g_cull.box->visibility.clipped;
        distance = g_cull.box->distance;
    } else if (g_cull.cell) {
        outside = g_cull.cell->outside;
        clipped = g_cull.cell->clipped;
    } else if (!g_cull.view || !g_cull.planes) {
        outside = 1;
    }
    o[0] = outside;
    o[1] = clipped;
    o[2] = uint32_t(distance);
}
void ps2e_graphics_cull_matrices(ps2e_u32 clipped, ps2e_u32 toScreen) {
    if (clipped) WriteMatrix(clipped, g_cull.toClip);
    if (toScreen) WriteMatrix(toScreen, g_cull.toScreen);
}

void ps2e_graphics_clip_box(ps2e_u32 view, ps2e_u32 boxAddress, ps2e_u32 flags) {
    const auto matrix = ReadMatrix(view);
    const auto guard = ReadVector(view + 0x40);
    const float* c = reinterpret_cast<const float*>(te::gfx::RecordGuestPointer(boxAddress, 32));
    uint32_t* f = reinterpret_cast<uint32_t*>(te::gfx::RecordGuestPointer(flags, 8));
    if (!f) return;
    if (!matrix || !guard || !c) { f[0] = 0x3F; f[1] = 0x3F; return; }
    title::BoundsMath::Box box{{c[0], c[1], c[2], c[3]}, {c[4], c[5], c[6], c[7]}};
    const auto result = title::ClippingMath::ClipBox(box, *matrix, *guard);
    f[0] = result.any;
    f[1] = result.common;
}
void ps2e_graphics_clip_box_at(ps2e_u32 view, ps2e_u32 boxAddress, ps2e_u32 flags, ps2e_u32 model) {
    const auto matrix = ReadMatrix(view);
    const auto guard = ReadVector(view + 0x40);
    const auto m = ReadMatrix(model);
    const float* c = reinterpret_cast<const float*>(te::gfx::RecordGuestPointer(boxAddress, 32));
    uint32_t* f = reinterpret_cast<uint32_t*>(te::gfx::RecordGuestPointer(flags, 8));
    if (!f) return;
    if (!matrix || !guard || !c || !m) { f[0] = 0x3F; f[1] = 0x3F; return; }
    title::BoundsMath::Box box{{c[0], c[1], c[2], c[3]}, {c[4], c[5], c[6], c[7]}};
    const auto result = title::ClippingMath::ClipBox(box, *matrix, *guard, &*m);
    f[0] = result.any;
    f[1] = result.common;
}

}

namespace {
using SkinGeo = title::SkinGeometry;
uint64_t g_skin_revision = 0;
struct SkinBounds {

    struct BoneBound { uint32_t at; float centre[3]; float radius; };
    std::vector<BoneBound> boneRadius;
    float weightSum = 0.f, weightMin = 0.f;
    bool boundMade = false;
};
struct SkinPart {
    uint64_t revision = 0;
    SkinGeo::Part part;
    BuildHint hint;
    bool decoded = false;

    uint8_t gpuState = 0;
    uint32_t gpu = 0;
    int32_t vertices = 0;
    std::vector<uint8_t> packet;
    SkinBounds bound;
};

void RetireGpuSkin(SkinPart& part) {
    if (part.gpu) te::gfx::RetireMesh(part.gpu);
    part.gpu = 0;
    part.gpuState = 0;
}

te::r3::Key SkinKey(uint64_t rev,const SkinGeo::Inputs& in,const ps2r::gs::DrawIr& st,bool tex) {
    te::r3::Key k;k.Add(rev);k.List(in.joints);k.Add(in.to_screen);k.Add(in.lights);k.Add(in.colours);k.Add(in.ambient);
    k.Add(in.clipped);k.Add(in.to_camera);k.Add(in.clip);k.Add(in.scroll);k.Add(st.off_x);k.Add(st.off_y);k.Add(tex);return k;
}
struct SkinMemo {
    RigidMemo payload;
    SkinGeo::Inputs input;
    uint64_t revision = 0;
    bool Matches(uint64_t rev, const SkinGeo::Inputs& in, const ps2r::gs::DrawIr& state, bool textured) const {
        if (revision != rev || input.joints.size() != in.joints.size() || input.clipped != in.clipped ||
            !payload.Matches(rev, Geometry::View{}, std::nullopt, state, textured)) return false;
        if (std::memcmp(input.joints.data(), in.joints.data(), in.joints.size()*sizeof(SkinGeo::Qw))) return false;
#define SKIN_SAME(member) if (std::memcmp(&input.member, &in.member, sizeof(in.member))) return false
        SKIN_SAME(to_screen); SKIN_SAME(lights); SKIN_SAME(colours); SKIN_SAME(ambient);
        SKIN_SAME(to_camera); SKIN_SAME(clip); SKIN_SAME(scroll);
#undef SKIN_SAME
        return true;
    }
    void Store(uint64_t rev, const SkinGeo::Inputs& in, const ps2r::gs::DrawIr& state, bool textured,
               const te::gfx::DeferredBuild& build) {
        payload.Store(rev, Geometry::View{}, std::nullopt, state, textured, build, 256*1024);
        if (payload.valid) {
            try { input = in; revision = rev; }
            catch (const std::bad_alloc&) { payload.Release(); }
        }
    }
};

struct SkinModel { std::vector<SkinPart> parts; };
std::unordered_map<uint32_t, SkinModel> g_skins;
struct BlendPartGeo { uint64_t revision=0; SkinGeo::Part part; bool decoded = false; bool tried = false; BuildHint hint; SkinBounds bound;
                      uint8_t gpuState = 0; uint32_t gpu = 0; };
std::unordered_map<uint32_t, std::unordered_map<uint32_t, BlendPartGeo>> g_blendSkins;
void RetireGpuSkins(SkinModel& m) { for (SkinPart& part : m.parts) RetireGpuSkin(part); }
void RetireGpuBlend(std::unordered_map<uint32_t, BlendPartGeo>& parts) {
    for (auto& [packet, geo] : parts) { if (geo.gpu) te::gfx::RetireMesh(geo.gpu); geo.gpu = 0; geo.gpuState = 0; }
}
void RetireAllGpuSkins() {
    for (auto& [address, m] : g_skins) RetireGpuSkins(m);
    for (auto& [address, parts] : g_blendSkins) RetireGpuBlend(parts);
}
std::vector<std::pair<uint32_t, uint32_t>> g_blendSkinRequests;

uint32_t Bits(float value) { uint32_t b; std::memcpy(&b, &value, 4); return b; }
SkinGeo::Qw RowQw(const Matrix& m, int r) { return {Bits(m[r][0]), Bits(m[r][1]), Bits(m[r][2]), Bits(m[r][3])}; }

struct PooledSkinInputs {
    SkinGeo::Inputs in;
    SkinGeo::BlendInputs blend;
};

struct SkinJointScratch {
    SkinGeo::Inputs& in;
    static std::vector<SkinGeo::Qw>& Saved() {static std::vector<SkinGeo::Qw> v;return v;}
    explicit SkinJointScratch(SkinGeo::Inputs& v):in(v) {in.joints.swap(Saved());}
    ~SkinJointScratch() {in.joints.swap(Saved());}
};

struct SkinShapeScratch {
    SkinGeo::BlendInputs& in;
    static std::vector<SkinGeo::Shape>& Saved() {static std::vector<SkinGeo::Shape> v;return v;}
    explicit SkinShapeScratch(SkinGeo::BlendInputs& v):in(v) {in.shapes.swap(Saved());}
    ~SkinShapeScratch(){in.shapes.swap(Saved());}
};

bool SkinInputs(uint32_t jointsAddress, int32_t jointCount, uint32_t matrixAddress, uint32_t lightsAddress, int32_t mode,
                SkinGeo::Inputs& in) {
    const RenderView* view = CurrentView();
    const auto world = ReadMatrix(matrixAddress);
    if (!view || !world || jointCount <= 0 || jointCount > int32_t(SkinGeo::kMaxJoints)) return false;
    const auto* joints = reinterpret_cast<const uint32_t*>(te::gfx::RecordGuestPointer(jointsAddress, 64 * size_t(jointCount)));
    if (!joints) return false;
    in.joints.resize(4 * size_t(jointCount));
    for (size_t q = 0; q < in.joints.size(); ++q)
        for (int k = 0; k < 4; ++k) in.joints[q][k] = joints[q * 4 + size_t(k)];
    const Matrix toScreen = Multiply(*world, ToMatrix(view->toScreen));
    if (!Finite(toScreen)) return false;
    for (int r = 0; r < 4; ++r) in.to_screen[r] = RowQw(toScreen, r);
    in.clipped = uint32_t(mode) != DrawUnclipped;
    if (in.clipped) {
        const Matrix toCamera = Multiply(*world, ToMatrix(view->toClip));
        if (!Finite(toCamera)) return false;
        for (int r = 0; r < 4; ++r) in.to_camera[r] = RowQw(toCamera, r);
        in.clip = {Bits(view->clip[0]), Bits(view->clip[1]), Bits(view->clip[2]), Bits(view->clip[3])};
    }

    bool lit = false;
    if (const auto* ml = Guest<ModelLights>(lightsAddress)) {
        const float* dirs = reinterpret_cast<const float*>(te::gfx::RecordGuestPointer(ml->directions, 48));
        const float* cols = reinterpret_cast<const float*>(te::gfx::RecordGuestPointer(ml->colours, 48));
        const float* amb = reinterpret_cast<const float*>(te::gfx::RecordGuestPointer(ml->ambient, 16));
        if (dirs && cols && amb) {
            lit = true;
            for (int i = 0; i < 3; ++i) {
                float d[3];
                for (int k = 0; k < 3; ++k) {
                    float sum = 0.f;
                    for (int r = 0; r < 3; ++r) sum += (*world)[k][r] * dirs[i * 4 + r];
                    d[k] = sum;
                }
                in.lights[i] = {Bits(d[0]), Bits(d[1]), Bits(d[2]), 0};
                in.colours[i] = {Bits(cols[i * 4]), Bits(cols[i * 4 + 1]), Bits(cols[i * 4 + 2]), Bits(cols[i * 4 + 3])};
            }
            in.ambient = {Bits(amb[0]), Bits(amb[1]), Bits(amb[2]), Bits(amb[3])};
        }
    }
    if (!lit) {
        for (int i = 0; i < 3; ++i) { in.lights[i] = {0, 0, 0, 0}; in.colours[i] = {0, 0, 0, 0}; }
        in.ambient = {Bits(1.f), Bits(1.f), Bits(1.f), Bits(1.f)};
    }
    in.scroll = {0.f, 0.f};
    return true;
}

void SkinMaterial(uint32_t materialsAddress, uint32_t count, uint32_t index, ps2e_draw_state& state, uint32_t& bucket, MaterialView* out) {
    const uint32_t* materials = count ? reinterpret_cast<const uint32_t*>(te::gfx::RecordGuestPointer(materialsAddress, 4 * size_t(count))) : nullptr;
    const uint32_t resource = materials ? materials[index] : 0;
    const uint32_t* materialAddress = resource ? Guest<uint32_t>(resource + 8) : nullptr;
    const MaterialView m = te::gfx::ReadMaterial(materialAddress ? *materialAddress : 0);
    state = te::gfx::StateOf(m);
    bucket = m.material ? m.bucket : BucketOpaque;
    if (out) *out = m;
}

void EmitSkinDraws(Emitter& emitter, const std::vector<ps2r::gs::DrawIr>& draws) {
    for (const auto& d : draws) emitter.Append(d.verts.data(), d.verts.size());
}

void LoadSkinPart(uint32_t skin, int32_t index, int32_t count, const uint8_t* bytes, int32_t size, int32_t vertices) {
    if (index < 0 || count <= 0 || index >= count || count > 4096) return;

    const auto loaded = g_skins.find(skin);
    if (loaded != g_skins.end() &&
        (loaded->second.parts.size() != size_t(count) || loaded->second.parts[size_t(index)].decoded))
        te::gfx::DrainBuilds();
    SkinModel& m = g_skins[skin];
    if (m.parts.size() != size_t(count)) { RetireGpuSkins(m); m.parts.assign(size_t(count), SkinPart{}); }
    SkinPart& part = m.parts[size_t(index)];
    RetireGpuSkin(part);
    part = SkinPart{};
    part.vertices = vertices;
    if (bytes && size > 0 && part.packet.data() != bytes) part.packet.assign(bytes, bytes + size);
    const SkinGeo::Result r = bytes ? SkinGeo::Decode(part.packet.data(), part.packet.size(), uint32_t(vertices), part.part)
                                    : SkinGeo::Result::InvalidAsset;
    if (r != SkinGeo::Result::Ok) return;
    part.revision = ++g_skin_revision;
    part.decoded = true;
}
}

namespace te::gfx {

void FlushGpuShadows() {
    if (g_shadowRequests.empty()) return;
    std::vector<MeshRequest> requests;
    std::vector<std::pair<ShadowModel*, uint32_t>> owners;
    std::vector<std::array<float, 3>> tri;
    for (const auto& [model, part] : g_shadowRequests) {
        auto it = g_shadowModels.find(model);
        if (it == g_shadowModels.end() || part >= it->second.parts.size()) continue;
        ShadowModel& sm = it->second;
        if (sm.gpuState[part] != 1 || !sm.decoded[part]) continue;
        sm.gpuState[part] = 2;
        if (title::ShadowGeometry::GpuTriangles(sm.parts[part], tri, sm.gpuBox[part]) != title::ShadowGeometry::Result::Ok) continue;
        MeshRequest r;
        r.vertices.resize(tri.size());
        for (size_t k = 0; k < tri.size(); ++k) {
            r.vertices[k] = ps2e_draw_mesh_vertex{};
            r.vertices[k].x = tri[k][0]; r.vertices[k].y = tri[k][1]; r.vertices[k].z = tri[k][2];
            r.vertices[k].q = 1.f;
            r.vertices[k].a = r.vertices[k].la = 0x80;
        }
        requests.push_back(std::move(r));
        owners.emplace_back(&sm, part);
    }
    g_shadowRequests.clear();
    CreateMeshes(requests);
    for (size_t k = 0; k < owners.size(); ++k) owners[k].first->gpu[owners[k].second] = requests[k].handle;
}
void FlushGpuSkins() {
    FlushGpuShadows();
    if (g_skinRequests.empty() && g_blendSkinRequests.empty()) return;
    std::vector<SkinRequest> requests;
    std::vector<uint32_t*> handles;
    std::vector<SkinGeo::GpuVertex> tri;
    struct Asked { const SkinGeo::Part* part; uint8_t* state; uint32_t* handle; };
    std::vector<Asked> asked;
    for (const auto& [address, index] : g_skinRequests) {
        auto it = g_skins.find(address);
        if (it == g_skins.end() || index >= it->second.parts.size()) continue;
        SkinPart& part = it->second.parts[index];
        if (part.decoded) asked.push_back(Asked{&part.part, &part.gpuState, &part.gpu});
    }
    for (const auto& [address, packet] : g_blendSkinRequests) {
        auto it = g_blendSkins.find(address);
        if (it == g_blendSkins.end()) continue;
        auto pt = it->second.find(packet);
        if (pt != it->second.end() && pt->second.decoded) asked.push_back(Asked{&pt->second.part, &pt->second.gpuState, &pt->second.gpu});
    }
    g_skinRequests.clear();
    g_blendSkinRequests.clear();
    for (const Asked& a : asked) {
        if (*a.state != 1) continue;
        *a.state = 2;
        if (SkinGeo::GpuTriangles(*a.part, tri) != SkinGeo::Result::Ok) continue;
        SkinRequest r;
        r.vertices.resize(tri.size());
        r.bones.resize(tri.size());
        for (size_t k = 0; k < tri.size(); ++k) {
            const SkinGeo::GpuVertex& v = tri[k];
            ps2e_draw_mesh_vertex& o = r.vertices[k];
            o = ps2e_draw_mesh_vertex{};
            o.x = v.position[0]; o.y = v.position[1]; o.z = v.position[2];
            o.s = v.st[0]; o.t = v.st[1]; o.q = 1.f;

            uint8_t lo[4], hi[4];
            for (int c = 0; c < 4; ++c) {
                const int32_t q = std::clamp<int32_t>(v.colour[c], -32768, 32767);
                lo[c] = uint8_t(uint32_t(q) & 0xFFu);
                hi[c] = uint8_t((uint32_t(q) >> 8) & 0xFFu);
            }
            o.lr = lo[0]; o.lg = lo[1]; o.lb = lo[2]; o.la = lo[3];
            o.r = hi[0]; o.g = hi[1]; o.b = hi[2]; o.a = hi[3];
            o.nx = v.normal[0]; o.ny = v.normal[1]; o.nz = v.normal[2];
            for (int b = 0; b < 3; ++b) { r.bones[k].bone[b] = v.bone[b]; r.bones[k].weight[b] = v.weight[b]; }
            r.bones[k].bone[3] = float(v.index);
            r.bones[k].pad = 0.f;
        }
        requests.push_back(std::move(r));
        handles.push_back(a.handle);
    }
    CreateSkins(requests);
    for (size_t k = 0; k < handles.size(); ++k) *handles[k] = requests[k].handle;
}
}

extern "C" {

void ps2e_gfx2_skin_loaded(ps2e_u32 skin, ps2e_s32 index, ps2e_s32 count, ps2e_u32 packet, ps2e_s32 size, ps2e_s32 vertices) {
    te::gfx::DrainRecordLane();
    const uint8_t* bytes = size > 0 ? te::gfx::RecordGuestPointer(packet, size_t(size)) : nullptr;
    LoadSkinPart(skin, index, count, bytes, size, vertices);
}
void ps2e_gfx2_skin_released(ps2e_u32 skin) {
    te::gfx::DrainRecordLane();
    te::gfx::DrainBuilds();
    if (auto it = g_skins.find(skin); it != g_skins.end()) RetireGpuSkins(it->second);
    g_skins.erase(skin);
}
void ps2e_gfx2_blend_skin_released(ps2e_u32 skin) {
    te::gfx::DrainRecordLane();
    te::gfx::DrainBuilds();
    if (auto it = g_blendSkins.find(skin); it != g_blendSkins.end()) RetireGpuBlend(it->second);
    g_blendSkins.erase(skin);
}

int SkinSide(SkinBounds& bound, const SkinGeo::Part& part, const SkinGeo::Inputs& in, float morphRadius = 0.f) {
    const auto F = [](uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; };
    if (!(morphRadius >= 0.f) || !std::isfinite(morphRadius)) return 0;
    if (!bound.boundMade) {
        bound.boundMade = true;
        struct Acc { float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f}; float r = 0.f; };
        std::unordered_map<uint32_t, Acc> acc;
        float wmax = 0.f, wmin = 1e30f;
        bool ok = true;

        for (int pass = 0; pass < 2 && ok; ++pass)
            for (const auto& batch : part.batches) {
                const float sx = F(batch.scale[0]);
                for (uint32_t j = 0; j < batch.Vertices(); ++j) {
                    const auto& q0 = batch.vertices[4 * j];
                    const auto& q2 = batch.vertices[4 * j + 2];
                    const float p[3] = {float(int32_t(q0[0])) * sx, float(int32_t(q0[1])) * sx, float(int32_t(q0[2])) * sx};
                    const uint32_t bones = q2[3] & 3u;
                    const int used = bones == 1 ? 1 : bones == 2 ? 2 : 3;
                    float w = 0.f;
                    for (int b = 0; b < used; ++b) {
                        Acc& a = acc[q2[size_t(b)] & 0x1FFu];
                        if (pass == 0) {
                            const float wb = F(q2[size_t(b)]);
                            if (!(wb >= 0.f) || !std::isfinite(p[0] + p[1] + p[2])) ok = false;
                            w += wb;
                            for (int k = 0; k < 3; ++k) { a.lo[k] = std::min(a.lo[k], p[k]); a.hi[k] = std::max(a.hi[k], p[k]); }
                        } else {
                            float d2 = 0.f;
                            for (int k = 0; k < 3; ++k) { const float d = p[k] - 0.5f * (a.lo[k] + a.hi[k]); d2 += d * d; }
                            a.r = std::max(a.r, std::sqrt(d2));
                        }
                    }
                    if (pass == 0) { wmax = std::max(wmax, w); wmin = std::min(wmin, w); }
                }
            }
        if (ok && std::isfinite(wmax) && wmin > 0.f && wmin <= wmax) {
            for (const auto& [at, a] : acc)
                bound.boneRadius.push_back({at, {0.5f * (a.lo[0] + a.hi[0]), 0.5f * (a.lo[1] + a.hi[1]), 0.5f * (a.lo[2] + a.hi[2])}, a.r});
            bound.weightSum = wmax;
            bound.weightMin = wmin;
        }
    }
    if (bound.boneRadius.empty()) return 0;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    bool any = false;
    const size_t palette = in.joints.size();
    for (const SkinBounds::BoneBound& bb : bound.boneRadius) {
        const uint32_t at = bb.at;
        if (size_t(at) + 3 >= palette) return 0;
        const auto &c0 = in.joints[at], &c1 = in.joints[at + 1], &c2 = in.joints[at + 2], &tr = in.joints[at + 3];
        for (int k = 0; k < 3; ++k) {
            const float a = F(c0[k]), b = F(c1[k]), c = F(c2[k]);
            const float centre = F(tr[k]) + a * bb.centre[0] + b * bb.centre[1] + c * bb.centre[2];
            if (!std::isfinite(centre) || !std::isfinite(a) || !std::isfinite(b) || !std::isfinite(c)) return 0;
            const float terms = std::fabs(F(tr[k])) + std::fabs(a * bb.centre[0]) + std::fabs(b * bb.centre[1]) + std::fabs(c * bb.centre[2]);
            const float e = (bb.radius + morphRadius) * std::sqrt(a * a + b * b + c * c) * 1.001f + 1e-3f * (1.f + terms);
            if (!std::isfinite(e)) return 0;
            lo[k] = std::min(lo[k], centre - e);
            hi[k] = std::max(hi[k], centre + e);
        }
        any = true;
    }
    if (!any) return 0;
    float box[2][3];
    for (int k = 0; k < 3; ++k) {
        const float a = bound.weightMin * lo[k], b = bound.weightMin * hi[k], c = bound.weightSum * lo[k], d = bound.weightSum * hi[k];
        box[0][k] = std::min(std::min(a, b), std::min(c, d));
        box[1][k] = std::max(std::max(a, b), std::max(c, d));
        if (!std::isfinite(box[0][k]) || !std::isfinite(box[1][k])) return 0;
    }
    bool front = true, behind = true;
    unsigned outside = 0x3f;
    float guard[4];
    for (int k = 0; k < 4; ++k) {
        guard[k] = F(in.clip[k]);
        if (!(guard[k] > 0.f) || !std::isfinite(guard[k])) return 0;
    }
    for (int c = 0; c < 8; ++c) {
        const float x = box[c & 1][0], y = box[(c >> 1) & 1][1], z = box[(c >> 2) & 1][2];
        float o[4], screen[4], error[4], screenError[4];
        for (int k = 0; k < 4; ++k) {
            o[k] = F(in.to_camera[3][k]) + F(in.to_camera[0][k]) * x + F(in.to_camera[1][k]) * y + F(in.to_camera[2][k]) * z;
            screen[k] = F(in.to_screen[3][k]) + F(in.to_screen[0][k]) * x + F(in.to_screen[1][k]) * y + F(in.to_screen[2][k]) * z;
            if (!std::isfinite(o[k]) || !std::isfinite(screen[k])) return 0;
            const auto magnitude = [&](const std::array<SkinGeo::Qw, 4>& m) {
                return std::fabs(F(m[3][k])) + std::fabs(F(m[0][k]) * x) + std::fabs(F(m[1][k]) * y) + std::fabs(F(m[2][k]) * z);
            };
            error[k] = 1e-5f * (1.f + magnitude(in.to_camera)) * guard[k];
            screenError[k] = 1e-5f * (1.f + magnitude(in.to_screen));
            o[k] *= guard[k];
            if (!std::isfinite(o[k]) || !std::isfinite(error[k]) || !std::isfinite(screenError[k])) return 0;
        }

        const float margin = error[3] + 1e-4f * (std::fabs(o[3]) + 1.f);
        if (!(o[3] < -margin)) behind = false;
        unsigned mask = 0;
        for (int k = 0; k < 3; ++k) {
            if (!(std::fabs(o[k]) + error[k] < o[3] - margin)) front = false;
            if (o[k] > std::fabs(o[3]) + margin + error[k]) mask |= 1u << (2 * k);
            if (o[k] < -std::fabs(o[3]) - margin - error[k]) mask |= 2u << (2 * k);
        }
        outside &= mask;

        if (!(screen[3] > screenError[3])) { front = false; continue; }
        for (int k = 0; k < 3; ++k) {
            const float lower = (screen[k] - screenError[k]) / (screen[3] + screenError[3]);
            const float upper = (screen[k] + screenError[k]) / (screen[3] - screenError[3]);
            if (!(lower > 1.f && upper < (k == 2 ? 2147483000.f : 65534.f))) front=false;
        }
    }
    return front ? 1 : behind && outside ? -1 : 0;
}

void ps2e_graphics_draw_skin(ps2e_u32 skinAddress, ps2e_u32 joints, ps2e_s32 jointCount, ps2e_u32 matrix, ps2e_u32 lights, ps2e_s32 mode) {
    const uint32_t record_args[] = {skinAddress, joints, uint32_t(jointCount), matrix, lights, uint32_t(mode)};
    if (te::gfx::SceneSkipNow()) return;
    if (te::gfx::CaptureRecord(te::gfx::RecordCall::Skin, record_args)) return;
    const auto* skin = Guest<te_gfx_skin>(skinAddress);
    if (!skin || !te::gfx::EnsureDevice()) return;
    auto it = g_skins.find(skinAddress);
    if (it == g_skins.end()) return;
    SkinGeo::Inputs in;
    SkinJointScratch scratch(in);
    if (!SkinInputs(joints, jointCount, matrix, lights, mode, in)) return;
    static Emitter emitter;
    static std::vector<ps2r::gs::DrawIr> draws;
    const uint32_t count = std::min<uint32_t>(skin->count, uint32_t(it->second.parts.size()));
    const uint64_t skinId = OrdinalId(TagId(skinAddress, 0));
    for (uint32_t i = 0; i < count; ++i) {
        const SkinPart& part = it->second.parts[i];
        if (!part.decoded) continue;
        ps2e_draw_state state;
        uint32_t bucket;
        MaterialView m;
        SkinMaterial(skin->materials, skin->count, i, state, bucket, &m);
        if (m.shader && std::isfinite(m.shader->scroll[0]) && std::isfinite(m.shader->scroll[1])) in.scroll = {m.shader->scroll[0], m.shader->scroll[1]};
        else in.scroll = {0.f, 0.f};

        te::gfx::SetDrawTag(ps2r::ps2e::InterpTag{skinId, i, ps2r::ps2e::InterpKind::Vertices});

        bool skinFront = !in.clipped;
        if (in.clipped && te::gfx::GpuSkins()) {
            const int side = SkinSide(const_cast<SkinPart&>(part).bound, part.part, in);
            if (side < 0) continue;
            skinFront = side > 0;
        }
        if (te::gfx::GpuSkins() && skinFront && !in.joints.empty() && (in.joints.size() & 3) == 0 &&
            in.joints.size() <= SkinGeo::kMaxJoints * 4) {
            SkinPart& gp = const_cast<SkinPart&>(part);
            if (gp.gpuState == 0) {
                gp.gpuState = 1;
                g_skinRequests.emplace_back(skinAddress, i);
            }
            if (gp.gpu) {
                const ps2e_draw_mesh_params mp = te::gfx::SkinParams(in.to_screen, in.scroll, in.lights, in.colours, in.ambient);
                static_assert(sizeof(SkinGeo::Qw) == 4 * sizeof(float), "a palette quadword is four floats' bits");
                te::gfx::RecordSkin(bucket, state, gp.gpu, mp, reinterpret_cast<const float*>(in.joints.data()), uint32_t(in.joints.size()));
                continue;
            }
        }
        if (Deferrable()) {

            te::gfx::DeferredBuild& build = te::gfx::ReserveDeferred(bucket, state);
            auto execute = [&build, geometry = &part.part, hint = &part.hint, buildState = BuildState(), textured = state.texture != 0, skinAddress, i, revision = part.revision](const SkinGeo::Inputs& in) {
                te::r3::Probe probe(build,te::r3::Skin);
                SkinMemo* memo = nullptr;
                if (revision) {
                    if (!build.skin_memo) build.skin_memo = std::make_shared<SkinMemo>();
                    memo = static_cast<SkinMemo*>(build.skin_memo.get());
                    if (memo->Matches(revision, in, buildState, textured)) {
                        memo->payload.misses = 0;
                        memo->payload.Load(build);
                        if(probe.TypedHit()) {
                            hint->Store(build.verts.size());
                            return;
                        }
                    }
                }

                ReserveVertices(build, *hint);
                Collector collector{&build, textured};
                const SkinGeo::Result r = SkinGeo::Build(*geometry, in, buildState, &Collector::Sink, &collector);
                if (r != SkinGeo::Result::Ok) {
                    collector.Discard();
                    return;
                }
                collector.Done(*hint);
                if (memo && !probe.HasHit()) memo->Store(revision, in, buildState, textured, build);
            };
            if (!build.skin_inputs) build.skin_inputs = std::make_shared<PooledSkinInputs>();
            auto* snapshot = &static_cast<PooledSkinInputs*>(build.skin_inputs.get())->in;
            *snapshot = in;
            te::gfx::PostBuild(build, [execute, snapshot] {execute(*snapshot);});
            continue;
        }
        draws.clear();
        const SkinGeo::Result r = SkinGeo::Build(part.part, in, BuildState(), draws);
        if (r != SkinGeo::Result::Ok) continue;
        emitter.Begin(bucket, state);
        EmitSkinDraws(emitter, draws);
    }
    te::gfx::ClearDrawTag();
}

void ps2e_graphics_draw_blend_skin(ps2e_u32 skinAddress, ps2e_u32 joints, ps2e_s32 jointCount, ps2e_u32 matrix, ps2e_u32 lights,
                                   ps2e_u32 weightsAddress, ps2e_u32 shapesAddress, ps2e_s32 shapeCount, ps2e_s32 mode) {
    const uint32_t record_args[] = {skinAddress, joints, uint32_t(jointCount), matrix, lights, weightsAddress, shapesAddress, uint32_t(shapeCount), uint32_t(mode)};
    if (te::gfx::SceneSkipNow()) return;
    if (te::gfx::CaptureRecord(te::gfx::RecordCall::Blend, record_args)) return;
    const auto* skin = Guest<te_gfx_blend_skin>(skinAddress);
    if (!skin || !te::gfx::EnsureDevice()) return;
    SkinGeo::Inputs in;
    SkinJointScratch scratch(in);
    if (!SkinInputs(joints, jointCount, matrix, lights, mode, in)) return;
    if (shapeCount < 0 || shapeCount > 64) return;
    const float* weights = shapeCount ? reinterpret_cast<const float*>(te::gfx::RecordGuestPointer(weightsAddress, 4 * size_t(shapeCount))) : nullptr;
    const int32_t* shapes = shapeCount ? reinterpret_cast<const int32_t*>(te::gfx::RecordGuestPointer(shapesAddress, 4 * size_t(shapeCount))) : nullptr;
    if (shapeCount && (!weights || !shapes)) return;
    const uint32_t* subModels = skin->count ? reinterpret_cast<const uint32_t*>(te::gfx::RecordGuestPointer(skin->subModels, 4 * size_t(skin->count))) : nullptr;
    if (!subModels || skin->count > 4096) return;
    auto& cache = g_blendSkins[skinAddress];
    static Emitter emitter;
    static std::vector<ps2r::gs::DrawIr> draws;
    const uint64_t blendId = OrdinalId(TagId(skinAddress, 1));
    for (uint32_t i = 0; i < skin->count; ++i) {
        const auto* subModel = Guest<te_gfx_blend_sub_model>(subModels[i]);
        if (!subModel || !subModel->count || subModel->count > 4096) continue;
        const uint32_t* parts = reinterpret_cast<const uint32_t*>(te::gfx::RecordGuestPointer(subModel->parts, 4 * size_t(subModel->count)));
        if (!parts) continue;
        ps2e_draw_state state;
        uint32_t bucket;
        MaterialView m;
        SkinMaterial(skin->materials, skin->count, i, state, bucket, &m);
        if (m.shader && std::isfinite(m.shader->scroll[0]) && std::isfinite(m.shader->scroll[1])) in.scroll = {m.shader->scroll[0], m.shader->scroll[1]};
        else in.scroll = {0.f, 0.f};
        const auto* first = Guest<te_gfx_blend_part>(parts[0]);
        if (!first) continue;
        SkinGeo::BlendInputs blend;
        SkinShapeScratch shapeScratch(blend);
        blend.factors = {first->shapeFactors[0], first->shapeFactors[1], first->shapeFactors[2]};
        te::gfx::SetDrawTag(ps2r::ps2e::InterpTag{blendId, i, ps2r::ps2e::InterpKind::Vertices});
        emitter.Begin(bucket, state);
        for (uint32_t p = 0; p < subModel->count; ++p) {
            const auto* part = Guest<te_gfx_blend_part>(parts[p]);
            if (!part || !part->packet || !part->packetSize) continue;
            BlendPartGeo& geo = cache[part->packet];
            if (!geo.tried) {
                geo.tried = true;
                const uint8_t* bytes = te::gfx::RecordGuestPointer(part->packet, part->packetSize);
                const SkinGeo::Result r = bytes ? SkinGeo::DecodePart(bytes, part->packetSize, 0, geo.part) : SkinGeo::Result::InvalidAsset;
                if (r == SkinGeo::Result::Ok) {
                    geo.revision = ++g_skin_revision;
                    geo.decoded = true;
                }
            }
            if (!geo.decoded) continue;

            blend.shapes.clear();
            const uint32_t* addresses = part->shapeCount ? reinterpret_cast<const uint32_t*>(te::gfx::RecordGuestPointer(part->shapeAddresses, 4 * size_t(part->shapeCount))) : nullptr;
            const uint32_t* counts = part->shapeCount ? reinterpret_cast<const uint32_t*>(te::gfx::RecordGuestPointer(part->shapeCounts, 4 * size_t(part->shapeCount))) : nullptr;
            bool ok = true;
            for (int32_t k = 0; k < shapeCount && ok; ++k) {
                const int32_t shape = shapes[k];
                if (shape < 0 || shape >= int32_t(part->shapeCount) || !addresses || !counts) { ok = false; break; }
                SkinGeo::Shape sh;
                sh.weight = weights[k];
                sh.count = counts[shape];
                sh.rows = sh.count ? te::gfx::RecordGuestPointer(addresses[shape], 4 * size_t(sh.count)) : nullptr;
                if (!sh.rows) { ok = false; break; }
                blend.shapes.push_back(sh);
            }
            if (!ok) continue;

            float disp[SkinGeo::kMaxBatchVertices][4];
            uint32_t displaced = 0;
            bool positionsReady = blend.shapes.empty();
            bool skinFront = !in.clipped;
            if (in.clipped && te::gfx::GpuSkins()) {
                positionsReady = blend.shapes.empty() || SkinGeo::BlendPositions(geo.part, blend, disp, &displaced);
                float morphRadius = 0.f;
                if (positionsReady && displaced) {
                    const auto F = [](uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; };
                    const auto& batch = geo.part.batches[0];
                    const float scale = F(batch.scale[0]);
                    for (uint32_t j = 0; j < displaced; ++j) {
                        float d2 = 0.f;
                        for (int k = 0; k < 3; ++k) {
                            const float d = disp[j][k] - float(int32_t(batch.vertices[4 * j][k])) * scale;
                            d2 += d * d;
                        }
                        if (!std::isfinite(d2)) { positionsReady = false; break; }
                        morphRadius = std::max(morphRadius, std::sqrt(d2));
                    }
                }
                const int side = positionsReady ? SkinSide(geo.bound, geo.part, in, morphRadius) : 0;
                if (side < 0) continue;
                skinFront = side > 0;
            }
            if (te::gfx::GpuSkins() && skinFront && !in.joints.empty() && (in.joints.size() & 3) == 0 &&
                in.joints.size() <= SkinGeo::kMaxJoints * 4) {
                if (geo.gpuState == 0) {
                    geo.gpuState = 1;
                    g_blendSkinRequests.emplace_back(skinAddress, part->packet);
                }
                if (geo.gpu && (positionsReady || SkinGeo::BlendPositions(geo.part, blend, disp, &displaced))) {
                    const ps2e_draw_mesh_params mp = te::gfx::SkinParams(in.to_screen, in.scroll, in.lights, in.colours, in.ambient);
                    te::gfx::SetDrawTag(ps2r::ps2e::InterpTag{blendId, (i << 8) | p, ps2r::ps2e::InterpKind::Vertices});
                    te::gfx::RecordSkin(bucket, emitter.state, geo.gpu, mp, reinterpret_cast<const float*>(in.joints.data()),
                                        uint32_t(in.joints.size()), displaced ? &disp[0][0] : nullptr, displaced);
                    te::gfx::SetDrawTag(ps2r::ps2e::InterpTag{blendId, i, ps2r::ps2e::InterpKind::Vertices});
                    continue;
                }
            }
            if (Deferrable()) {

                te::gfx::DeferredBuild& build = te::gfx::ReserveDeferred(bucket, emitter.state);
                auto execute = [&build, geometry = &geo.part, hint = &geo.hint, buildState = BuildState(), textured = emitter.textured, skinAddress, i, p, revision = geo.revision](const SkinGeo::Inputs& in, const SkinGeo::BlendInputs& blend) {
                    te::r3::Probe probe(build,te::r3::Blend);
                    if(probe.Active()) {
                        auto k=SkinKey(revision,in,buildState,textured);k.Add(blend.factors);k.Add(blend.shapes.size());
                        for(const auto& sh:blend.shapes){k.Add(sh.weight);k.Add(sh.count);k.AddBytes(sh.rows,size_t(sh.count)*4);}
                        if(probe.Match(std::move(k))){hint->Store(build.verts.size());return;}
                    }
                    ReserveVertices(build, *hint);
                    Collector collector{&build, textured};
                    const SkinGeo::Result r = SkinGeo::BuildBlend(*geometry, in, blend, buildState, &Collector::Sink, &collector);
                    if (r != SkinGeo::Result::Ok) {
                        collector.Discard();
                        return;
                    }
                    collector.Done(*hint);
                };
                if (!build.skin_inputs) build.skin_inputs=std::make_shared<PooledSkinInputs>();
                auto* snapshot=static_cast<PooledSkinInputs*>(build.skin_inputs.get());
                snapshot->in=in;snapshot->blend=blend;
                te::gfx::PostBuild(build,[execute,snapshot]{execute(snapshot->in,snapshot->blend);});
                continue;
            }
            draws.clear();
            const SkinGeo::Result r = SkinGeo::BuildBlend(geo.part, in, blend, BuildState(), draws);
            if (r != SkinGeo::Result::Ok) continue;
            EmitSkinDraws(emitter, draws);
        }
    }
    te::gfx::ClearDrawTag();
}

}

