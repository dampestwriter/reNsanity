#include "games/slus_20909_crash_twinsanity/hle/math/particle_view_math.h"

#include <cmath>
#include <cstdint>
#include <cstring>

#include "engine/vu/vu_float.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/rounding_guard.h"

namespace title {
namespace {

using Vector = ParticleViewMath::Vector;
using Matrix = ParticleViewMath::Matrix;

constexpr uint32_t kSign = 0x80000000u;
constexpr uint32_t kMaxFloat = 0x7F7FFFFFu;

uint32_t Bits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }
float Float(uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; }

float Clamp(float v) {
    const uint32_t b = Bits(v), e = b & 0x7F800000u;
    if (e == 0x7F800000u) return Float((b & kSign) | kMaxFloat);
    if (e == 0) return Float(b & kSign);
    return v;
}
Vector Clamp4(const Vector& v) { return {Clamp(v[0]), Clamp(v[1]), Clamp(v[2]), Clamp(v[3])}; }

uint16_t SignFlags(const Vector& raw) {
    uint16_t s = 0;
    for (int k = 0; k < 4; ++k) if (Bits(raw[k]) & kSign) s |= uint16_t(0x80 >> k);
    return s;
}

Vector MaddBc(const Vector& acc, const Vector& a, float s) {
    const Vector ca = Clamp4(a), cacc = Clamp4(acc);
    const float cs = Clamp(s);
    Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = Clamp(VuFloat::Fma(ca[k], cs, cacc[k]));
    return r;
}
Vector MulBc(const Vector& a, float s) {
    const Vector ca = Clamp4(a);
    const float cs = Clamp(s);
    Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = Clamp(ca[k] * cs);
    return r;
}

Vector RowTimes(const Vector& m0, const Vector& m1, const Vector& m2, const Vector& m3, const Vector& v) {
    Vector acc = MulBc(m0, v[0]);
    acc = MaddBc(acc, m1, v[1]);
    acc = MaddBc(acc, m2, v[2]);
    return MaddBc(acc, m3, v[3]);
}

uint32_t GroupFlags(const Vector* qw, const Vector& extents, const Vector& place) {
    Vector radius = MulBc(qw[0], extents[0]);
    radius = MaddBc(radius, qw[1], extents[1]);
    radius = MaddBc(radius, qw[2], extents[2]);
    Vector centre = MulBc(qw[3], place[0]);
    centre = MaddBc(centre, qw[4], place[1]);
    centre = MaddBc(centre, qw[5], place[2]);
    centre = MaddBc(centre, qw[6], 1.f);
    Vector plus{}, minus{};
    for (int k = 0; k < 4; ++k) {
        plus[k] = VuFloat::Fma(radius[k], 1.f, centre[k]);
        minus[k] = VuFloat::Fma(-radius[k], 1.f, centre[k]);
    }
    return uint32_t(SignFlags(plus) & 0xF0) & uint32_t(SignFlags(minus) & 0xF0);
}
float Sqrt(float x) { return Clamp(std::sqrt(std::fabs(Clamp(x)))); }
float Div(float num, float den) {
    num = Clamp(num); den = Clamp(den);
    if (den == 0.f) return num == 0.f ? Float((Bits(num) ^ Bits(den)) & kSign) : Float(((Bits(num) ^ Bits(den)) & kSign) | kMaxFloat);
    return Clamp(num / den);
}

}

ParticleViewMath::Result ParticleViewMath::BlockView(const View& view, const Vector& extents, const Matrix& gravity, const Vector& place,
                                                     float scale, bool keepsTranslation) {
    RoundingGuard rounding;
    const Vector* q = view.qw.data();
    const Vector camera = Clamp4(q[14]);
    const Vector p = Clamp4(place);
    Result out{};

    Vector d{};
    for (int k = 0; k < 4; ++k) d[k] = Clamp(keepsTranslation ? camera[k] - p[k] : p[k] - camera[k]);
    Vector sq{};
    for (int k = 0; k < 4; ++k) sq[k] = Clamp(d[k] * d[k]);
    const float len2 = Clamp(Clamp(sq[0] + sq[1]) + sq[2]);
    const float len = Sqrt(len2);
    out.distance = Clamp(0.f + len);
    const uint32_t flags1 = GroupFlags(q + 0, extents, p);
    const uint32_t flags2 = GroupFlags(q + 7, extents, p);
    out.flags = flags1 | flags2;
    const float inverse = Div(1.f, out.distance);

    Vector pull{};
    for (int k = 0; k < 3; ++k) pull[k] = Clamp(Clamp(d[k] * inverse) * Clamp(scale));
    pull[3] = Clamp(d[3] * Clamp(scale));
    Vector at{};
    if (keepsTranslation) {
        at = {pull[0], pull[1], pull[2], p[3]};
    } else {
        for (int k = 0; k < 4; ++k) at[k] = Clamp(p[k] - pull[k]);
    }
    for (int r = 0; r < 3; ++r)
        out.matrix[r] = keepsTranslation ? q[15 + r] : RowTimes(q[15], q[16], q[17], q[18], gravity[r]);
    out.matrix[3] = RowTimes(q[15], q[16], q[17], q[18], at);
    return out;
}

}
