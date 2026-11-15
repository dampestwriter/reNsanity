#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_geometry.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include <arm_neon.h>

#include "games/slus_20909_crash_twinsanity/hle/graphics/rounding_guard.h"

#include "engine/platform/host/platform.h"
#include "engine/vu/vu_float.h"

PS2R_FENV_ACCESS_ON

namespace title {
namespace {
using Vector = ShadowGeometry::Vector;
using Matrix = ShadowGeometry::Matrix;
using Vertex = ShadowGeometry::Vertex;
using Draw = ps2r::gs::DrawIr;
using Screen = ps2r::gs::VertexRec;
uint32_t Bits(float value) { uint32_t bits; std::memcpy(&bits, &value, 4); return bits; }
float Float(uint32_t bits) { float value; std::memcpy(&value, &bits, 4); return value; }
float Finite(float value) {
    const uint32_t bits = Bits(value), exponent = bits & 0x7f800000;
    if (!exponent) return Float(bits & 0x80000000);
    return exponent == 0x7f800000 ? Float((bits & 0x80000000) | 0x7f7fffff) : value;
}
float Product(float a, float b) { return Finite(Finite(a) * Finite(b)); }
float Add(float a, float b) { return Finite(Finite(a) + Finite(b)); }
float Madd(float a, float b, float c) { return Finite(VuFloat::Fma(Finite(a), Finite(b), Finite(c))); }
float Reciprocal(float value) {
    value = Finite(value);
    return value == 0 ? Float((Bits(value) & 0x80000000) | 0x7f7fffff) : Finite(1.f / value);
}
uint32_t Integer(float value) {
    value = Finite(value);
    if (value >= 2147483648.f) return 0x7fffffff;
    if (value < -2147483648.f) return 0x80000000;
    return uint32_t(int32_t(value));
}

#define TITLE_SHADOW_LANES 1
using Lanes = float __attribute__((vector_size(16)));
using LaneBits = uint32_t __attribute__((vector_size(16)));
inline Lanes LoadLanes(const Vector& v) {
    Lanes lanes;
    std::memcpy(&lanes, v.data(), sizeof(lanes));
    return lanes;
}
inline Vector StoreLanes(Lanes lanes) {
    Vector v;
    std::memcpy(v.data(), &lanes, sizeof(lanes));
    return v;
}

inline Lanes FiniteLanes(Lanes value) {
    const LaneBits bits = (LaneBits)value, exponent = bits & 0x7f800000u, sign = bits & 0x80000000u;
    const LaneBits zero = (LaneBits)(exponent == 0), full = (LaneBits)(exponent == 0x7f800000u);
    return (Lanes)((zero & sign) | (full & (sign | 0x7f7fffffu)) | (~(zero | full) & bits));
}

inline Lanes ProductLanes(Lanes a, Lanes b) { return FiniteLanes(FiniteLanes(a) * FiniteLanes(b)); }

template <int kLane>
inline Lanes MaddFlushed(Lanes row, Lanes point, Lanes sum) {
    return FiniteLanes((Lanes)vfmaq_laneq_f32((float32x4_t)sum, (float32x4_t)row, (float32x4_t)point, kLane));
}
struct FlushedMatrix {
    Lanes row[4];
};
inline FlushedMatrix Flush(const Matrix& m) {
    return {{FiniteLanes(LoadLanes(m[0])), FiniteLanes(LoadLanes(m[1])), FiniteLanes(LoadLanes(m[2])),
             FiniteLanes(LoadLanes(m[3]))}};
}

inline Lanes TransformFlushed(const FlushedMatrix& m, Lanes point, bool strip) {
    Lanes value = ProductLanes(m.row[3], Lanes{1, 1, 1, 1});
    if (strip) {
        value = MaddFlushed<1>(m.row[1], point, value);
        value = MaddFlushed<2>(m.row[2], point, value);
        return MaddFlushed<0>(m.row[0], point, value);
    }
    value = MaddFlushed<0>(m.row[0], point, value);
    value = MaddFlushed<1>(m.row[1], point, value);
    return MaddFlushed<2>(m.row[2], point, value);
}
inline Vector ProjectFlushed(const FlushedMatrix& m, Lanes point, bool strip) {
    const Lanes value = TransformFlushed(m, point, strip);
    const float inverse = Reciprocal(value[3]);
    const Lanes scaled = ProductLanes(value, Lanes{inverse, inverse, inverse, inverse});
    return {scaled[0], scaled[1], scaled[2], value[3]};
}
Vector Project(const Matrix& matrix, const Vector& position, bool strip) {
    return ProjectFlushed(Flush(matrix), FiniteLanes(LoadLanes(position)), strip);
}
Vector Difference(const Vector& a, const Vector& b) {

    const Lanes sum = FiniteLanes(FiniteLanes(LoadLanes(a)) + -FiniteLanes(LoadLanes(b)));
    return {sum[0], sum[1], sum[2], 0};
}
bool Reverse(const Vector& a, const Vector& b) {

    return std::signbit(Madd(-Finite(b[0]), a[1], Product(a[0], b[1])));
}
uint32_t Outside(const Vector& point) {
    const uint32_t wb = Bits(point[3]);
    const int32_t bound = int32_t(wb & 0x7f800000 ? wb & 0x7fffffff : 0x007fffff);
    uint32_t result = 0;
    for (unsigned lane = 0; lane < 3; ++lane) {
        const uint32_t bits = Bits(point[lane]), flipped = bits ^ 0x80000000;
        int32_t positive, negative;
        std::memcpy(&positive, &bits, 4);
        std::memcpy(&negative, &flipped, 4);
        if (positive > bound) result |= 1u << (lane * 2);
        if (negative > bound) result |= 2u << (lane * 2);
    }
    return result;
}
Vector Position(const Vertex& vertex) {
    return {vertex.position[0], vertex.position[1], vertex.position[2], 1};
}
Screen Quantize(const Vector& position, const ShadowGeometry::View& view, uint8_t colour, bool emit) {
    Screen result{};
    result.x = float(Integer(position[0]) & 0xffff) / 16 - view.offset[0];
    result.y = float(Integer(position[1]) & 0xffff) / 16 - view.offset[1];
    volatile uint32_t depth = Integer(Add(0, -Finite(position[2])));
    result.z = float(depth);
    result.r = result.g = result.b = colour;
    result.a = 128;
    result.q = 1;
    result.draw_kick = emit;
    return result;
}
void Triangle(Draw& draw, const Screen& a, const Screen& b, const Screen& c, bool degen_check) {
    if (degen_check && ((a.x == b.x && a.y == b.y) ||
        (a.x == c.x && a.y == c.y) || (b.x == c.x && b.y == c.y))) return;
    draw.verts.push_back(a);
    draw.verts.push_back(b);
    draw.verts.push_back(c);
}
std::vector<Vector> Clip(const std::array<Vertex, 3>& corners, const Matrix& matrix) {
    std::vector<Vector> polygon{Position(corners[0]), Position(corners[1]), Position(corners[2])};
    for (unsigned plane = 0; plane < 6 && !polygon.empty(); ++plane) {
        Vector row;
        for (unsigned i = 0; i < 4; ++i)
            row[i] = Add(matrix[i][3], (plane % 2 ? 1 : -1) * Finite(matrix[i][plane / 2]));
        const auto distance = [&](const Vector& position) {
            float value = Product(row[0], position[0]);
            value = Madd(row[1], position[1], value);
            value = Madd(row[2], position[2], value);
            return Madd(row[3], 1, value);
        };
        std::vector<Vector> next;
        Vector previous = polygon.back();
        float pd = distance(previous);
        for (const auto& current : polygon) {
            const float cd = distance(current);
            if (std::signbit(pd) != std::signbit(cd)) {
                const float inverse = Reciprocal(Add(cd, -Finite(pd)));
                Vector intersection;
                for (unsigned lane = 0; lane < 4; ++lane)
                    intersection[lane] = Product(Madd(-Finite(current[lane]), pd,
                        Product(previous[lane], cd)), inverse);
                next.push_back(intersection);
            }
            if (!std::signbit(cd)) next.push_back(current);
            previous = current;
            pd = cd;
        }
        polygon = std::move(next);
    }
    return polygon;
}
Draw Fan(const std::array<Vertex, 3>& vertices, const ShadowGeometry::View& view, bool winding) {
    Draw result;
    result.prim_type = 5;
    const auto polygon = Clip(vertices, view.clip);
    if (polygon.size() < 3) return result;
    std::vector<Vector> projected;
    for (const auto& vertex : polygon) projected.push_back(Project(view.screen, vertex, false));
    const bool reversed = vertices[2].reverse_fan;
    const auto a = reversed ? Difference(projected[0], projected[1])
                            : Difference(projected[1], projected[0]);
    const auto b = reversed ? Difference(projected[2], projected[0])
                            : Difference(projected[2], projected[1]);
    if (Reverse(a, b) != winding) return result;
    const uint8_t colour = winding ? 248 : 8;
    const auto first = Quantize(projected[0], view, colour, true);
    for (size_t i = 2; i < projected.size(); ++i)
        Triangle(result, first, Quantize(projected[i - 1], view, colour, true),
                 Quantize(projected[i], view, colour, true), false);
    return result;
}

struct Output {
    std::vector<Draw>* draws = nullptr;
    ShadowGeometry::Sink sink = nullptr;
    void* user = nullptr;
};
bool Publish(Draw& draw, size_t& total, const Output& to) {
    if (draw.verts.empty()) return true;
    total += draw.verts.size();
    if (total > ShadowGeometry::kMaxOutputVertices) return false;
    if (to.sink) {
        to.sink(to.user, draw.verts.data(), draw.verts.size(), draw.prim_type == 5);
        return true;
    }
    std::vector<Draw>& out = *to.draws;
    draw.vertex_count = uint32_t(draw.verts.size());
    draw.snapshot.verts_incl = 1;
    draw.snapshot.vertex_hash = ps2r::gs::GsdlFormat::VertexHash(draw.verts.data(), draw.verts.size());
    ps2r::gs::GsdlFormat::Bbox(draw.verts.data(), draw.verts.size(), draw.bbox);
    out.push_back(std::move(draw));
    return true;
}
struct StripCalc {std::array<Vector,38> projected{};std::array<uint32_t,38> hard{},guard{};bool valid=false;};
bool Instance(const ShadowGeometry::Strip& strip, const ShadowGeometry::View& view, bool winding,
              size_t& total, const Output& out, StripCalc* reuse) {

    thread_local Draw scratch;
    Draw fresh;
    Draw& body = out.sink ? scratch : fresh;
    body.verts.clear();
    body.prim_type = 4;
    std::array<uint32_t, 3> hard{}, guard{};
    std::array<Vector, 38> projected{};
    std::array<Screen, 38> quantized{};
    unsigned parity = 0;
#if TITLE_SHADOW_LANES
    const FlushedMatrix clip = Flush(view.clip), screen = Flush(view.screen);
    const Lanes guard_band = LoadLanes(view.guard);
#endif
    for (size_t i = 0; i < strip.vertices.size(); ++i) {
        const auto& vertex = strip.vertices[i];
        if (reuse && reuse->valid) {
            hard[i%3]=reuse->hard[i];guard[i%3]=reuse->guard[i];
        } else {
        const auto position = Position(vertex);
#if TITLE_SHADOW_LANES
        const Lanes point = FiniteLanes(LoadLanes(position));
        const Lanes clipped_lanes = TransformFlushed(clip, point, false);
        const Vector clipped = StoreLanes(clipped_lanes);
        const Vector guarded = StoreLanes(ProductLanes(clipped_lanes, guard_band));
#else
        const auto clipped = Transform(view.clip, position, false);
        Vector guarded;
        for (unsigned lane = 0; lane < 4; ++lane) guarded[lane] = Product(clipped[lane], view.guard[lane]);
#endif
        hard[i % 3] = Outside(clipped);
        guard[i % 3] = Outside(guarded);
        if(reuse){reuse->hard[i]=hard[i%3];reuse->guard[i]=guard[i%3];}
        }
        bool emit = !vertex.restart;
        if (i >= 2) {
            if (hard[0] & hard[1] & hard[2]) emit = false;
            else if (guard[0] | guard[1] | guard[2]) {
                if (emit) {
                    auto fan = Fan({strip.vertices[i - 2], strip.vertices[i - 1], vertex}, view, winding);
                    if (!Publish(fan, total, out)) return false;
                }
                emit = false;
            }
        }
        if(reuse && reuse->valid) projected[i]=reuse->projected[i];
        else {
#if TITLE_SHADOW_LANES
        const Lanes point = FiniteLanes(LoadLanes(Position(vertex)));
        projected[i] = ProjectFlushed(screen, point, true);
#else
        projected[i] = Project(view.screen, Position(vertex), true);
#endif
        if(reuse)reuse->projected[i]=projected[i];
        }
        if (i >= 2) {
            const auto previous = Difference(projected[i - 1], projected[i - 2]);
            const auto current = Difference(projected[i], projected[i - 1]);
            if ((i % 2 ? Reverse(current, previous) : Reverse(previous, current)) != winding)
                emit = false;
        }
        quantized[i] = Quantize(projected[i], view, winding ? 248 : 8, emit);
        if (!emit) parity = 0;
        else if (i >= 2) {
            Triangle(body, quantized[i - 2 + (parity & 1)], quantized[i - 1 - (parity & 1)],
                     quantized[i], true);
            ++parity;
        }
    }
    if(reuse)reuse->valid=true;
    return Publish(body, total, out);
}
}
ShadowGeometry::Result ShadowGeometry::Decode(const uint8_t* bytes, size_t size,
                                              uint32_t vertices, Mesh& out) {
    if (!bytes || size < 16 || size > 4 * 1024 * 1024 || vertices < 3)
        return Result::InvalidAsset;
    if (vertices > kMaxVertices) return Result::LimitExceeded;
    size_t cursor = 0, limit = size;
    const auto read = [&](uint32_t& value) {
        if (cursor > limit || limit - cursor < 4) return false;
        value = uint32_t(bytes[cursor]) | uint32_t(bytes[cursor + 1]) << 8 |
                uint32_t(bytes[cursor + 2]) << 16 | uint32_t(bytes[cursor + 3]) << 24;
        cursor += 4;
        return true;
    };
    const auto expect = [&](uint32_t value) {
        uint32_t actual;
        return read(actual) && actual == value;
    };
    uint32_t header;
    if (!read(header) || (header >> 16) != 0x6000 || !expect(0) ||
        !expect(0x070000fa) || !expect(0)) return Result::InvalidAsset;
    limit = 16 + size_t(header & 0xffff) * 16;
    if (limit > size || size - limit > 36) return Result::InvalidAsset;
    for (size_t i = limit; i < size; ++i) if (bytes[i]) return Result::InvalidAsset;
    Mesh mesh;
    uint32_t total = 0;
    while (cursor < limit) {
        uint32_t command;
        if (!read(command)) return Result::InvalidAsset;
        if (!command) {
            for (size_t i = cursor; i < limit; ++i) if (bytes[i]) return Result::InvalidAsset;
            break;
        }
        if (command != 0x6c018000) return Result::InvalidAsset;
        uint32_t count;
        if (!read(count) || (count & ~0xffu) != 0x8000) return Result::InvalidAsset;
        count &= 255;
        if (count < 3 || count > 38 || total + count > vertices ||
            !expect(0x30024000) || !expect(0x512) || !expect(0) ||
            !expect(0x01000101) || !expect(0x64018001) || !expect(count * 4) ||
            !expect(count | 0x8000) || !expect(0x01000104) ||
            !expect(0x68008003 | count << 16)) return Result::InvalidAsset;
        Strip strip;
        strip.vertices.resize(count);
        for (auto& vertex : strip.vertices)
            for (float& coordinate : vertex.position) {
                uint32_t bits;
                if (!read(bits) || !std::isfinite(coordinate = Float(bits))) return Result::InvalidAsset;
            }
        if (!expect(0x6c008004 | count << 16)) return Result::InvalidAsset;
        for (auto& vertex : strip.vertices) {
            uint32_t word;
            for (unsigned lane = 0; lane < 4; ++lane) {
                if (!read(word)) return Result::InvalidAsset;
                if (lane == 3) vertex.restart = word & 0x8000;
            }
        }

        if (!expect(0x68008005 | count << 16)) return Result::InvalidAsset;
        for (auto& vertex : strip.vertices) {
            uint32_t word;
            for (unsigned lane = 0; lane < 3; ++lane) {
                if (!read(word)) return Result::InvalidAsset;
                if (!lane) vertex.reverse_fan = word & 32;
            }
        }
        if (!expect(0x14000000) || !expect(0x01000101)) return Result::InvalidAsset;
        total += count;
        mesh.strips.push_back(std::move(strip));
    }
    if (total != vertices) return Result::InvalidAsset;
    out = std::move(mesh);
    return Result::Ok;
}
namespace {
ShadowGeometry::Result BuildViews(const ShadowGeometry::Mesh& mesh, const ShadowGeometry::View* views, size_t view_count,
                                  const Output& to) {
    using Result = ShadowGeometry::Result;
    if (!view_count || view_count > 20) return Result::InvalidGeometry;
    for (size_t v = 0; v < view_count; ++v) {
        const auto& view = views[v];
        for (const auto* matrix : {&view.screen, &view.clip})
            for (const auto& row : *matrix)
                for (float value : row) if (!std::isfinite(value)) return Result::InvalidGeometry;
        for (float value : view.guard) if (!std::isfinite(value)) return Result::InvalidGeometry;
        for (float value : view.offset) if (!std::isfinite(value)) return Result::InvalidGeometry;
    }
    size_t vertices = 0, total = 0;
    for (const auto& strip : mesh.strips) {
        if (strip.vertices.size() < 3 || strip.vertices.size() > 38) return Result::InvalidGeometry;
        vertices += strip.vertices.size();
        if (vertices > ShadowGeometry::kMaxVertices) return Result::LimitExceeded;
        for (const auto& vertex : strip.vertices)
            for (float value : vertex.position) if (!std::isfinite(value)) return Result::InvalidGeometry;
    }
    RoundingGuard rounding;
    std::vector<Draw> output;
    Output into = to;
    if (to.draws) into.draws = &output;
    thread_local std::array<StripCalc,20> cached;
    for (const auto& strip : mesh.strips) {
        for(size_t v=0;v<view_count;++v)cached[v].valid=false;
        for (bool winding : {false, true})
            for (size_t v = 0; v < view_count; ++v)
                if (!Instance(strip, views[v], winding, total, into, &cached[v])) return Result::LimitExceeded;
    }
    if (to.draws) *to.draws = std::move(output);
    return Result::Ok;
}
}

ShadowGeometry::Result ShadowGeometry::Build(const Mesh& mesh, const std::vector<View>& views,
                                             std::vector<Draw>& out) {
    Output to;
    to.draws = &out;
    return BuildViews(mesh, views.data(), views.size(), to);
}

ShadowGeometry::Result ShadowGeometry::Build(const Mesh& mesh, const View* views, size_t count, Sink sink, void* user) {
    if (!sink) return Result::InvalidGeometry;
    Output to;
    to.sink = sink;
    to.user = user;
    return BuildViews(mesh, views, count, to);
}
ShadowGeometry::Result ShadowGeometry::GpuTriangles(const Mesh& mesh, std::vector<std::array<float, 3>>& out,
                                                    std::array<float, 6>& box) {
    out.clear();
    box = {1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f};
    for (const Strip& strip : mesh.strips)
        for (size_t i = 0; i < strip.vertices.size(); ++i) {
            const auto& p = strip.vertices[i].position;
            for (int k = 0; k < 3; ++k) { box[size_t(k)] = std::min(box[size_t(k)], p[size_t(k)]); box[size_t(k) + 3] = std::max(box[size_t(k) + 3], p[size_t(k)]); }
            if (i < 2 || strip.vertices[i].restart) continue;
            const size_t a = i % 2 ? i - 1 : i - 2, b = i % 2 ? i - 2 : i - 1;
            out.push_back(strip.vertices[a].position);
            out.push_back(strip.vertices[b].position);
            out.push_back(strip.vertices[i].position);
            if (out.size() > kMaxOutputVertices) return Result::LimitExceeded;
        }
    return out.empty() ? Result::InvalidGeometry : Result::Ok;
}

bool ShadowGeometry::InsideGuard(const std::array<float, 6>& box, const View& view) {
#if TITLE_SHADOW_LANES
    const FlushedMatrix clip = Flush(view.clip);
    const Lanes guard_band = LoadLanes(view.guard);
#endif
    for (int c = 0; c < 8; ++c) {
        const Vector position{box[(c & 1) ? 3 : 0], box[(c & 2) ? 4 : 1], box[(c & 4) ? 5 : 2], 1.f};
#if TITLE_SHADOW_LANES
        const Lanes clipped_lanes = TransformFlushed(clip, FiniteLanes(LoadLanes(position)), false);
        const Vector guarded = StoreLanes(ProductLanes(clipped_lanes, guard_band));
#else
        const auto clipped = Transform(view.clip, position, false);
        Vector guarded;
        for (unsigned lane = 0; lane < 4; ++lane) guarded[lane] = Product(clipped[lane], view.guard[lane]);
#endif

        Vector shrunk = guarded;
        for (unsigned lane = 0; lane < 3; ++lane) shrunk[lane] = guarded[lane] * (1.f + 1.f / 256.f);
        if (!(guarded[3] > 0.f) || Outside(shrunk)) return false;
    }
    return true;
}

}
