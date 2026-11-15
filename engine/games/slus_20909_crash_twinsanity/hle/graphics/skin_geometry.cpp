#include "games/slus_20909_crash_twinsanity/hle/graphics/skin_geometry.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "engine/vu/vu_chop_round_guard.h"
#include "engine/vu/vu_flags_exact.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/qw_ptr.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/skinned_lit_strip.h"
#include <arm_neon.h>

namespace title {
namespace {
using Qw = SkinGeometry::Qw;
using Draw = ps2r::gs::DrawIr;
using Vertex = ps2r::gs::VertexRec;

uint32_t Word(const uint8_t* bytes, size_t at) {
    uint32_t value;
    std::memcpy(&value, bytes + at, 4);
    return value;
}

uint32_t Widen(const uint8_t* at, unsigned bits) {
    if (bits == 32) return Word(at, 0);
    if (bits == 16) {
        uint16_t raw;
        std::memcpy(&raw, at, 2);
        return uint32_t(int32_t(int16_t(raw)));
    }
    return uint32_t(int32_t(int8_t(*at)));
}

struct BatchImage {
    std::vector<Qw> qw;
    std::vector<bool> written;
};

bool SetImageQw(BatchImage& image, uint32_t at, const Qw& value) {
    if (at >= image.qw.size()) return false;
    image.qw[at] = value;
    image.written[at] = true;
    return true;
}

struct StripKicks {
    Vertex ring[3];
    unsigned held = 0, oldest = 0, parity = 0;
    Vertex* out;
    size_t count = 0;
    explicit StripKicks(Vertex* list) : out(list) {}
    void Kick(const Vertex& v, bool draw) {
        if (held < 3) {
            ring[held++] = v;
        } else {
            ring[oldest] = v;
            oldest = oldest == 2 ? 0 : oldest + 1;
        }
        if (!draw) {
            parity = 0;
            return;
        }
        if (held != 3) return;
        const Vertex& q0 = ring[oldest];
        const Vertex& q1 = ring[oldest == 2 ? 0 : oldest + 1];
        const Vertex& q2 = ring[oldest == 0 ? 2 : oldest - 1];
        const bool degenerate = (q0.x == q1.x && q0.y == q1.y) || (q1.x == q2.x && q1.y == q2.y) ||
                                (q0.x == q2.x && q0.y == q2.y);
        if (!degenerate) {
            out[count] = parity % 2 == 0 ? q0 : q1;
            out[count + 1] = parity % 2 == 0 ? q1 : q0;
            out[count + 2] = q2;
            count += 3;
        }
        ++parity;
    }
};

struct BatchOutput {
    std::vector<Draw>* draws = nullptr;
    SkinGeometry::Sink sink = nullptr;
    void* user = nullptr;
};

Vertex* BatchList() {
    thread_local std::vector<Vertex> list(size_t(SkinGeometry::kMaxBatchVertices) * 3);
    return list.data();
}

constexpr uint32_t kJointsBase = 13;
constexpr uint32_t kSkinData = 78;
constexpr uint32_t kSkinClip = 11;
constexpr uint32_t kEyeRow = 77;
constexpr uint32_t kScaleSkinPc = 0xcb0 / 8, kSkin1040Pc = 0x1040 / 8, kClipMarkPc = 0xb30 / 8, kEmitterPc = 0x1720 / 8,
                   kExitPc = 0x90 / 8;
constexpr uint32_t kQwCount = 1024;
}

namespace {

SkinGeometry::Result CloseBatch(BatchImage& image, uint32_t vertices, uint32_t& total, std::vector<SkinGeometry::Batch>& batches) {
    using Result = SkinGeometry::Result;
    SkinGeometry::Batch batch;
    batch.tag = image.qw[0];
    batch.count = image.qw[1];
    batch.scale = image.qw[2];
    const uint32_t n = batch.count[0] & 0xffff;
    if (!image.written[0] || !image.written[1] || !image.written[2] || n < 12 || (n & 3) ||
        n / 4 > SkinGeometry::kMaxBatchVertices)
        return Result::InvalidAsset;
    const uint32_t count = n / 4;

    if ((batch.tag[0] & 0x7fff) != count || !(batch.tag[0] & 0x8000) || batch.tag[1] != 0x30024000 ||
        batch.tag[2] != 0x512 || batch.tag[3] != 0)
        return Result::InvalidAsset;
    for (uint32_t q = 3; q < 3 + 4 * count; ++q) if (!image.written[q]) return Result::InvalidAsset;
    batch.vertices.assign(image.qw.begin() + 3, image.qw.begin() + 3 + 4 * count);
    total += count;
    if (vertices && total > vertices) return Result::InvalidAsset;
    batches.push_back(std::move(batch));
    std::fill(image.written.begin(), image.written.end(), false);
    return Result::Ok;
}

SkinGeometry::Result DecodeStream(const uint8_t* bytes, size_t size, uint32_t vertices, bool terminated, SkinGeometry::Part& out) {
    using Result = SkinGeometry::Result;
    using Part = SkinGeometry::Part;

    if (!bytes || size < 32 || size > 4 * 1024 * 1024 || (vertices && vertices < 3)) return Result::InvalidAsset;
    if (vertices > SkinGeometry::kMaxVertices) return Result::LimitExceeded;

    const uint32_t tag = Word(bytes, 0);
    const uint32_t id = (tag >> 28) & 7;
    if ((id != 6 && id != 7) || Word(bytes, 4) != 0) return Result::InvalidAsset;
    const size_t limit = 16 + size_t(tag & 0xffff) * 16;
    if (limit > size) return Result::InvalidAsset;
    Part part;
    uint32_t total = 0, cl = 1, wl = 1, mode = 0;
    Qw row{};
    BatchImage image;
    image.qw.assign(3 + 4 * SkinGeometry::kMaxBatchVertices, Qw{});
    image.written.assign(image.qw.size(), false);
    bool any = false;
    size_t cursor = 8;
    while (cursor + 4 <= limit) {
        const uint32_t code = Word(bytes, cursor);
        cursor += 4;
        const uint32_t cmd = (code >> 24) & 0x7f;
        if (code & 0x80000000u) return Result::InvalidAsset;
        if (cmd == 0x00 || cmd == 0x07 || cmd == 0x10 || cmd == 0x11 || cmd == 0x13) continue;
        if (cmd == 0x01) {
            cl = code & 0xff;
            wl = (code >> 8) & 0xff;
            if (!cl || !wl || wl > cl) return Result::InvalidAsset;
            continue;
        }
        if (cmd == 0x05) {
            mode = code & 3;
            if (mode > 1) return Result::InvalidAsset;
            continue;
        }
        if (cmd == 0x30) {
            if (cursor + 16 > limit) return Result::InvalidAsset;
            for (unsigned k = 0; k < 4; ++k) row[k] = Word(bytes, cursor + 4 * k);
            cursor += 16;
            continue;
        }
        if (cmd == 0x14) {
            if ((code & 0xffff) != 0 || !any || !terminated) return Result::InvalidAsset;
            const Result closed = CloseBatch(image, vertices, total, part.batches);
            if (closed != Result::Ok) return closed;
            any = false;
            continue;
        }
        if ((cmd & 0x60) != 0x60) return Result::InvalidAsset;
        const uint32_t format = cmd & 0xf;
        const bool masked = (cmd >> 4) & 1;
        uint32_t num = (code >> 16) & 0xff;
        if (!num) num = 256;
        const uint32_t imm = code & 0xffff;
        const uint32_t address = imm & 0x3ff;
        const bool usn = (imm >> 14) & 1, tops = (imm >> 15) & 1;
        unsigned bits = 0, elements = 0;
        switch (format) {
            case 0xc: bits = 32; elements = 4; break;
            case 0x4: bits = 32; elements = 2; break;
            case 0xd: bits = 16; elements = 4; break;
            case 0xe: bits = 8; elements = 4; break;
            default: return Result::InvalidAsset;
        }
        if (masked || usn || !tops) return Result::InvalidAsset;
        const size_t payload = (size_t(num) * elements * bits / 8 + 3) & ~size_t(3);
        if (cursor + payload > limit) return Result::InvalidAsset;

        uint32_t at = address, write_cycle = 0;
        const uint8_t* src = bytes + cursor;
        for (uint32_t k = 0; k < num; ++k) {
            Qw vec{};
            for (unsigned e = 0; e < elements; ++e) vec[e] = Widen(src + (size_t(k) * elements + e) * (bits / 8), bits);
            if (elements == 2) {
                vec[2] = vec[0];
                vec[3] = vec[1];
            }
            if (mode == 1) for (unsigned f = 0; f < 4; ++f) vec[f] += row[f];
            if (!SetImageQw(image, at, vec)) return Result::InvalidAsset;
            any = true;
            ++at;
            if (++write_cycle >= wl) {
                write_cycle = 0;
                at += cl - wl;
            }
        }
        cursor += payload;
    }
    if (cursor != limit) return Result::InvalidAsset;
    if (!terminated) {
        if (!any || !part.batches.empty()) return Result::InvalidAsset;
        const Result closed = CloseBatch(image, vertices, total, part.batches);
        if (closed != Result::Ok) return closed;
        any = false;
    }
    if (any || part.batches.empty() || (vertices && total != vertices)) return Result::InvalidAsset;
    part.vertices = total;
    out = std::move(part);
    return Result::Ok;
}
}

SkinGeometry::Result SkinGeometry::Decode(const uint8_t* bytes, size_t size, uint32_t vertices, Part& out) {
    if (vertices < 3) return Result::InvalidAsset;
    return DecodeStream(bytes, size, vertices, true, out);
}

SkinGeometry::Result SkinGeometry::DecodePart(const uint8_t* bytes, size_t size, uint32_t vertices, Part& out) {
    return DecodeStream(bytes, size, vertices, false, out);
}

namespace {

struct BatchProgram {
    const SkinGeometry::BlendInputs* blend = nullptr;
};

bool RunDisplace(QwPtr m, uint32_t top, const SkinGeometry::Shape& shape, const std::array<float, 3>& factors, bool first) {
    const uint32_t count = m.W(top + 1, 0) & 0xFFFF;
    if (count == 0 || (count & 3) != 0) return false;
    const uint32_t vertices = count / 4;
    if (!shape.rows || shape.count < vertices) return false;
    const float sx = vu1lib::CF(m.F(top + 2, 0)), sy = vu1lib::CF(m.F(top + 2, 1));
    const float r8[4] = {vu1lib::CF(factors[0]), vu1lib::CF(factors[1]), vu1lib::CF(factors[2]), vu1lib::CF(shape.weight)};
    for (uint32_t j = 0; j < vertices; ++j) {
        const uint32_t v = top + 3 + 4 * j;
        float f5[4], f6[4], f9[4];
        for (int k = 0; k < 4; ++k) {
            const int8_t d = int8_t(shape.rows[size_t(j) * 4 + size_t(k)]);
            f9[k] = float(int32_t(d));
            if (first) {
                f5[k] = float(int32_t(m.W(v, k)));
                f6[k] = float(int32_t(m.W(v + 1, k)));
            }
        }
        if (first) {
            for (int k = 0; k < 4; ++k) f5[k] = vu1lib::CF(vu1lib::CF(f5[k]) * sx);
            for (int k = 0; k < 4; ++k) f6[k] = vu1lib::CF(vu1lib::CF(f6[k]) * sy);
        }
        for (int k = 0; k < 3; ++k) f9[k] = vu1lib::CF(vu1lib::CF(f9[k]) * r8[k]);
        for (int k = 0; k < 3; ++k) f9[k] = vu1lib::CF(vu1lib::CF(f9[k]) * r8[3]);
        for (int k = 0; k < 3; ++k) {
            const float p = first ? vu1lib::CF(f5[k]) : vu1lib::CF(vu1lib::bits2f(m.W(v, k)));
            const float res = vu1lib::CF(vu1lib::CF(f9[k]) + p);
            m.SetW(v, k, vu1lib::f2bits(res));
        }
        if (first) {
            m.SetW(v, 3, vu1lib::f2bits(f5[3]));
            for (int k = 0; k < 4; ++k) m.SetW(v + 1, k, vu1lib::f2bits(f6[k]));
        }
    }
    return true;
}

SkinGeometry::Result FastSkin(const SkinGeometry::Part& part, const SkinGeometry::Inputs& in, const Draw& state, const BatchOutput& out) {
    using Result = SkinGeometry::Result;
    const auto F = [](uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; };
    float screen[4][4], light[3][3], colour[3][3], ambient[3];
    for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 4; ++k) screen[r][k] = F(in.to_screen[r][k]);
    for (int l = 0; l < 3; ++l)
        for (int k = 0; k < 3; ++k) { light[l][k] = F(in.lights[l][k]); colour[l][k] = F(in.colours[l][k]); }
    for (int k = 0; k < 3; ++k) ambient[k] = F(in.ambient[k]);
    const size_t palette = in.joints.size();
    std::vector<Draw> result;
    Vertex* const list = BatchList();
    size_t output = 0;
    for (const auto& batch : part.batches) {
        const uint32_t count = batch.Vertices();
        if (count < 3 || count > SkinGeometry::kMaxBatchVertices || batch.vertices.size() != 4 * size_t(count))
            return Result::InvalidGeometry;
        const float sx = F(batch.scale[0]), sy = F(batch.scale[1]);
        StripKicks strip(list);
        for (uint32_t j = 0; j < count; ++j) {
            const auto& q0 = batch.vertices[4 * j];
            const auto& q1 = batch.vertices[4 * j + 1];
            const auto& q2 = batch.vertices[4 * j + 2];
            const auto& q3 = batch.vertices[4 * j + 3];
            const float px = float(int32_t(q0[0])) * sx, py = float(int32_t(q0[1])) * sx, pz = float(int32_t(q0[2])) * sx;
            const float nx = float(int32_t(q0[3])) * sx, ny = float(int32_t(q1[2])) * sy, nz = float(int32_t(q1[3])) * sy;
            const float s0 = float(int32_t(q1[0])) * sy, t0 = float(int32_t(q1[1])) * sy;
            const uint32_t flags = q2[3] & 0xFFFFu;
            const uint32_t bones = flags & 3u;
            const int used = bones == 1 ? 1 : bones == 2 ? 2 : 3;
            float po[3] = {0.f, 0.f, 0.f}, n[3] = {0.f, 0.f, 0.f};
            for (int b = 0; b < used; ++b) {
                const size_t at = q2[b] & 0x1FFu;
                if (at + 3 >= palette) continue;
                const float w = F(q2[b]);
                const auto& c0 = in.joints[at];
                const auto& c1 = in.joints[at + 1];
                const auto& c2 = in.joints[at + 2];
                const auto& tr = in.joints[at + 3];
                for (int k = 0; k < 3; ++k)
                    po[k] += w * (F(tr[k]) + F(c0[k]) * px + F(c1[k]) * py + F(c2[k]) * pz);
                if (b == 0)
                    for (int k = 0; k < 3; ++k) n[k] = F(c0[k]) * nx + F(c1[k]) * ny + F(c2[k]) * nz;
            }
            float T[4];
            for (int k = 0; k < 4; ++k) T[k] = screen[3][k] + screen[0][k] * po[0] + screen[1][k] * po[1] + screen[2][k] * po[2];
            const float Q = T[3] != 0.f ? 1.0f / T[3] : 3.4028235e38f;
            float lit[3] = {ambient[0], ambient[1], ambient[2]};
            for (int l = 0; l < 3; ++l) {
                const float d = std::max(0.f, light[l][0] * n[0] + light[l][1] * n[1] + light[l][2] * n[2]);
                for (int k = 0; k < 3; ++k) lit[k] += colour[l][k] * d;
            }
            Vertex v{};
            const auto channel = [](float value) { return uint8_t(int32_t(std::min(value, 255.f))); };
            v.r = channel(lit[0] * float(int32_t(q3[0])));
            v.g = channel(lit[1] * float(int32_t(q3[1])));
            v.b = channel(lit[2] * float(int32_t(q3[2])));
            v.a = uint8_t(std::min<int32_t>(int32_t(q3[3]), 255));
            v.s = (s0 + in.scroll[0]) * Q;
            v.t = (t0 + in.scroll[1]) * Q;
            v.q = Q;
            const auto fixed = [](float value) {
                if (!(std::fabs(value) < 2147483648.f)) return value < 0.f ? 0x80000000u : 0x7FFFFFFFu;
                return uint32_t(int32_t(value));
            };
            v.x = float(uint16_t(fixed(T[0] * Q))) / 16.0f - state.off_x;
            v.y = float(uint16_t(fixed(T[1] * Q))) / 16.0f - state.off_y;
            v.z = float(fixed(T[2] * Q));
            const bool adc = (flags & 0x8000u) != 0;
            v.draw_kick = adc ? 0 : 1;
            strip.Kick(v, !adc);
        }
        output += strip.count;
        if (output > SkinGeometry::kMaxVertices * 24) return Result::LimitExceeded;
        if (!strip.count) continue;
        if (out.sink) {
            out.sink(out.user, list, strip.count, false);
            continue;
        }
        Draw draw = state;
        draw.verts.assign(list, list + strip.count);
        draw.vertex_count = uint32_t(draw.verts.size());
        result.push_back(std::move(draw));
    }
    if (out.draws) out.draws->insert(out.draws->end(), std::make_move_iterator(result.begin()), std::make_move_iterator(result.end()));
    return Result::Ok;
}

#include "games/slus_20909_crash_twinsanity/hle/graphics/skin_behind2.h"

SkinGeometry::Result BuildBatchesReference(const SkinGeometry::Part& part, const SkinGeometry::Inputs& inputs, const BatchProgram& program,
                                  const Draw& state, const BatchOutput& out) {
    using Result = SkinGeometry::Result;
    if (!state.tex_enabled || state.use_fst || state.tex.fst || state.ctx || state.fog || state.prim_type != 4 ||
        state.raster.frame.psm || state.raster.zbuf.psm)
        return Result::UnsupportedState;
    if (inputs.joints.empty() || (inputs.joints.size() & 3) || inputs.joints.size() / 4 > SkinGeometry::kMaxJoints ||
        part.batches.empty())
        return Result::InvalidGeometry;

    if (!program.blend && !inputs.clipped) return FastSkin(part, inputs, state, out);

    const uint32_t joints_end = kJointsBase + uint32_t(inputs.joints.size());
    const uint32_t buffer = (joints_end + 15) & ~15u;
    const uint32_t top = buffer + 0x60;
    const uint32_t list = top + 3 + 4 * SkinGeometry::kMaxBatchVertices + 1;
    if (list + 1 + 3 * SkinGeometry::kMaxBatchVertices > kQwCount) return Result::InvalidGeometry;

    thread_local std::vector<uint8_t> memory(kQwCount * 16);
    std::fill(memory.begin(), memory.end(), uint8_t(0));
    QwPtr m{memory.data()};
    const auto set = [&](uint32_t qw, const Qw& value) { for (int k = 0; k < 4; ++k) m.SetW(qw, k, value[k]); };
    const auto f2bits = [](float value) { uint32_t bits; std::memcpy(&bits, &value, 4); return bits; };
    set(1, {list, 0, 0, 0});
    set(3, {buffer, 0, 0, 0});
    for (size_t j = 0; j < inputs.joints.size(); ++j) set(kJointsBase + uint32_t(j), inputs.joints[j]);

    uint32_t node = buffer;
    set(node, {program.blend ? kSkin1040Pc : kScaleSkinPc, 0, node + 1, 0});
    ++node;
    if (inputs.clipped) {
        set(node, {kClipMarkPc, 0, node + 1, 0});
        ++node;
    }
    const uint32_t emitter = node;
    set(emitter, {kEmitterPc, 0, emitter + 4, 1});
    set(emitter + 1, {0x80, 0x80, 0x80, 0x80});
    set(emitter + 2, {f2bits(inputs.scroll[0]), f2bits(inputs.scroll[1]), 0, 0});
    set(emitter + 3, {0x00008000, 0x10000000, 0xe, 0});
    set(emitter + 4, {kExitPc, 0, 0, 0});
    if (emitter + 4 >= buffer + kEyeRow) return Result::InvalidGeometry;

    for (uint32_t r = 0; r < 4; ++r) set(buffer + kSkinData + r, inputs.to_screen[r]);
    for (uint32_t r = 0; r < 3; ++r) {
        Qw column{};
        for (uint32_t k = 0; k < 3; ++k) column[k] = inputs.lights[k][r];
        set(buffer + kSkinData + 4 + r, column);
    }
    for (uint32_t r = 0; r < 3; ++r) set(buffer + kSkinData + 7 + r, inputs.colours[r]);
    set(buffer + kSkinData + 10, inputs.ambient);
    if (inputs.clipped) {
        for (uint32_t r = 0; r < 4; ++r) set(buffer + kSkinData + kSkinClip + r, inputs.to_camera[r]);
        set(buffer + kSkinData + kSkinClip + 4, inputs.clip);
    }
    std::vector<Draw> result;
    size_t output = 0;
    VuChopRoundGuard rounding;
    Vertex* const list_out = BatchList();
    for (const auto& batch : part.batches) {
        const uint32_t count = batch.Vertices();
        if (count < 3 || count > SkinGeometry::kMaxBatchVertices || batch.vertices.size() != 4 * size_t(count))
            return Result::InvalidGeometry;

        std::memset(memory.data() + size_t(top) * 16, 0, size_t(list - top) * 16);
        set(top, batch.tag);
        set(top + 1, batch.count);
        set(top + 2, batch.scale);
        for (uint32_t q = 0; q < 4 * count; ++q) set(top + 3 + q, batch.vertices[q]);
        std::memset(memory.data() + size_t(list) * 16, 0, size_t(1 + 3 * SkinGeometry::kMaxBatchVertices) * 16);
        if (!program.blend) {
            if (!SkinnedLitStrip::RunSkinCb0(m, top)) return Result::InvalidGeometry;
        } else {
            const auto& blend = *program.blend;
            if (blend.shapes.empty()) {
                if (!SkinnedLitStrip::RunScalePhase(m, top, nullptr)) return Result::InvalidGeometry;
            } else {
                for (size_t i = 0; i < blend.shapes.size(); ++i)
                    if (!RunDisplace(m, top, blend.shapes[i], blend.factors, i == 0)) return Result::InvalidGeometry;
            }
            if (!SkinnedLitStrip::RunSkin1040(m, top, 0)) return Result::InvalidGeometry;
        }
        if (inputs.clipped && !SkinnedLitStrip::RunClipMark0B30(m, top)) return Result::InvalidGeometry;

        if (!SkinnedLitStrip::RunSkinnedLitBody(m, emitter, top)) return Result::InvalidGeometry;

        StripKicks strip(list_out);
        for (uint32_t j = 0; j < count; ++j) {
            const uint32_t st = list + 1 + 3 * j;
            Vertex v{};
            v.s = m.F(st, 0);
            v.t = m.F(st, 1);
            v.q = m.F(st, 2);
            v.r = uint8_t(m.W(st + 1, 0));
            v.g = uint8_t(m.W(st + 1, 1));
            v.b = uint8_t(m.W(st + 1, 2));
            v.a = uint8_t(m.W(st + 1, 3));
            v.x = float(uint16_t(m.W(st + 2, 0))) / 16.0f - state.off_x;
            v.y = float(uint16_t(m.W(st + 2, 1))) / 16.0f - state.off_y;
            v.z = float(m.W(st + 2, 2));
            const bool adc = (m.W(st + 2, 3) >> 15) & 1;
            v.draw_kick = adc ? 0 : 1;
            strip.Kick(v, !adc);
        }
        output += strip.count;
        if (output > SkinGeometry::kMaxVertices * 24) return Result::LimitExceeded;
        if (!strip.count) continue;
        if (out.sink) {
            out.sink(out.user, list_out, strip.count, false);
            continue;
        }
        Draw draw = state;
        draw.verts.assign(list_out, list_out + strip.count);
        draw.vertex_count = uint32_t(draw.verts.size());
        result.push_back(std::move(draw));
    }
    if (out.draws) out.draws->insert(out.draws->end(), std::make_move_iterator(result.begin()), std::make_move_iterator(result.end()));
    return Result::Ok;
}

SkinGeometry::Result BuildBatches(const SkinGeometry::Part& part, const SkinGeometry::Inputs& inputs, const BatchProgram& program,
                                  const Draw& state, const BatchOutput& out) {
    if (!inputs.clipped)
        return BuildBatchesReference(part,inputs,program,state,out);
    thread_local behind_skin::Output candidate;
    behind_skin::Attempt attempt;
    { VuChopRoundGuard rounding; attempt=behind_skin::Run(part,inputs,program,state,candidate); }
    if (attempt!=behind_skin::Attempt::Clean) return BuildBatchesReference(part,inputs,program,state,out);
    size_t at=0;
    for(size_t end:candidate.ends){
        if(out.sink)out.sink(out.user,candidate.verts.data()+at,end-at,false);
        else if(out.draws){Draw d=state;d.verts.assign(candidate.verts.begin()+at,candidate.verts.begin()+end);d.vertex_count=uint32_t(end-at);out.draws->push_back(std::move(d));}
        at=end;
    }
    return SkinGeometry::Result::Ok;
}
}

