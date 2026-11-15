#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <optional>
#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/math/collision_math.h"
#include "games/slus_20909_crash_twinsanity/hle/math/joint_math.h"
#include "games/slus_20909_crash_twinsanity/hle/math/math_library.h"

#include "games/slus_20909_crash_twinsanity/hle/math/ray_math.h"

#include "games/slus_20909_crash_twinsanity/hle/math/particle_view_math.h"
#include "graphics_internal.h"
#include "ps2e_seam.h"
#include "te_host.h"

namespace {

using Vector = title::MathLibrary::Vector;
using Matrix = title::MathLibrary::Matrix;

std::optional<Vector> ReadVector(uint32_t address) {
    const float* v = reinterpret_cast<const float*>(te::GuestPointer(address, 16));
    if (!v) return std::nullopt;
    return Vector{v[0], v[1], v[2], v[3]};
}
std::optional<Matrix> ReadMatrix(uint32_t address) {
    const float* m = reinterpret_cast<const float*>(te::GuestPointer(address, 64));
    if (!m) return std::nullopt;
    Matrix out;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[r][c] = m[r * 4 + c];
    return out;
}
void WriteVector(uint32_t address, const Vector& v) {
    float* out = reinterpret_cast<float*>(te::GuestPointer(address, 16));
    if (out) std::memcpy(out, v.data(), 16);
}
void WriteMatrix(uint32_t address, const Matrix& m) {
    float* out = reinterpret_cast<float*>(te::GuestPointer(address, 64));
    if (!out) return;
    for (int r = 0; r < 4; ++r) std::memcpy(out + r * 4, m[r].data(), 16);
}
float* Floats(uint32_t address, size_t count) { return reinterpret_cast<float*>(te::GuestPointer(address, count * 4)); }

int64_t FpuKey(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    const int64_t magnitude = int64_t(bits & 0x7FFFFFFFu);
    return (bits & 0x80000000u) ? -magnitude - 1 : magnitude;
}

struct ParticleView {
    Vector camera;
    Matrix toScreen;
    std::array<Vector, 6> planes;
    bool valid = false;
};
constexpr int kParticleViews = 64;
ParticleView g_particleViews[kParticleViews];
constexpr uint32_t kViewPlanes = 0x003B4900;

Vector TransformPlane(const Vector& plane, const Matrix& place) {

    Vector out{};
    for (int c = 0; c < 3; ++c) out[c] = plane[0] * place[0][c] + plane[1] * place[1][c] + plane[2] * place[2][c];
    out[3] = plane[3] - (out[0] * place[3][0] + out[1] * place[3][1] + out[2] * place[3][2]);
    return out;
}

struct RayState {
    Vector start{}, end{};
    bool haveRay = false;
    std::optional<title::CollisionMath::EdgeSetup> pending;
    title::CollisionMath::Triangle triangle{};
    Vector nearest{};
} g_ray;

std::optional<title::CollisionMath::EdgeSetup> g_edges;

std::optional<title::CollisionMath::HullSupport> g_support;

}

