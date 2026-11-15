#include "games/slus_20909_crash_twinsanity/hle/graphics/mesh_geometry.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <atomic>
#include <limits>
#include <utility>

#include "games/slus_20909_crash_twinsanity/hle/graphics/rounding_guard.h"

#include <arm_neon.h>

#include "engine/platform/host/platform.h"
#include "engine/vu/vu_float.h"

PS2R_FENV_ACCESS_ON

namespace title {
namespace {
using Vector = MeshGeometry::Vector;
using Matrix = MeshGeometry::Matrix;
using Vertex = MeshGeometry::Vertex;
using Draw = ps2r::gs::DrawIr;
using ScreenVertex = ps2r::gs::VertexRec;

uint32_t Bits(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    return bits;
}
float Float(uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}

float Finite(float value) {
    const uint32_t bits = Bits(value), exponent = bits & 0x7f800000;
    if (!exponent) return Float(bits & 0x80000000);
    if (exponent == 0x7f800000) return Float((bits & 0x80000000) | 0x7f7fffff);
    return value;
}
float Product(float a, float b) { return Finite(Finite(a) * Finite(b)); }
float Sum(float a, float b) { return Finite(Finite(a) + Finite(b)); }
float MultiplyAdd(float a, float b, float c) {
    return Finite(VuFloat::Fma(Finite(a), Finite(b), Finite(c)));
}
float Reciprocal(float value) {
    value = Finite(value);
    if (value == 0) return Float((Bits(value) & 0x80000000) | 0x7f7fffff);
    return Finite(1.0f / value);
}
uint32_t Integer(float value) {
    value = Finite(value);
    if (value >= 2147483648.0f) return 0x7fffffff;
    if (value < -2147483648.0f) return 0x80000000;
    return uint32_t(int32_t(value));
}
Vector Transform(const Matrix& matrix, const Vector& point, bool strip) {
    Vector result;

    const std::array<unsigned, 3> order = strip ? std::array<unsigned, 3>{1, 2, 0}
                                               : std::array<unsigned, 3>{0, 1, 2};
    for (unsigned lane = 0; lane != 4; ++lane) {
        float value = Product(matrix[3][lane], 1);
        for (unsigned row : order) value = MultiplyAdd(matrix[row][lane], point[row], value);
        result[lane] = value;
    }
    return result;
}
uint32_t Outside(const Vector& point) {

    const uint32_t w = Bits(point[3]);
    const int32_t bound = int32_t((w & 0x7f800000) ? w & 0x7fffffff : 0x007fffff);
    uint32_t result = 0;
    for (unsigned lane = 0; lane != 3; ++lane) {
        const uint32_t bits = Bits(point[lane]);
        int32_t positive, negative;
        std::memcpy(&positive, &bits, 4);
        const uint32_t flipped = bits ^ 0x80000000;
        std::memcpy(&negative, &flipped, 4);
        if (positive > bound) result |= 1u << (lane * 2);
        if (negative > bound) result |= 2u << (lane * 2);
    }
    return result;
}
bool SamePoint(const ScreenVertex& a, const ScreenVertex& b) { return a.x == b.x && a.y == b.y; }
ScreenVertex Project(const Vector& position, const Vector& attributes, const Vector& colour,
                     const MeshGeometry::View& view, const Draw& state, bool strip, bool draw) {
    const Vector clip = Transform(view.screen, position, strip);
    const float reciprocal = Reciprocal(clip[3]);
    ScreenVertex result{};
    result.x = float(Integer(Product(clip[0], reciprocal)) & 0xffff) / 16 - state.off_x;
    result.y = float(Integer(Product(clip[1], reciprocal)) & 0xffff) / 16 - state.off_y;
    volatile uint32_t depth = Integer(Product(clip[2], reciprocal));
    result.z = float(depth);
    result.s = Product(Sum(attributes[0], view.scroll[0]), reciprocal);
    result.t = Product(Sum(attributes[1], view.scroll[1]), reciprocal);
    result.q = Product(strip ? Sum(attributes[2], 0) : attributes[2], reciprocal);
    result.r = uint8_t(Integer(colour[0]));
    result.g = uint8_t(Integer(colour[1]));
    result.b = uint8_t(Integer(colour[2]));
    result.a = uint8_t(Integer(colour[3]));
    result.draw_kick = draw;
    return result;
}
struct Corner { Vector position, weight; };

void Clip(const std::array<Vertex, 3>& triangle, const Matrix& matrix, std::vector<Corner>& polygon) {

    thread_local std::vector<Corner> next_list;
    std::vector<Corner>& next = next_list;
    polygon.clear();
    for (unsigned i = 0; i != 3; ++i) {
        Corner corner{{triangle[i].position[0], triangle[i].position[1], triangle[i].position[2], 0},
                      {0, 0, 0, 1}};
        corner.weight[i] = 1;
        polygon.push_back(corner);
    }
    for (unsigned plane = 0; plane != 6 && !polygon.empty(); ++plane) {
        Vector row;
        for (unsigned i = 0; i != 4; ++i)
            row[i] = Sum(matrix[i][3], (plane % 2 ? 1 : -1) * Finite(matrix[i][plane / 2]));
        const auto distance = [&](const Corner& corner) {
            float value = Product(row[0], corner.position[0]);
            value = MultiplyAdd(row[1], corner.position[1], value);
            value = MultiplyAdd(row[2], corner.position[2], value);
            return MultiplyAdd(row[3], 1, value);
        };
        next.clear();
        Corner previous = polygon.back();
        float previous_distance = distance(previous);
        for (const auto& current : polygon) {
            const float current_distance = distance(current);
            if (std::signbit(previous_distance) != std::signbit(current_distance)) {
                const float reciprocal = Reciprocal(Finite(current_distance - previous_distance));
                Corner intersection;
                for (unsigned lane = 0; lane != 4; ++lane) {
                    intersection.position[lane] = Product(MultiplyAdd(-current.position[lane],
                        previous_distance, Product(previous.position[lane], current_distance)), reciprocal);
                    intersection.weight[lane] = Product(MultiplyAdd(-current.weight[lane],
                        previous_distance, Product(previous.weight[lane], current_distance)), reciprocal);
                }
                next.push_back(intersection);
            }
            if (!std::signbit(current_distance)) next.push_back(current);
            previous = current;
            previous_distance = current_distance;
        }
        polygon.swap(next);
    }
}
void Fan(std::vector<ScreenVertex>& out, const std::array<Vertex, 3>& triangle, const MeshGeometry::View& view,
         const Draw& state) {
    thread_local std::vector<Corner> polygon_list;
    thread_local std::vector<ScreenVertex> projected_list;
    std::vector<Corner>& polygon = polygon_list;
    std::vector<ScreenVertex>& projected = projected_list;
    Clip(triangle, view.clip, polygon);
    projected.clear();
    for (const auto& corner : polygon) {
        Vector stq{}, colour{};
        for (unsigned lane = 0; lane != 4; ++lane) {
            float color = Product(float(triangle[0].colour[lane]), corner.weight[0]);
            color = MultiplyAdd(float(triangle[1].colour[lane]), corner.weight[1], color);
            colour[lane] = MultiplyAdd(float(triangle[2].colour[lane]), corner.weight[2], color);
            if (lane < 3) {
                float attribute = Product(triangle[0].stq[lane], corner.weight[0]);
                attribute = MultiplyAdd(triangle[1].stq[lane], corner.weight[1], attribute);
                stq[lane] = MultiplyAdd(triangle[2].stq[lane], corner.weight[2], attribute);
            }
        }
        projected.push_back(Project(corner.position, stq, colour, view, state, false, true));
    }
    for (size_t i = 2; i < projected.size(); ++i) {
        out.push_back(projected[0]);
        out.push_back(projected[i - 1]);
        out.push_back(projected[i]);
    }
}
}

namespace {

#define TITLE_MESH_NEON 1

inline Vector FlushVector(const Vector& v) { return {Finite(v[0]), Finite(v[1]), Finite(v[2]), Finite(v[3])}; }
inline Matrix FlushMatrix(const Matrix& m) {
    return {FlushVector(m[0]), FlushVector(m[1]), FlushVector(m[2]), FlushVector(m[3])};
}

#define TITLE_MESH_LANES 1
using Lanes = float __attribute__((vector_size(16)));
using LaneBits = uint32_t __attribute__((vector_size(16)));
inline Lanes LoadLanes(const Vector& v) {
    Lanes lanes;
    std::memcpy(&lanes, v.data(), sizeof(lanes));
    return lanes;
}
inline Lanes FusedLanes(const Vector& row, float scale, Lanes sum) {
#if TITLE_MESH_NEON
    return (Lanes)vfmaq_n_f32((float32x4_t)sum, vld1q_f32(row.data()), scale);
#else
    return Lanes{VuFloat::Fma(row[0], scale, sum[0]), VuFloat::Fma(row[1], scale, sum[1]),
                 VuFloat::Fma(row[2], scale, sum[2]), VuFloat::Fma(row[3], scale, sum[3])};
#endif
}
inline LaneBits DenormalKey(Lanes value) { return ((LaneBits)value << 1) - 1; }
inline bool AnyDenormal(Lanes a, Lanes b, Lanes c) {
    LaneBits key = DenormalKey(a);
    const LaneBits kb = DenormalKey(b), kc = DenormalKey(c);
    key = kb < key ? kb : key;
    key = kc < key ? kc : key;
#if TITLE_MESH_NEON
    return vminvq_u32((uint32x4_t)key) < 0xffffffu;
#else
    return (key[0] < 0xffffffu) | (key[1] < 0xffffffu) | (key[2] < 0xffffffu) | (key[3] < 0xffffffu);
#endif
}

inline Lanes FlushLanes(Lanes value) {
    const LaneBits bits = (LaneBits)value, exponent = bits & 0x7f800000u, sign = bits & 0x80000000u;
    const LaneBits zero = (LaneBits)(exponent == 0), full = (LaneBits)(exponent == 0x7f800000u);
    return (Lanes)((zero & sign) | (full & (sign | 0x7f7fffffu)) | (~(zero | full) & bits));
}

#if TITLE_MESH_NEON
inline float32x4_t FiniteLanes(float32x4_t value) {
    const uint32x4_t bits = vreinterpretq_u32_f32(value);
    const uint32x4_t exponent = vandq_u32(bits, vdupq_n_u32(0x7f800000u));
    const uint32x4_t sign = vandq_u32(bits, vdupq_n_u32(0x80000000u));
    const uint32x4_t zero_exponent = vceqq_u32(exponent, vdupq_n_u32(0));
    const uint32x4_t full_exponent = vceqq_u32(exponent, vdupq_n_u32(0x7f800000u));
    const uint32x4_t saturated = vorrq_u32(sign, vdupq_n_u32(0x7f7fffffu));
    const uint32x4_t result = vbslq_u32(zero_exponent, sign, vbslq_u32(full_exponent, saturated, bits));
    return vreinterpretq_f32_u32(result);
}
#endif

inline Vector TransformFlushed(const Matrix& matrix, const Vector& point, bool strip) {
    const std::array<unsigned, 3> order = strip ? std::array<unsigned, 3>{1, 2, 0}
                                               : std::array<unsigned, 3>{0, 1, 2};
#if TITLE_MESH_LANES
    const Lanes first = FusedLanes(matrix[order[0]], point[order[0]], LoadLanes(matrix[3]));
    const Lanes second = FusedLanes(matrix[order[1]], point[order[1]], first);
    const Lanes third = FusedLanes(matrix[order[2]], point[order[2]], second);
    if (!AnyDenormal(first, second, third)) {
        Vector result;
        std::memcpy(result.data(), &third, sizeof(third));
        return result;
    }
#endif
#if TITLE_MESH_NEON
    float32x4_t value = vld1q_f32(matrix[3].data());
    for (unsigned row : order)
        value = FiniteLanes(vfmaq_n_f32(value, vld1q_f32(matrix[row].data()), point[row]));
    Vector result;
    vst1q_f32(result.data(), value);
    return result;
#else
    Vector result;
    for (unsigned lane = 0; lane != 4; ++lane) {
        float value = matrix[3][lane];
        for (unsigned row : order) value = Finite(VuFloat::Fma(matrix[row][lane], point[row], value));
        result[lane] = value;
    }
    return result;
#endif
}

inline ScreenVertex ProjectFlushed(const Vector& clip, const Vector& stq, const std::array<uint8_t, 4>& colour,
                                   const Vector& scroll, const Draw& state, bool draw) {
    const float reciprocal = Reciprocal(clip[3]);
    ScreenVertex result{};

    result.x = float(Integer(clip[0] * reciprocal) & 0xffff) / 16 - state.off_x;
    result.y = float(Integer(clip[1] * reciprocal) & 0xffff) / 16 - state.off_y;
    volatile uint32_t depth = Integer(clip[2] * reciprocal);
    result.z = float(depth);
#if TITLE_MESH_LANES

    const Lanes texture = FlushLanes(FlushLanes(LoadLanes(stq) + Lanes{scroll[0], scroll[1], 0, 0}) * reciprocal);
    result.s = texture[0];
    result.t = texture[1];
    result.q = texture[2];
#else
    result.s = Finite(Finite(stq[0] + scroll[0]) * reciprocal);
    result.t = Finite(Finite(stq[1] + scroll[1]) * reciprocal);
    result.q = Finite(Finite(stq[2] + 0.0f) * reciprocal);
#endif
    result.r = colour[0];
    result.g = colour[1];
    result.b = colour[2];
    result.a = colour[3];
    result.draw_kick = draw;
    return result;
}
}

MeshGeometry::Result MeshGeometry::Prepare(const Mesh& mesh, Prepared& out) {
    size_t count = 0;
    for (const auto& strip : mesh.strips) {
        const size_t size = strip.vertices.size();
        count += size;
        if (size < 3 || size > 38) return Result::InvalidGeometry;
        if (count > kMaxVertices) return Result::LimitExceeded;
        for (const auto& vertex : strip.vertices) {
            for (float value : vertex.position) if (!std::isfinite(value)) return Result::InvalidGeometry;
            for (float value : vertex.stq) if (!std::isfinite(value)) return Result::InvalidGeometry;
        }
    }

    Prepared& result = out;
    result.strips.reserve(mesh.strips.size());
    result.positions.reserve(count);
    result.stq.reserve(count);
    result.colours.reserve(count);
    result.restart.reserve(count);
    result.strips.clear();
    result.positions.clear();
    result.stq.clear();
    result.colours.clear();
    result.restart.clear();
    for (const auto& strip : mesh.strips) {
        result.strips.push_back({uint32_t(result.positions.size()), uint32_t(strip.vertices.size())});
        for (const auto& vertex : strip.vertices) {
            result.positions.push_back(FlushVector({vertex.position[0], vertex.position[1], vertex.position[2], 1}));
            result.stq.push_back(FlushVector({vertex.stq[0], vertex.stq[1], vertex.stq[2], 0}));
            result.colours.push_back(vertex.colour);
            result.restart.push_back(vertex.restart ? 1 : 0);
        }
    }
    result.vertex_count = count;
    return Result::Ok;
}

namespace {

struct CopiedBody {
    std::array<ScreenVertex, 38 * 3> list;
    size_t count = 0;
    void Add(const std::array<ScreenVertex, 38>& projected, size_t a, size_t b, size_t c) {
        list[count++] = projected[a];
        list[count++] = projected[b];
        list[count++] = projected[c];
    }
    void Flush(const std::array<ScreenVertex, 38>&, MeshGeometry::Sink sink, MeshGeometry::OrderSink, void* user) {
        if (count) sink(user, list.data(), count, false);
        count = 0;
    }
};
struct OrderedBody {
    std::array<uint8_t, 38 * 3> order;
    size_t count = 0;
    void Add(const std::array<ScreenVertex, 38>&, size_t a, size_t b, size_t c) {
        order[count++] = uint8_t(a);
        order[count++] = uint8_t(b);
        order[count++] = uint8_t(c);
    }
    void Flush(const std::array<ScreenVertex, 38>& projected, MeshGeometry::Sink, MeshGeometry::OrderSink body, void* user) {
        if (count) body(user, projected.data(), order.data(), count);
        count = 0;
    }
};

template <typename Body>
MeshGeometry::Result BuildPreparedWith(const MeshGeometry::Prepared& prepared, const MeshGeometry::View& view, const Draw& state,
                                       const std::array<uint8_t, 4>* colours, MeshGeometry::Sink sink,
                                       MeshGeometry::OrderSink ordered, void* user) {
    using Result = MeshGeometry::Result;
    if (!state.tex_enabled || state.use_fst || state.tex.fst || state.ctx || state.fog ||
        state.raster.frame.psm || state.raster.zbuf.psm || state.prim_type != 4 ||
        !std::isfinite(state.off_x) || !std::isfinite(state.off_y))
        return Result::UnsupportedState;
    for (const auto* matrix : {&view.screen, &view.clip})
        for (const auto& row : *matrix)
            for (float value : row) if (!std::isfinite(value)) return Result::InvalidGeometry;
    for (float value : view.guard) if (!std::isfinite(value)) return Result::InvalidGeometry;
    for (float value : view.scroll) if (!std::isfinite(value)) return Result::InvalidGeometry;
    if (!sink) return Result::InvalidGeometry;
    RoundingGuard rounding;
    const Matrix screen = FlushMatrix(view.screen), clip = FlushMatrix(view.clip);
    const Vector guard = FlushVector(view.guard);
    const Vector scroll{Finite(view.scroll[0]), Finite(view.scroll[1]), 0, 0};
    Body body;
    thread_local std::vector<ScreenVertex> fan_list;
    std::vector<ScreenVertex>& fan = fan_list;
    for (const auto& range : prepared.strips) {
        const size_t first = range.first, size = range.count;
        std::array<uint32_t, 3> hard{}, guarded{};
        std::array<ScreenVertex, 38> projected;
        unsigned parity = 0;

        std::array<bool, 38> needed{}, kicks{};
        std::array<uint8_t, 38 * 3> candidates;
        size_t candidate_count = 0;
        for (size_t i = 0; i < size; ++i) {
            const size_t at = first + i;
            const Vector& point = prepared.positions[at];
            const std::array<uint8_t, 4>& colour = colours ? colours[at] : prepared.colours[at];
            bool emit = !prepared.restart[at];
            if (view.clipped) {
                const Vector clipped = TransformFlushed(clip, point, false);
                Vector banded;
                for (unsigned lane = 0; lane != 4; ++lane) banded[lane] = Finite(clipped[lane] * guard[lane]);
                hard[i % 3] = Outside(clipped);
                guarded[i % 3] = Outside(banded);
                if (i >= 2) {
                    if (hard[0] & hard[1] & hard[2]) emit = false;
                    else if (guarded[0] | guarded[1] | guarded[2]) {
                        if (emit) {

                            std::array<Vertex, 3> triangle;
                            for (unsigned k = 0; k != 3; ++k) {
                                const size_t v = at - 2 + k;
                                triangle[k].position = {prepared.positions[v][0], prepared.positions[v][1], prepared.positions[v][2]};
                                triangle[k].stq = {prepared.stq[v][0], prepared.stq[v][1], prepared.stq[v][2]};
                                triangle[k].colour = colours ? colours[v] : prepared.colours[v];
                                triangle[k].restart = prepared.restart[v] != 0;
                            }
                            fan.clear();
                            Fan(fan, triangle, view, state);
                            if (!fan.empty()) sink(user, fan.data(), fan.size(), true);
                        }
                        emit = false;
                    }
                }
            }
            if (view.clipped) {
                kicks[i] = emit;
                if (!emit) parity = 0;
                else if (i >= 2) {
                    const size_t ia = i - 2 + (parity & 1), ib = i - 1 - (parity & 1);
                    needed[ia] = needed[ib] = needed[i] = true;
                    candidates[candidate_count++] = uint8_t(ia);
                    candidates[candidate_count++] = uint8_t(ib);
                    candidates[candidate_count++] = uint8_t(i);
                    ++parity;
                }
                continue;
            }
            projected[i] = ProjectFlushed(TransformFlushed(screen, point, true), prepared.stq[at], colour, scroll, state, emit);
            if (!emit) parity = 0;
            else if (i >= 2) {
                const size_t ia = i - 2 + (parity & 1), ib = i - 1 - (parity & 1);
                const ScreenVertex& a = projected[ia];
                const ScreenVertex& b = projected[ib];
                const ScreenVertex& c = projected[i];
                if (!SamePoint(a, b) && !SamePoint(b, c) && !SamePoint(a, c)) body.Add(projected, ia, ib, i);
                ++parity;
            }
        }
        if (view.clipped) {
            for (size_t i = 0; i < size; ++i) if (needed[i]) {
                const size_t at = first + i;
                const auto& colour = colours ? colours[at] : prepared.colours[at];
                projected[i] = ProjectFlushed(TransformFlushed(screen, prepared.positions[at], true),
                                             prepared.stq[at], colour, scroll, state, kicks[i]);
            }
            for (size_t i = 0; i < candidate_count; i += 3) {
                const size_t ia = candidates[i], ib = candidates[i + 1], ic = candidates[i + 2];
                const auto& a = projected[ia];
                const auto& b = projected[ib];
                const auto& c = projected[ic];
                if (!SamePoint(a, b) && !SamePoint(b, c) && !SamePoint(a, c)) body.Add(projected, ia, ib, ic);
            }
        }
        body.Flush(projected, sink, ordered, user);
    }
    return Result::Ok;
}
}

MeshGeometry::Result MeshGeometry::BuildPrepared(const Prepared& prepared, const View& view, const Draw& state,
                                                const std::array<uint8_t, 4>* colours, Sink sink, void* user) {
    return BuildPreparedWith<CopiedBody>(prepared, view, state, colours, sink, nullptr, user);
}

MeshGeometry::Result MeshGeometry::BuildPrepared(const Prepared& prepared, const View& view, const Draw& state,
                                                const std::array<uint8_t, 4>* colours, Sink sink, OrderSink body,
                                                void* user) {
    if (!body) return Result::InvalidGeometry;
    return BuildPreparedWith<OrderedBody>(prepared, view, state, colours, sink, body, user);
}

}