SkinGeometry::Result SkinGeometry::Build(const Part& part, const Inputs& inputs, const Draw& state,
                                         std::vector<Draw>& out) {
    BatchOutput to;
    to.draws = &out;
    return BuildBatches(part, inputs, BatchProgram{}, state, to);
}

SkinGeometry::Result SkinGeometry::BuildBlend(const Part& part, const Inputs& inputs, const BlendInputs& blend,
                                              const Draw& state, std::vector<Draw>& out) {
    BatchProgram program;
    program.blend = &blend;
    BatchOutput to;
    to.draws = &out;
    return BuildBatches(part, inputs, program, state, to);
}

SkinGeometry::Result SkinGeometry::Build(const Part& part, const Inputs& inputs, const Draw& state, Sink sink, void* user) {
    if (!sink) return Result::InvalidGeometry;
    BatchOutput to;
    to.sink = sink;
    to.user = user;
    return BuildBatches(part, inputs, BatchProgram{}, state, to);
}

SkinGeometry::Result SkinGeometry::BuildBlend(const Part& part, const Inputs& inputs, const BlendInputs& blend,
                                              const Draw& state, Sink sink, void* user) {
    if (!sink) return Result::InvalidGeometry;
    BatchProgram program;
    program.blend = &blend;
    BatchOutput to;
    to.sink = sink;
    to.user = user;
    return BuildBatches(part, inputs, program, state, to);
}

