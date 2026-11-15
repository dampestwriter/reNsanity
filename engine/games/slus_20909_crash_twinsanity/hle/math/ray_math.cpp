#include "games/slus_20909_crash_twinsanity/hle/math/ray_math.h"

#include <cmath>
#include <cstdint>
#include <cstring>

#include <arm_neon.h>

#include "engine/vu/vu_float.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/rounding_guard.h"

namespace title {
namespace {

using Vector = RayMath::Vector;

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

struct Vu {
    Vector vf[32]{};
    Vector acc{};
    float q = 0.f;
    float r = 0.f;
    float i = 0.f;
    uint32_t vi1 = 0, vi2 = 0;
};
constexpr uint8_t X = 8, Y = 4, Z = 2, W = 1;

#define RAY_INLINE inline __attribute__((always_inline))
using Lanes = float __attribute__((vector_size(16)));
using LaneBits = uint32_t __attribute__((vector_size(16)));
#define RAY_NEON 1

RAY_INLINE LaneBits LoadBits(const Vector& v) { LaneBits b; std::memcpy(&b, v.data(), sizeof(b)); return b; }
RAY_INLINE void StoreBits(Vector& v, LaneBits b) { std::memcpy(v.data(), &b, sizeof(b)); }

RAY_INLINE LaneBits ClampBits(LaneBits b) {
    const LaneBits e = b & 0x7F800000u, s = b & kSign;
    const LaneBits full = (LaneBits)(e == 0x7F800000u), zero = (LaneBits)(e == 0u);
    return (zero & s) | (full & (s | kMaxFloat)) | (~(zero | full) & b);
}

RAY_INLINE Lanes Fused(Lanes a, Lanes b, Lanes c) {
#if RAY_NEON
    return (Lanes)vfmaq_f32((float32x4_t)c, (float32x4_t)a, (float32x4_t)b);
#else
    return Lanes{VuFloat::Fma(a[0], b[0], c[0]), VuFloat::Fma(a[1], b[1], c[1]), VuFloat::Fma(a[2], b[2], c[2]),
                 VuFloat::Fma(a[3], b[3], c[3])};
#endif
}
RAY_INLINE Lanes Fused(Lanes a, float b, Lanes c) {
#if RAY_NEON
    return (Lanes)vfmaq_n_f32((float32x4_t)c, (float32x4_t)a, b);
#else
    return Fused(a, Lanes{b, b, b, b}, c);
#endif
}

RAY_INLINE LaneBits On(uint8_t mask) {
    const LaneBits m = LaneBits{mask, mask, mask, mask} & LaneBits{8, 4, 2, 1};
    return (LaneBits)(m != 0u);
}

RAY_INLINE void Write(Vu& u, int fd, Lanes raw, uint8_t mask, uint16_t* flags) {
    const LaneBits bits = (LaneBits)raw, on = On(mask);
    if (flags) {
        const LaneBits s = (LaneBits)((bits & kSign) != 0u) & on & LaneBits{0x80, 0x40, 0x20, 0x10};
        *flags = uint16_t(s[0] | s[1] | s[2] | s[3]);
    }
    if (fd != 0) StoreBits(u.vf[fd], (on & ClampBits(bits)) | (~on & LoadBits(u.vf[fd])));
}
RAY_INLINE void WriteAcc(Vu& u, Lanes raw, uint8_t mask, uint16_t* flags) {
    const LaneBits bits = (LaneBits)raw, on = On(mask);
    if (flags) {
        const LaneBits s = (LaneBits)((bits & kSign) != 0u) & on & LaneBits{0x80, 0x40, 0x20, 0x10};
        *flags = uint16_t(s[0] | s[1] | s[2] | s[3]);
    }
    StoreBits(u.acc, (on & ClampBits(bits)) | (~on & LoadBits(u.acc)));
}
RAY_INLINE Lanes S(const Vu& u, int reg) { return (Lanes)ClampBits(LoadBits(u.vf[reg])); }
RAY_INLINE float Sbc(const Vu& u, int reg, int bc) { return Clamp(u.vf[reg][bc]); }
RAY_INLINE Lanes Acc(const Vu& u) { return (Lanes)LoadBits(u.acc); }
RAY_INLINE void ClampAcc(Vu& u) { StoreBits(u.acc, ClampBits(LoadBits(u.acc))); }

RAY_INLINE Lanes Yzx(Lanes v) { return Lanes{v[1], v[2], v[0], v[3]}; }
RAY_INLINE Lanes Zxy(Lanes v) { return Lanes{v[2], v[0], v[1], v[3]}; }

RAY_INLINE void Add(Vu& u, int fd, int fs, int ft, uint8_t mask, uint16_t* f = nullptr) {
    ClampAcc(u); const Lanes a = S(u, fs), b = S(u, ft);
    Write(u, fd, a + b, mask, f);
}
RAY_INLINE void Sub(Vu& u, int fd, int fs, int ft, uint8_t mask, uint16_t* f = nullptr) {
    ClampAcc(u); const Lanes a = S(u, fs), b = S(u, ft);
    Write(u, fd, a - b, mask, f);
}
RAY_INLINE void Mul(Vu& u, int fd, int fs, int ft, uint8_t mask, uint16_t* f = nullptr) {
    ClampAcc(u); const Lanes a = S(u, fs), b = S(u, ft);
    Write(u, fd, a * b, mask, f);
}
RAY_INLINE void AddBc(Vu& u, int fd, int fs, int ft, int bc, uint8_t mask, uint16_t* f = nullptr) {
    ClampAcc(u); const Lanes a = S(u, fs); const float s = Sbc(u, ft, bc);
    Write(u, fd, a + s, mask, f);
}
RAY_INLINE void SubBc(Vu& u, int fd, int fs, int ft, int bc, uint8_t mask, uint16_t* f = nullptr) {
    ClampAcc(u); const Lanes a = S(u, fs); const float s = Sbc(u, ft, bc);
    Write(u, fd, a - s, mask, f);
}
RAY_INLINE void MulQ(Vu& u, int fd, int fs, uint8_t mask, uint16_t* f = nullptr) {
    ClampAcc(u); const Lanes a = S(u, fs);
    Write(u, fd, a * u.q, mask, f);
}
RAY_INLINE void MaddBc(Vu& u, int fd, int fs, int ft, int bc, uint8_t mask, uint16_t* f = nullptr) {
    ClampAcc(u); const Lanes a = S(u, fs); const float s = Sbc(u, ft, bc);
    Write(u, fd, Fused(a, s, Acc(u)), mask, f);
}
RAY_INLINE void MaddQ(Vu& u, int fd, int fs, uint8_t mask, uint16_t* f = nullptr) {
    ClampAcc(u); const Lanes a = S(u, fs);
    Write(u, fd, Fused(a, u.q, Acc(u)), mask, f);
}
RAY_INLINE void MaddI(Vu& u, int fd, int fs, uint8_t mask, uint16_t* f = nullptr) {
    ClampAcc(u); const Lanes a = S(u, fs); const float s = Clamp(u.i);
    Write(u, fd, Fused(a, s, Acc(u)), mask, f);
}
RAY_INLINE void Abs(Vu& u, int fd, int fs, uint8_t mask, uint16_t* f = nullptr) {
    ClampAcc(u);
    Write(u, fd, (Lanes)(LoadBits(u.vf[fs]) & ~kSign), mask, f);
}

void Max(Vu& u, int fd, int fs, int ft, uint8_t mask) {
    ClampAcc(u);
    for (int k = 0; k < 4; ++k) {
        if (!(mask & (8 >> k))) continue;
        const uint32_t a = Bits(u.vf[fs][k]), b = Bits(u.vf[ft][k]);
        const int32_t ka = (a & kSign) ? -int32_t(a & 0x7FFFFFFFu) - 1 : int32_t(a), kb = (b & kSign) ? -int32_t(b & 0x7FFFFFFFu) - 1 : int32_t(b);
        if (fd != 0) u.vf[fd][k] = Float(ka >= kb ? a : b);
    }
}
RAY_INLINE void AddABc(Vu& u, int fs, int ft, int bc, uint8_t mask, uint16_t* f = nullptr) {
    ClampAcc(u); const Lanes a = S(u, fs); const float s = Sbc(u, ft, bc);
    WriteAcc(u, a + s, mask, f);
}
RAY_INLINE void MulABc(Vu& u, int fs, int ft, int bc, uint8_t mask, uint16_t* f = nullptr) {
    ClampAcc(u); const Lanes a = S(u, fs); const float s = Sbc(u, ft, bc);
    WriteAcc(u, a * s, mask, f);
}
RAY_INLINE void MaddABc(Vu& u, int fs, int ft, int bc, uint8_t mask, uint16_t* f = nullptr) {
    ClampAcc(u); const Lanes a = S(u, fs); const float s = Sbc(u, ft, bc);
    WriteAcc(u, Fused(a, s, Acc(u)), mask, f);
}
RAY_INLINE void MsubAQ(Vu& u, int fs, uint8_t mask, uint16_t* f = nullptr) {
    ClampAcc(u); const Lanes a = S(u, fs);
    WriteAcc(u, Fused(-a, u.q, Acc(u)), mask, f);
}
RAY_INLINE void Opmula(Vu& u, int fs, int ft, uint16_t* f = nullptr) {
    ClampAcc(u); const Lanes a = S(u, fs), b = S(u, ft);
    WriteAcc(u, Yzx(a) * Zxy(b), X | Y | Z, f);
}
RAY_INLINE void Opmsub(Vu& u, int fd, int fs, int ft, uint16_t* f = nullptr) {
    ClampAcc(u); const Lanes a = S(u, fs), b = S(u, ft);
    Write(u, fd, Fused(-Yzx(a), Zxy(b), Acc(u)), X | Y | Z, f);
}

float Rsqrt(float num, float den) {
    num = Clamp(num); den = Clamp(den);
    const uint32_t sign = (Bits(num) ^ Bits(den)) & kSign;
    if (den == 0.f) return num != 0.f ? Float(sign | kMaxFloat) : Float(sign);
    return Clamp(num / std::sqrt(std::fabs(den)));
}
float Div(float num, float den) {
    num = Clamp(num); den = Clamp(den);
    if (den == 0.f) return num == 0.f ? Float((Bits(num) ^ Bits(den)) & kSign) : Float(((Bits(num) ^ Bits(den)) & kSign) | kMaxFloat);
    return Clamp(num / den);
}

void Rinit(Vu& u) { u.r = Float(0x3F800000u | (Bits(u.vf[0][3]) & 0x007FFFFFu)); }

}

RayMath::Result RayMath::Test(const Ray& ray, const Triangle& t, const Vector& nearest) {
    RoundingGuard rounding;
    Vu u;
    u.vf[0] = {0.f, 0.f, 0.f, 1.f};
    u.vf[1] = t[0]; u.vf[2] = t[1]; u.vf[3] = t[2];
    u.vf[4] = ray.start; u.vf[5] = ray.end; u.vf[6] = nearest;
        constexpr int x = 0, y = 1, z = 2, w = 3;
    uint16_t fDs = 0, fDe = 0, fNearer = 0, fEdges = 0;
    float q1, q2, q3, q4;

    Sub(u, 6, 2, 1, X | Y | Z); u.vi1 = 0x80;

    Sub(u, 7, 3, 1, X | Y | Z);

    Opmula(u, 6, 7); Opmsub(u, 8, 7, 6);

    Mul(u, 7, 8, 8, X | Y | Z);

    Sub(u, 2, 3, 2, X | Y | Z);

    AddBc(u, 7, 7, 7, y, X);

    Opmula(u, 8, 2);

    AddBc(u, 11, 7, 7, z, X);

    Opmsub(u, 2, 2, 8);

    q1 = Rsqrt(u.vf[0][w], u.vf[11][x]);
    Sub(u, 31, 1, 3, X | Y | Z);

    Mul(u, 10, 2, 2, X | Y | Z); AddBc(u, 10, 10, 10, y, X);

    Opmula(u, 8, 6); AddBc(u, 10, 10, 10, z, X); Opmsub(u, 6, 6, 8);

    Opmula(u, 8, 31); Opmsub(u, 31, 31, 8);

    q2 = Rsqrt(u.vf[0][w], u.vf[10][x]);
    u.q = q1; MulQ(u, 8, 8, X | Y | Z);

    Mul(u, 9, 6, 6, X | Y | Z); Mul(u, 7, 31, 31, X | Y | Z); Mul(u, 11, 8, 1, X | Y | Z);

    AddBc(u, 9, 9, 9, y, X); Mul(u, 10, 4, 8, X | Y | Z); Mul(u, 8, 5, 8, X | Y | Z);

    AddBc(u, 12, 11, 11, y, X); AddBc(u, 11, 9, 9, z, X); AddBc(u, 10, 10, 10, y, X);

    { const Vector pre0 = u.vf[0]; AddBc(u, 9, 12, 11, z, X); u.vf[31][w] = pre0[x]; }

    q3 = Rsqrt(u.vf[0][w], u.vf[11][x]);
    AddBc(u, 8, 8, 8, y, X);

    u.q = q2; MulQ(u, 2, 2, X | Y | Z);

    AddBc(u, 10, 10, 10, z, X);

    SubBc(u, 31, 31, 9, x, W);

    AddBc(u, 11, 8, 8, z, X);

    AddBc(u, 9, 7, 7, y, X);

    Mul(u, 8, 2, 3, X | Y | Z);

    AddBc(u, 3, 10, 31, w, X, &fDs);

    AddBc(u, 7, 11, 31, w, X, &fDe); Rinit(u);

    { const float pre7z = u.vf[7][z]; (void)pre7z; AddBc(u, 9, 9, 7, z, X); u.vf[10][x] = u.r; }

    { const Vector pre0 = u.vf[0]; AddABc(u, 8, 8, y, X); u.vf[31][w] = pre0[x]; }

    Abs(u, 3, 3, X); u.vi2 = u.vi1 & fDs;

    Max(u, 5, 0, 0, W); u.vi1 = u.vi1 & fDe;

    q4 = Rsqrt(u.vf[0][w], u.vf[9][x]);
    MaddBc(u, 8, 10, 8, z, X);

    Abs(u, 10, 7, X); Add(u, 10, 3, 10, X);

    u.q = q3; MulQ(u, 6, 6, X | Y | Z);

    const float q5 = Div(u.vf[3][x], u.vf[10][x]);
    u.q = q4; MulQ(u, 3, 31, X | Y | Z);

    { const Vector pre0 = u.vf[0]; SubBc(u, 31, 31, 8, x, W); u.vf[4][w] = pre0[x]; }

    Mul(u, 10, 3, 1, X | Y | Z); Rinit(u);

    Mul(u, 7, 6, 1, X | Y | Z); u.vf[8][x] = u.r;

    AddABc(u, 4, 0, x, X | Y | Z | W); Rinit(u);

    u.q = q5; MsubAQ(u, 4, X | Y | Z | W); u.vf[9][x] = u.r;

    { const Vector pre31 = u.vf[31]; MaddQ(u, 31, 5, X | Y | Z | W); u.vf[1][z] = pre31[w]; }

    AddABc(u, 7, 7, y, X);

    { const Vector pre0 = u.vf[0]; MaddBc(u, 8, 8, 7, z, X); u.vf[2][w] = pre0[x]; }

    { const Vector pre0 = u.vf[0]; AddABc(u, 10, 10, y, X); u.vf[1][w] = pre0[x]; }

    MaddBc(u, 7, 9, 10, z, X); u.vi2 = (u.vi1 + u.vi2) & 0xFFFF;

    { const Vector pre6 = u.vf[6]; Sub(u, 0, 31, 6, W, &fNearer); u.vf[1][y] = pre6[z]; }

    { const Vector pre6 = u.vf[6]; SubBc(u, 2, 2, 8, x, W); u.vf[1][x] = pre6[y]; }

    u.vf[6][y] = u.vf[2][z];

    SubBc(u, 6, 1, 7, x, W); u.vi1 = 0x10;

    AddBc(u, 2, 0, 3, y, Z); u.vi1 = u.vi1 & fNearer;

    { const Vector pre3 = u.vf[3]; AddBc(u, 7, 0, 2, w, X); u.vf[6][z] = pre3[z]; }

    { const Vector pre1 = u.vf[1]; AddBc(u, 3, 0, 3, x, Z); u.vf[7][y] = pre1[z]; }

    { const Vector pre6 = u.vf[6]; Max(u, 3, 6, 6, X); u.vf[7][z] = pre6[w]; }

    AddBc(u, 3, 0, 2, x, Y);

    Max(u, 2, 1, 1, X);

    AddBc(u, 6, 0, 1, y, X); Rinit(u);

    MulABc(u, 7, 0, w, X | Y | Z); u.vf[1][x] = u.vf[1][y] = u.vf[1][z] = u.r;

    MaddABc(u, 3, 31, x, X | Y | Z); u.i = 0.1f;

    MaddABc(u, 2, 31, y, X | Y | Z); u.vi2 = (u.vi2 + u.vi1) & 0xFFFF;

    MaddABc(u, 6, 31, z, X | Y | Z); u.vi2 = (u.vi2 + u.vi2) & 0xFFFF;

    MaddI(u, 13, 1, X | Y | Z, &fEdges); u.vi2 = (u.vi2 + u.vi2) & 0xFFFF;

    u.vi2 = (u.vi2 + u.vi2) & 0xFFFF; u.vi2 = (u.vi2 + u.vi2) & 0xFFFF;

    u.vi1 = 0xE0;

    Abs(u, 0, 13, X | Y | Z); u.vi1 = u.vi1 & fEdges;

    u.vi2 = (u.vi1 | u.vi2) & 0xFFFF;

    Result out;
    out.flags = u.vi2;
    out.nearer = u.vi2 == kNearer;
    out.hit = u.vf[31];
    return out;
}

}