extern "C" {

ps2e_f32 ps2e_math_max(ps2e_f32 first, ps2e_f32 second) { return FpuKey(first) >= FpuKey(second) ? first : second; }

void ps2e_math_slerp_rotations(ps2e_u32 from, ps2e_u32 to, ps2e_f32 share, ps2e_u32 out) {
    const auto f = ReadVector(from), t = ReadVector(to);
    if (!f || !t) return;
    WriteVector(out, title::JointMath::Interpolate(*f, *t, share).rotation);
}

void ps2e_math_euler_rotation(ps2e_u32 anglesNow, ps2e_u32 anglesNext, ps2e_u32 moveNow, ps2e_u32 moveNext, ps2e_u32 rotation,
                              ps2e_u32 move) {
    const auto an = ReadVector(anglesNow), ax = ReadVector(anglesNext), mn = ReadVector(moveNow), mx = ReadVector(moveNext);
    if (!an || !ax || !mn || !mx) return;
    const auto result = title::JointMath::EulerRotation(*an, *ax, *mn, *mx);
    WriteVector(rotation, result.rotation);
    WriteVector(move, result.move);
}

void ps2e_math_turn_rotation(ps2e_u32 by, ps2e_u32 rotation) {
    const auto b = ReadVector(by), r = ReadVector(rotation);
    if (!b || !r) return;
    WriteVector(rotation, title::JointMath::Turn(*b, *r));
}

void ps2e_math_lerp(ps2e_u32 from, ps2e_u32 to, ps2e_f32 share, ps2e_u32 out) {
    const auto f = ReadVector(from), t = ReadVector(to);
    if (!f || !t) return;
    WriteVector(out, title::JointMath::Lerp(*f, *t, share));
}

void ps2e_math_joint_matrix(ps2e_u32 rotation, ps2e_u32 parentScale, ps2e_u32 scale, ps2e_u32 translation, ps2e_u32 parent,
                            ps2e_u32 out) {
    Matrix matrix;
    if (rotation) {
        const auto r = ReadVector(rotation);
        if (!r) return;
        Vector ps{1, 1, 1, 1};
        if (parentScale) {
            if (const auto v = ReadVector(parentScale)) ps = *v;
        }
        std::optional<Vector> own;
        if (scale) own = ReadVector(scale);
        const auto basis = title::JointMath::RotationBasis(*r, ps, own ? &*own : nullptr);
        for (int row = 0; row < 3; ++row) matrix[row] = basis[row];
    } else {
        matrix[0] = {1, 0, 0, 0};
        matrix[1] = {0, 1, 0, 0};
        matrix[2] = {0, 0, 1, 0};
    }
    matrix[3] = {0, 0, 0, 1};
    if (translation) {
        if (const auto t = ReadVector(translation)) matrix[3] = *t;
    }
    if (parent) {
        if (const auto p = ReadMatrix(parent)) matrix = title::JointMath::MultiplyByParent(matrix, *p);
    }
    WriteMatrix(out, matrix);
}

void ps2e_math_multiply_by_parent(ps2e_u32 matrix, ps2e_u32 parent, ps2e_u32 out) {
    const auto m = ReadMatrix(matrix), p = ReadMatrix(parent);
    if (!m || !p) return;
    WriteMatrix(out, title::JointMath::MultiplyByParent(*m, *p));
}

ps2e_s32 ps2e_graphics_load_particle_view(ps2e_u32 matrices, ps2e_s32 index) {
    if (index < 0 || index >= kParticleViews) return index;
    const auto chunk = ReadMatrix(matrices), toScreen = ReadMatrix(matrices + 0x40);
    if (!chunk || !toScreen) return index;
    ParticleView& v = g_particleViews[index];
    v.camera = (*chunk)[3];
    v.toScreen = *toScreen;
    const float* planes = Floats(kViewPlanes, 24);
    for (int i = 0; i < 6; ++i) {
        Vector plane = planes ? Vector{planes[i * 4], planes[i * 4 + 1], planes[i * 4 + 2], planes[i * 4 + 3]} : Vector{0, 0, 0, 0};
        v.planes[i] = TransformPlane(plane, *chunk);
    }
    v.valid = true;

    if (int32_t* kept = reinterpret_cast<int32_t*>(te::GuestPointer(matrices + 0x100, 4))) *kept = index;
    return index;
}

ps2e_f32 ps2e_math_view_distance(ps2e_s32 view, ps2e_u32 point) {
    const float* p = Floats(point, 3);
    if (!p || view < 0 || view >= kParticleViews || !g_particleViews[view].valid) return 0x1.312D00p+23f;
    const Vector& c = g_particleViews[view].camera;
    const float dx = p[0] - c[0], dy = p[1] - c[1], dz = p[2] - c[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

ps2e_s32 ps2e_math_particle_block_view(ps2e_s32 view, ps2e_s32 keepsTranslation, ps2e_u32 extents, ps2e_u32 gravity, ps2e_u32 place,
                                       ps2e_f32 scale, ps2e_u32 matrix, ps2e_u32 distance) {
    const float* e = Floats(extents, 3);
    const auto g = ReadMatrix(gravity);
    const float* p = Floats(place, 3);
    float* d = Floats(distance, 1);
    if (!e || !g || !p || view < 0 || view >= kParticleViews || !g_particleViews[view].valid) {
        if (d) *d = 0x1.312D00p+23f;
        return 1;
    }
    const ParticleView& v = g_particleViews[view];
    const Vector c = v.camera;
    const Vector box{e[0], e[1], e[2], 1.f};
    const Vector at{p[0], p[1], p[2], 1.f};
    Matrix out{};
    float far = 0.f;
    bool outside = false;

    title::ParticleViewMath::View record{};
    for (int grp = 0; grp < 2; ++grp) {
        const int base = grp * 7;
        for (int lane = 0; lane < 4; ++lane) {
            const int pi = grp * 4 + lane;
            const Vector plane = pi < 6 ? v.planes[pi] : Vector{0.f, 0.f, 0.f, 0.f};
            for (int k = 0; k < 3; ++k) {
                record.qw[base + k][lane] = std::fabs(plane[k]);
                record.qw[base + 3 + k][lane] = plane[k];
            }
            record.qw[base + 6][lane] = plane[3];
        }
    }
    record.qw[14] = {c[0], c[1], c[2], 1.f};
    for (int r = 0; r < 4; ++r) record.qw[15 + r] = v.toScreen[r];
    const auto answer = title::ParticleViewMath::BlockView(record, box, *g, at, scale, keepsTranslation != 0);
    out = answer.matrix;
    far = answer.distance;
    outside = answer.flags != 0;
    if (d) *d = far;
    WriteMatrix(matrix, out);
    return outside ? 1 : 0;
}

void ps2e_math_set_ray(ps2e_u32 start, ps2e_u32 end) {
    const auto s = ReadVector(start), e = ReadVector(end);
    if (!s || !e) return;
    g_ray.start = *s;
    g_ray.end = *e;
    g_ray.haveRay = true;
    g_ray.pending.reset();
}

void ps2e_math_start_ray_triangle(ps2e_u32 vertices, ps2e_u32 nearest) {
    const float* v = Floats(vertices, 12);
    const auto n = ReadVector(nearest);
    if (!v || !n || !g_ray.haveRay) { g_ray.pending.reset(); return; }
    for (int i = 0; i < 3; ++i) g_ray.triangle[i] = {v[i * 4], v[i * 4 + 1], v[i * 4 + 2], v[i * 4 + 3]};
    g_ray.nearest = *n;

    g_ray.pending = title::CollisionMath::EdgeSetup{};
}

ps2e_s32 ps2e_math_finish_ray_triangle(ps2e_u32 hit) {
    if (!g_ray.pending) return 0;
    g_ray.pending.reset();

    for (int a = 0, apart = 0; a < 3; ++a) {
        const float r0 = std::min(g_ray.start[a], g_ray.end[a]), r1 = std::max(g_ray.start[a], g_ray.end[a]);
        const float t0 = std::min(g_ray.triangle[0][a], std::min(g_ray.triangle[1][a], g_ray.triangle[2][a]));
        const float t1 = std::max(g_ray.triangle[0][a], std::max(g_ray.triangle[1][a], g_ray.triangle[2][a]));
        const float margin = 0.01f + 1e-3f * std::max(std::max(std::fabs(t0), std::fabs(t1)), std::max(std::fabs(r0), std::fabs(r1)));
        apart = r1 + margin < t0 || t1 + margin < r0 || !(r0 == r0) || !(t0 == t0);
        if (!(r0 == r0) || !(t0 == t0)) apart = 0;
        if (apart) return 0;
    }
    const title::RayMath::Result r = title::RayMath::Test(title::RayMath::Ray{g_ray.start, g_ray.end}, {g_ray.triangle[0], g_ray.triangle[1], g_ray.triangle[2]}, g_ray.nearest);
    if (!r.nearer) return 0;
    WriteVector(hit, r.hit);
    return 1;
}

void ps2e_vu0_start_triangle_edge_tests(ps2e_u32 start, ps2e_u32 end, ps2e_u32 triangle, ps2e_u32 values) {
    const auto s = ReadVector(start), e = ReadVector(end);
    const float* t = Floats(triangle, 12);
    float* out = Floats(values, 2);
    if (!s || !e || !t || !out) return;
    title::CollisionMath::Triangle tri;
    for (int i = 0; i < 3; ++i) tri[i] = {t[i * 4], t[i * 4 + 1], t[i * 4 + 2], t[i * 4 + 3]};
    g_edges = title::CollisionMath::EvaluateStartEdges(*s, *e, tri);
    out[0] = g_edges->plane_values[0];
    out[1] = g_edges->plane_values[1];
}

void ps2e_vu0_finish_triangle_edge_tests(ps2e_u32 values) {
    float* out = Floats(values, 6);
    if (!out || !g_edges) return;
    const auto result = title::CollisionMath::EvaluateFinishEdges(*g_edges);
    for (int i = 0; i < 6; ++i) out[i] = result.values[i];
}

void ps2e_vu0_make_vertex_differences(ps2e_u32 request) {
    const uint32_t* r = reinterpret_cast<const uint32_t*>(te::GuestPointer(request, 32));
    if (!r) return;
    const auto matrix = ReadMatrix(r[0]), otherMatrix = ReadMatrix(r[3]);
    const uint32_t count = r[2], otherCount = r[5];
    if (!matrix || !otherMatrix || count == 0 || otherCount == 0 || count > 1024 || otherCount > 1024) return;
    const float* vertices = Floats(r[1], size_t(count) * 4);
    const float* otherVertices = Floats(r[4], size_t(otherCount) * 4);
    float* differences = Floats(r[6], size_t(count) * otherCount * 4);
    float* moved = Floats(r[7], (size_t(count) + otherCount) * 4);
    if (!vertices || !otherVertices || !differences || !moved) return;
    for (uint32_t i = 0; i < count; ++i) {
        const Vector point{vertices[i * 4], vertices[i * 4 + 1], vertices[i * 4 + 2], vertices[i * 4 + 3]};
        const auto out = title::MathLibrary::EvaluateTransformVector(*matrix, point).output;
        std::memcpy(moved + i * 4, out.data(), 16);
    }
    for (uint32_t j = 0; j < otherCount; ++j) {
        const Vector point{otherVertices[j * 4], otherVertices[j * 4 + 1], otherVertices[j * 4 + 2], otherVertices[j * 4 + 3]};
        const auto out = title::MathLibrary::EvaluateTransformVector(*otherMatrix, point).output;
        std::memcpy(moved + (count + j) * 4, out.data(), 16);
    }
    const float w = (*matrix)[2][3];
    for (uint32_t i = 0; i < count; ++i)
        for (uint32_t j = 0; j < otherCount; ++j) {
            float* d = differences + (size_t(i) * otherCount + j) * 4;
            for (int k = 0; k < 3; ++k) d[k] = moved[i * 4 + k] - moved[(count + j) * 4 + k];
            d[3] = w;
        }
}

void ps2e_vu0_group_points(ps2e_u32 points, ps2e_s32 groups) {
    if (groups <= 0 || groups > 1024) return;
    float* p = Floats(points, size_t(groups) * 16);
    if (!p) return;
    std::vector<title::CollisionMath::PointGroup> packed; packed.resize(size_t(groups));
    for (int32_t g = 0; g < groups; ++g) {
        std::array<Vector, 4> in;
        for (int i = 0; i < 4; ++i) in[i] = {p[g * 16 + i * 4], p[g * 16 + i * 4 + 1], p[g * 16 + i * 4 + 2], p[g * 16 + i * 4 + 3]};
        packed[size_t(g)] = title::CollisionMath::PackPoints(in);
    }
    for (int32_t g = 0; g < groups; ++g)
        for (int i = 0; i < 3; ++i) std::memcpy(p + g * 12 + i * 4, packed[size_t(g)][i].data(), 16);
}

void ps2e_vu0_groups_range(ps2e_u32 groups, ps2e_s32 count, ps2e_u32 axis, ps2e_u32 range) {
    if (count <= 0 || count > 1024) return;
    const float* g = Floats(groups, size_t(count) * 12);
    const auto a = ReadVector(axis);
    float* out = Floats(range, 2);
    if (!g || !a || !out) return;
    float least = INFINITY, greatest = -INFINITY;
    for (int32_t i = 0; i < count; ++i) {
        title::CollisionMath::PointGroup group;
        for (int k = 0; k < 3; ++k) group[k] = {g[i * 12 + k * 4], g[i * 12 + k * 4 + 1], g[i * 12 + k * 4 + 2], g[i * 12 + k * 4 + 3]};
        const auto projected = title::CollisionMath::ProjectGroup(group, *a);
        for (float v : projected.values) {
            least = std::min(least, v);
            greatest = std::max(greatest, v);
        }
    }
    out[0] = least;
    out[1] = greatest;
}

void ps2e_vu0_sixteen_groups_range(ps2e_u32 groups, ps2e_u32 axis, ps2e_u32 range) { ps2e_vu0_groups_range(groups, 16, axis, range); }

void ps2e_vu0_load_triangle_hull_support(ps2e_u32 triangle, ps2e_u32 hullVertices) {
    const float* t = Floats(triangle, 12);
    const float* h = Floats(hullVertices, 32);
    if (!t || !h) return;
    title::CollisionMath::Triangle tri;
    for (int i = 0; i < 3; ++i) tri[i] = {t[i * 4], t[i * 4 + 1], t[i * 4 + 2], t[i * 4 + 3]};
    std::array<Vector, 8> hull;
    for (int i = 0; i < 8; ++i) hull[i] = {h[i * 4], h[i * 4 + 1], h[i * 4 + 2], h[i * 4 + 3]};
    g_support = title::CollisionMath::BuildHullSupport(tri, hull);
}

void ps2e_vu0_triangle_hull_support_range(ps2e_u32 axis, ps2e_u32 range) {
    const auto a = ReadVector(axis);
    float* out = Floats(range, 2);
    if (!a || !out || !g_support) return;
    const auto projected = title::CollisionMath::ProjectHullSupport(*g_support, *a);
    out[0] = projected.range[0];
    out[1] = projected.range[1];
}

}