SkinGeometry::Result SkinGeometry::GpuTriangles(const Part& part, std::vector<GpuVertex>& out) {
    const auto F = [](uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; };
    out.clear();
    for (const auto& batch : part.batches) {
        const uint32_t count = batch.Vertices();
        if (count < 3 || count > kMaxBatchVertices || batch.vertices.size() != 4 * size_t(count)) return Result::InvalidGeometry;
        const float sx = F(batch.scale[0]), sy = F(batch.scale[1]);
        GpuVertex ring[3];
        unsigned held = 0, oldest = 0, parity = 0;
        for (uint32_t j = 0; j < count; ++j) {
            const auto& q0 = batch.vertices[4 * j];
            const auto& q1 = batch.vertices[4 * j + 1];
            const auto& q2 = batch.vertices[4 * j + 2];
            const auto& q3 = batch.vertices[4 * j + 3];
            GpuVertex v{};
            v.position[0] = float(int32_t(q0[0])) * sx;
            v.position[1] = float(int32_t(q0[1])) * sx;
            v.position[2] = float(int32_t(q0[2])) * sx;
            v.normal[0] = float(int32_t(q0[3])) * sx;
            v.normal[1] = float(int32_t(q1[2])) * sy;
            v.normal[2] = float(int32_t(q1[3])) * sy;
            v.st[0] = float(int32_t(q1[0])) * sy;
            v.st[1] = float(int32_t(q1[1])) * sy;
            const uint32_t flags = q2[3] & 0xFFFFu;
            const uint32_t bones = flags & 3u;
            const int used = bones == 1 ? 1 : bones == 2 ? 2 : 3;
            for (int b = 0; b < 3; ++b) {
                v.bone[b] = b < used ? float(q2[size_t(b)] & 0x1FFu) : -1.f;
                v.weight[b] = b < used ? F(q2[size_t(b)]) : 0.f;
            }
            for (int k = 0; k < 4; ++k) v.colour[k] = int32_t(q3[size_t(k)]);
            v.index = j;
            const bool draw = (flags & 0x8000u) == 0;
            if (held < 3) {
                ring[held++] = v;
            } else {
                ring[oldest] = v;
                oldest = oldest == 2 ? 0 : oldest + 1;
            }
            if (!draw) { parity = 0; continue; }
            if (held != 3) continue;
            const GpuVertex& a = ring[oldest];
            const GpuVertex& b = ring[oldest == 2 ? 0 : oldest + 1];
            const GpuVertex& c = ring[oldest == 0 ? 2 : oldest - 1];
            out.push_back(parity % 2 == 0 ? a : b);
            out.push_back(parity % 2 == 0 ? b : a);
            out.push_back(c);
            ++parity;
            if (out.size() > kMaxVertices * 24) return Result::LimitExceeded;
        }
    }
    return out.empty() ? Result::InvalidGeometry : Result::Ok;
}

