#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ps2e_seam.h"
#include "te_host.h"

namespace {

struct V4 {
    float x, y, z, w;
};
struct Box {
    V4 min, max;
};
struct M4 {
    V4 r[4];
};

const V4* Vecs(uint32_t a, uint32_t count) { return reinterpret_cast<const V4*>(te::GuestPointer(a, sizeof(V4) * count)); }
Box* BoxW(uint32_t a) { return reinterpret_cast<Box*>(te::GuestPointer(a, sizeof(Box))); }
const M4* Mat(uint32_t a) { return reinterpret_cast<const M4*>(te::GuestPointer(a, sizeof(M4))); }

void TakeMin(V4& m, const V4& p) { m.x = std::fmin(m.x, p.x); m.y = std::fmin(m.y, p.y); m.z = std::fmin(m.z, p.z); }
void TakeMax(V4& m, const V4& p) { m.x = std::fmax(m.x, p.x); m.y = std::fmax(m.y, p.y); m.z = std::fmax(m.z, p.z); }

V4 Scale(const V4& v, float s) { return {v.x * s, v.y * s, v.z * s, v.w * s}; }
V4 Add(const V4& a, const V4& b) { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
V4 Transform(const M4& m, const V4& p) {
    V4 acc = Scale(m.r[0], p.x);
    acc = Add(acc, Scale(m.r[1], p.y));
    acc = Add(acc, Scale(m.r[2], p.z));
    return Add(acc, Scale(m.r[3], p.w));
}

[[maybe_unused]] float CosinePolynomial(float radians) {
    constexpr float MinusInverseTurn = -0x1.45F30Ep-3f;
    constexpr float Term1 = 0x1.921FB4p+2f;
    constexpr float Term3 = -0x1.4ABBC0p+5f;
    constexpr float Term5 = 0x1.4668AEp+6f;
    constexpr float Term7 = -0x1.324CC2p+6f;
    constexpr float Term9 = 0x1.3DAF6Ep+5f;
    float angle = std::fabs(radians);
    float turns = angle * MinusInverseTurn;
    float whole = -static_cast<float>(static_cast<int32_t>(-turns));
    float fraction = whole - turns;
    float t = std::fabs(fraction - 0.5f) - 0.25f;
    float t2 = t * t;
    float t4 = t2 * t2;
    float t8 = t4 * t4;
    float sum = Term3 * t * t2;
    sum += Term7 * t * t2 * t4;
    sum += Term5 * t * t4;
    sum += t * Term1;
    return sum + Term9 * t * t8;
}

using Lanes = float __attribute__((vector_size(16)));
using Words = int32_t __attribute__((vector_size(16)));
Lanes CosinePolynomials(Lanes radians) {
    constexpr float MinusInverseTurn = -0x1.45F30Ep-3f;
    constexpr float Term1 = 0x1.921FB4p+2f;
    constexpr float Term3 = -0x1.4ABBC0p+5f;
    constexpr float Term5 = 0x1.4668AEp+6f;
    constexpr float Term7 = -0x1.324CC2p+6f;
    constexpr float Term9 = 0x1.3DAF6Ep+5f;
    const Words magnitude{0x7fffffff, 0x7fffffff, 0x7fffffff, 0x7fffffff};
    Lanes angle = (Lanes)((Words)radians & magnitude);
    Lanes turns = angle * MinusInverseTurn;
    Lanes whole = -__builtin_convertvector(__builtin_convertvector(-turns, Words), Lanes);
    Lanes fraction = whole - turns;
    Lanes t = (Lanes)((Words)(fraction - 0.5f) & magnitude) - 0.25f;
    Lanes t2 = t * t;
    Lanes t4 = t2 * t2;
    Lanes t8 = t4 * t4;
    Lanes sum = Term3 * t * t2;
    sum += Term7 * t * t2 * t4;
    sum += Term5 * t * t4;
    sum += t * Term1;
    return sum + Term9 * t * t8;
}

}

extern "C" {

void ps2e_vu0_points_box(ps2e_u32 points, ps2e_s32 count, ps2e_u32 box) {
    Box* out = BoxW(box);
    if (!out || count <= 0) return;
    const V4* p = Vecs(points, uint32_t(count));
    if (!p) return;
    V4 lo = p[0], hi = p[0];
    for (int32_t i = 1; i < count; ++i) { TakeMin(lo, p[i]); TakeMax(hi, p[i]); }
    out->min = lo;
    out->max = hi;
}

void ps2e_vu0_triangle_bounds(ps2e_u32 box, ps2e_u32 first, ps2e_u32 second, ps2e_u32 third) {
    Box* out = BoxW(box);
    const V4* a = Vecs(first, 1);
    const V4* b = Vecs(second, 1);
    const V4* c = Vecs(third, 1);
    if (!out || !a || !b || !c) return;

    V4 lo = *a, hi = *a;
    TakeMin(lo, *b); TakeMax(hi, *b);
    TakeMin(lo, *c); TakeMax(hi, *c);
    lo.w = out->min.w;
    hi.w = out->max.w;
    out->min = lo;
    out->max = hi;
}

void ps2e_vu0_get_bbox(ps2e_u32 points, ps2e_s32 count, ps2e_u32 box, ps2e_u32 matrix) {
    Box* out = BoxW(box);
    const M4* m = Mat(matrix);
    if (!out || !m || count <= 0) return;
    const V4* p = Vecs(points, uint32_t(count));
    if (!p) return;
    V4 first = Transform(*m, p[0]);
    V4 lo = first, hi = first;
    for (int32_t i = 1; i < count; ++i) {
        V4 q = Transform(*m, p[i]);
        TakeMin(lo, q);
        TakeMax(hi, q);
    }
    out->min = lo;
    out->max = hi;
}

void ps2e_math_sin_cos(ps2e_f32 first, ps2e_f32 second, ps2e_u32 out) {
    float* o = reinterpret_cast<float*>(te::GuestPointer(out, 4 * sizeof(float)));
    if (!o) return;
    constexpr float ProgramHalfPi = 0x1.921FB0p+0f;
    const Lanes cosines = CosinePolynomials(Lanes{first - ProgramHalfPi, first, second - ProgramHalfPi, second});
    std::memcpy(o, &cosines, sizeof(cosines));
}

ps2e_f32 ps2e_math_divide_by_square_root(ps2e_f32 value, ps2e_f32 square) {

    const float s = std::sqrt(std::fabs(square));
    if (s == 0.0f) return std::copysign(0x1.FFFFFEp+127f, value);
    return value / s;
}

}