bool SkinGeometry::BlendPositions(const Part& part, const BlendInputs& blend, float (*out)[4], uint32_t* count) {
    if (blend.shapes.empty() || part.batches.size() != 1) return false;
    const Batch& batch = part.batches[0];
    const uint32_t vertices = batch.Vertices();
    if (vertices < 3 || vertices > kMaxBatchVertices || batch.vertices.size() != 4 * size_t(vertices)) return false;
    for (const Shape& shape : blend.shapes)
        if (!shape.rows || shape.count < vertices) return false;

    const auto run = [&](auto C) {
        const float sx = C(vu1lib::bits2f(batch.scale[0]));
        for (uint32_t j = 0; j < vertices; ++j) {
            float p[3];
            for (int k = 0; k < 3; ++k) p[k] = C(C(float(int32_t(batch.vertices[4 * j][size_t(k)]))) * sx);
            for (const Shape& shape : blend.shapes) {
                const float r8[4] = {C(blend.factors[0]), C(blend.factors[1]), C(blend.factors[2]), C(shape.weight)};
                for (int k = 0; k < 3; ++k) {
                    float f9 = float(int32_t(int8_t(shape.rows[size_t(j) * 4 + size_t(k)])));
                    f9 = C(C(f9) * r8[k]);
                    f9 = C(C(f9) * r8[3]);
                    p[k] = C(C(f9) + C(p[k]));
                }
            }
            for (int k = 0; k < 3; ++k) out[j][k] = p[k];
            out[j][3] = 1.f;
        }
    };
    bool plain = vuexact::Plain(vu1lib::bits2f(batch.scale[0])) && vuexact::Plain(blend.factors[0]) &&
                 vuexact::Plain(blend.factors[1]) && vuexact::Plain(blend.factors[2]);
    for (const Shape& shape : blend.shapes) plain = plain && vuexact::Plain(shape.weight);
    bool done = false;
    if (plain) {
        vuexact::Scope scope;
        run([](float v) { return v; });
        done = scope.Clean();
    }
    if (!done) run([](float v) { return vu1lib::CF(v); });
    *count = vertices;
    return true;
}

}
