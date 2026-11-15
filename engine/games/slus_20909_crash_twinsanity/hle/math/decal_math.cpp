#include "games/slus_20909_crash_twinsanity/hle/math/decal_math.h"

#include <cmath>
#include <cstdint>
#include <cstring>

#include "games/slus_20909_crash_twinsanity/hle/graphics/rounding_guard.h"
#include "engine/vu/vu_float.h"

namespace title {
namespace {

using Vector = DecalMath::Vector;

constexpr uint32_t kSign = 0x80000000u;
constexpr uint32_t kMaxFloat = 0x7F7FFFFFu;
constexpr uint8_t X = 8, Y = 4, Z = 2, W = 1, XZ = X | Z, XYZ = X | Y | Z, XYZW = 15;

uint32_t Bits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }
float Float(uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; }

float Clamp(float v) {
    const uint32_t b = Bits(v), e = b & 0x7F800000u;
    if (e == 0x7F800000u) return Float((b & kSign) | kMaxFloat);
    if (e == 0) return Float(b & kSign);
    return v;
}
Vector Clamp4(const Vector& v) { return {Clamp(v[0]), Clamp(v[1]), Clamp(v[2]), Clamp(v[3])}; }

uint32_t FtoiSat(float f) {
    const uint32_t b = Bits(f);
    if ((b & 0x7F800000u) >= 0x4F000000u) return (b & kSign) ? 0x80000000u : 0x7FFFFFFFu;
    return uint32_t(int32_t(f));
}

struct Vu {
    const Vector* data;
    Vector vf[32]{};
    Vector acc{};
    float i = 0.f, q = 0.f;
    uint16_t vi1 = 0, vi2 = 0;
};

Vector Src(Vu& u, int reg) { u.acc = Clamp4(u.acc); return Clamp4(u.vf[reg]); }
float Bc(const Vu& u, int reg, int lane) { return Clamp(u.vf[reg][lane]); }

uint16_t Put(Vu& u, int fd, const Vector& raw, uint8_t mask) {
    uint16_t s = 0;
    for (int k = 0; k < 4; ++k) {
        if (!(mask & (8 >> k))) continue;
        if (Bits(raw[k]) & kSign) s |= uint16_t(0x80 >> k);
        if (fd != 0) u.vf[fd][k] = Clamp(raw[k]);
    }
    return s;
}
void PutAcc(Vu& u, const Vector& raw, uint8_t mask) {
    for (int k = 0; k < 4; ++k)
        if (mask & (8 >> k)) u.acc[k] = Clamp(raw[k]);
}

void AddS(Vu& u, int fd, int fs, float s, uint8_t mask) {
    const Vector a = Src(u, fs); Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = a[k] + s;
    Put(u, fd, r, mask);
}
void SubS(Vu& u, int fd, int fs, float s, uint8_t mask) {
    const Vector a = Src(u, fs); Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = a[k] - s;
    Put(u, fd, r, mask);
}
void MulS(Vu& u, int fd, int fs, float s, uint8_t mask) {
    const Vector a = Src(u, fs); Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = a[k] * s;
    Put(u, fd, r, mask);
}
void SubV(Vu& u, int fd, int fs, int ft, uint8_t mask) {
    const Vector a = Src(u, fs), b = Clamp4(u.vf[ft]); Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = a[k] - b[k];
    Put(u, fd, r, mask);
}
void MulV(Vu& u, int fd, int fs, int ft, uint8_t mask) {
    const Vector a = Src(u, fs), b = Clamp4(u.vf[ft]); Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = a[k] * b[k];
    Put(u, fd, r, mask);
}
void MaddS(Vu& u, int fd, int fs, float s, uint8_t mask) {
    const Vector a = Src(u, fs); Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = VuFloat::Fma(a[k], s, u.acc[k]);
    Put(u, fd, r, mask);
}
uint16_t MsubS(Vu& u, int fd, int fs, float s, uint8_t mask) {
    const Vector a = Src(u, fs); Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = VuFloat::Fma(-a[k], s, u.acc[k]);
    return Put(u, fd, r, mask);
}
void AccAddS(Vu& u, int fs, float s, uint8_t mask) {
    const Vector a = Src(u, fs); Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = a[k] + s;
    PutAcc(u, r, mask);
}
void AccMulS(Vu& u, int fs, float s, uint8_t mask) {
    const Vector a = Src(u, fs); Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = a[k] * s;
    PutAcc(u, r, mask);
}
void AccMaddS(Vu& u, int fs, float s, uint8_t mask) {
    const Vector a = Src(u, fs); Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = VuFloat::Fma(a[k], s, u.acc[k]);
    PutAcc(u, r, mask);
}
void AccMsubS(Vu& u, int fs, float s, uint8_t mask) {
    const Vector a = Src(u, fs); Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = VuFloat::Fma(-a[k], s, u.acc[k]);
    PutAcc(u, r, mask);
}

void Itof(Vu& u, int ft, int fs, float scale, uint8_t mask) {
    u.acc = Clamp4(u.acc);
    const Vector a = u.vf[fs];
    for (int k = 0; k < 4; ++k)
        if (mask & (8 >> k)) u.vf[ft][k] = float(int32_t(Bits(a[k]))) / scale;
}
void Ftoi(Vu& u, int ft, int fs, float scale, uint8_t mask) {
    u.acc = Clamp4(u.acc);
    const Vector a = u.vf[fs];
    for (int k = 0; k < 4; ++k)
        if (mask & (8 >> k)) u.vf[ft][k] = Float(FtoiSat(a[k] * scale));
}

uint32_t MaxBits(uint32_t a, uint32_t b) {
    const int32_t ka = (a & kSign) ? -int32_t(a & 0x7FFFFFFFu) - 1 : int32_t(a);
    const int32_t kb = (b & kSign) ? -int32_t(b & 0x7FFFFFFFu) - 1 : int32_t(b);
    return ka >= kb ? a : b;
}
void MaxV(Vu& u, int fd, int fs, int ft, uint8_t mask) {
    u.acc = Clamp4(u.acc);
    const Vector a = u.vf[fs], b = u.vf[ft];
    for (int k = 0; k < 4; ++k)
        if (mask & (8 >> k)) u.vf[fd][k] = Float(MaxBits(Bits(a[k]), Bits(b[k])));
}
void MaxBc(Vu& u, int fd, int fs, int ft, int bc, uint8_t mask) {
    u.acc = Clamp4(u.acc);
    const Vector a = u.vf[fs];
    const uint32_t b = Bits(u.vf[ft][bc]);
    for (int k = 0; k < 4; ++k)
        if (mask & (8 >> k)) u.vf[fd][k] = Float(MaxBits(Bits(a[k]), b));
}

void Lq(Vu& u, int ft, uint16_t vi, int imm, uint8_t mask) {
    const Vector& m = u.data[(uint32_t(vi) + uint32_t(imm)) & (DecalMath::kDataQuadwords - 1)];
    for (int k = 0; k < 4; ++k)
        if (mask & (8 >> k)) u.vf[ft][k] = m[k];
}
uint16_t Mtir(const Vu& u, int fs, int lane) { return uint16_t(Bits(u.vf[fs][lane])); }
void Sqrt(Vu& u, float x) { u.q = Clamp(std::sqrt(std::fabs(Clamp(x)))); }

DecalMath::Look Out(const Vu& u) {
    DecalMath::Look look;
    for (int k = 0; k < 4; ++k) {
        look.place[k] = Bits(u.vf[28][k]);
        look.colour[k] = Bits(u.vf[29][k]);
        look.offsets[k] = Bits(u.vf[31][k]);
        look.more[k] = Bits(u.vf[30][k]);
    }
    return look;
}

DecalMath::Look SameChunk(Vu& u) {
    Lq(u, 28, u.vi1, 5, W);
    MulS(u, 1, 1, Bc(u, 28, 3), W); u.i = 0.5f;
    AddS(u, 29, 0, Clamp(u.i), XZ);
    SubS(u, 1, 0, Bc(u, 1, 3), W); u.i = 0.5f;
    AddS(u, 30, 0, Clamp(u.i), XZ);
    SubS(u, 31, 0, Clamp(u.i), X);
    AddS(u, 31, 0, Clamp(u.i), Z); u.i = 3.0f;
    MulS(u, 1, 1, Clamp(u.i), W);
    Ftoi(u, 29, 1, 1.f, W);
    u.vi2 = Mtir(u, 29, 3); Itof(u, 29, 29, 1.f, W);
    u.vi1 = uint16_t(u.vi2 + u.vi1);
    Lq(u, 4, u.vi1, 4, XZ);
    SubV(u, 1, 1, 29, W); u.vi2 = 240;
    Itof(u, 3, 3, 32768.f, XYZW); Lq(u, 5, u.vi1, 5, XZ);
    Itof(u, 2, 2, 32768.f, XYZW); Lq(u, 6, u.vi1, 0, XYZW);
    AccAddS(u, 4, Bc(u, 0, 0), XZ);
    AccMsubS(u, 4, Bc(u, 1, 3), XZ); Lq(u, 4, u.vi1, 1, XYZW);
    MaddS(u, 5, 5, Bc(u, 1, 3), XZ);
    AccAddS(u, 6, Bc(u, 0, 0), XYZW);
    AccMsubS(u, 6, Bc(u, 1, 3), XYZW);
    MaddS(u, 7, 4, Bc(u, 1, 3), XYZW);
    MulV(u, 6, 5, 29, XZ);
    MulS(u, 4, 2, Bc(u, 5, 0), XYZW);
    MulS(u, 3, 3, Bc(u, 5, 2), XYZW);
    Ftoi(u, 29, 7, 1.f, XYZW);
    MulV(u, 7, 6, 6, XZ);
    AccMulS(u, 4, Bc(u, 31, 0), XYZ);
    MaddS(u, 2, 3, Bc(u, 31, 2), XYZ); Lq(u, 31, 0, 5, XYZW);
    AccMulS(u, 4, Bc(u, 30, 0), XYZW); Lq(u, 5, 0, 7, XYZW);
    AddS(u, 4, 7, Bc(u, 7, 2), X); Lq(u, 6, 0, 4, XYZW);
    MaddS(u, 3, 3, Bc(u, 30, 2), XYZW); Lq(u, 30, 0, 6, XYZW);
    AccMulS(u, 5, Bc(u, 0, 3), XYZW);
    AccMaddS(u, 6, Bc(u, 1, 0), XYZW); Sqrt(u, u.vf[4][0]);
    AccMaddS(u, 31, Bc(u, 1, 1), XYZW); Lq(u, 6, 0, 0, XYZW);
    AccMaddS(u, 30, Bc(u, 1, 2), XYZW);
    Ftoi(u, 30, 3, 4096.f, XYZW); u.vf[3][3] = u.vf[0][0];
    Ftoi(u, 31, 2, 4096.f, XYZ);
    MaxBc(u, 2, 0, 0, 3, XYZW);
    AddS(u, 1, 3, u.q, W); Lq(u, 3, 0, 3, XYZW);
    const uint16_t nearFlags = MsubS(u, 0, 2, Bc(u, 1, 3), XYZW); Lq(u, 4, 0, 1, XYZW);
    AccMulS(u, 3, Bc(u, 0, 3), XYZW); Lq(u, 3, 0, 2, XYZW);
    AccMaddS(u, 6, Bc(u, 1, 0), XYZW); u.vi1 = 240;
    AccMaddS(u, 4, Bc(u, 1, 1), XYZW); u.vi1 = uint16_t(nearFlags & u.vi1);
    AccMaddS(u, 3, Bc(u, 1, 2), XYZW);
    const uint16_t sideFlags = MsubS(u, 8, 2, Bc(u, 1, 3), XYZW);
    u.vi2 = uint16_t(sideFlags & u.vi2);
    MaxV(u, 28, 1, 1, XYZ); u.vi1 = uint16_t(u.vi2 | u.vi1);
    MaxV(u, 28, 0, 0, W); u.vf[31][3] = Float(uint32_t(int32_t(int16_t(u.vi1))));
    return Out(u);
}

DecalMath::Look OtherChunk(Vu& u) {
    Itof(u, 2, 2, 32768.f, XYZW); Lq(u, 31, 0, 8, XYZ);
    Itof(u, 3, 3, 32768.f, XYZW); Lq(u, 29, 0, 9, XYZ);
    Lq(u, 29, u.vi1, 5, W);
    AccMulS(u, 31, Bc(u, 1, 0), XYZ); Lq(u, 30, 0, 10, XYZ);
    AccMaddS(u, 29, Bc(u, 1, 1), XYZ); Lq(u, 29, 0, 11, XYZ);
    MulS(u, 1, 1, Bc(u, 29, 3), W);
    AccMaddS(u, 30, Bc(u, 1, 2), XYZ);
    MaddS(u, 1, 29, Bc(u, 0, 3), XYZ);
    SubS(u, 1, 0, Bc(u, 1, 3), W); u.i = 0.5f;
    AddS(u, 29, 0, Clamp(u.i), XZ); u.i = 0.5f;
    AddS(u, 30, 0, Clamp(u.i), XZ);
    SubS(u, 31, 0, Clamp(u.i), X);
    AddS(u, 31, 0, Clamp(u.i), Z); u.i = 3.0f;
    MulS(u, 1, 1, Clamp(u.i), W);
    Ftoi(u, 29, 1, 1.f, W);
    u.vi2 = Mtir(u, 29, 3); Itof(u, 29, 29, 1.f, W);
    u.vi1 = uint16_t(u.vi2 + u.vi1);
    Lq(u, 28, u.vi1, 4, XZ);
    SubV(u, 1, 1, 29, W); Lq(u, 4, u.vi1, 5, XZ);
    AccAddS(u, 28, Bc(u, 0, 0), XZ); Lq(u, 5, u.vi1, 0, XYZW);
    AccMsubS(u, 28, Bc(u, 1, 3), XZ); u.vi2 = 240;
    MaddS(u, 4, 4, Bc(u, 1, 3), XZ);
    AccAddS(u, 5, Bc(u, 0, 0), XYZW); Lq(u, 28, u.vi1, 1, XYZW);
    AccMsubS(u, 5, Bc(u, 1, 3), XYZW); u.vi1 = 240;
    MulV(u, 5, 4, 29, XZ);
    MaddS(u, 6, 28, Bc(u, 1, 3), XYZW);
    MulS(u, 28, 2, Bc(u, 4, 0), XYZW);
    MulV(u, 5, 5, 5, XZ);
    MulS(u, 2, 3, Bc(u, 4, 2), XYZW);
    AccMulS(u, 28, Bc(u, 31, 0), XYZ);
    AddS(u, 4, 5, Bc(u, 5, 2), X); Lq(u, 5, 0, 0, XYZW);
    MaddS(u, 3, 2, Bc(u, 31, 2), XYZ); Lq(u, 31, 0, 3, XYZW);
    AccMulS(u, 28, Bc(u, 30, 0), XYZW); Lq(u, 28, 0, 1, XYZW);
    Sqrt(u, u.vf[4][0]);
    MaddS(u, 30, 2, Bc(u, 30, 2), XYZW); Lq(u, 2, 0, 2, XYZW);
    AccMulS(u, 31, Bc(u, 0, 3), XYZW); Lq(u, 4, 0, 7, XYZW);
    AccMaddS(u, 5, Bc(u, 1, 0), XYZW); u.vf[1][3] = u.vf[0][0];
    AccMaddS(u, 28, Bc(u, 1, 1), XYZW);
    AccMaddS(u, 2, Bc(u, 1, 2), XYZW);
    MaxBc(u, 2, 0, 0, 3, XYZW);
    AddS(u, 1, 1, u.q, W);
    Ftoi(u, 31, 3, 4096.f, XYZ); Lq(u, 3, 0, 4, XYZW);
    const Vector moved = u.vf[1];
    const uint16_t sideFlags = MsubS(u, 7, 2, Bc(u, 1, 3), XYZW); u.vf[28] = moved;
    AccMulS(u, 4, Bc(u, 0, 3), XYZW); Lq(u, 5, 0, 5, XYZW);
    AccMaddS(u, 3, Bc(u, 1, 0), XYZW); Lq(u, 3, 0, 6, XYZW);
    u.vi1 = uint16_t(sideFlags & u.vi1);
    AccMaddS(u, 5, Bc(u, 1, 1), XYZW);
    AccMaddS(u, 3, Bc(u, 1, 2), XYZW);
    const uint16_t nearFlags = MsubS(u, 8, 2, Bc(u, 1, 3), XYZW);
    u.vi2 = uint16_t(nearFlags & u.vi2);
    Ftoi(u, 29, 6, 1.f, XYZW); u.vi1 = uint16_t(u.vi1 | u.vi2);
    Ftoi(u, 30, 30, 4096.f, XYZW); u.vf[31][3] = Float(uint32_t(int32_t(int16_t(u.vi1))));
    return Out(u);
}

}

DecalMath::Look DecalMath::Age(const Vector* data, uint32_t program, const Decal& decal) {
    RoundingGuard rounding;
    Vu u;
    u.data = data;
    u.vf[0] = {0.f, 0.f, 0.f, 1.f};
    u.vf[1] = decal.place;
    for (int k = 0; k < 4; ++k) {
        u.vf[2][k] = Float(uint32_t(decal.frame[k]));
        u.vf[3][k] = Float(uint32_t(decal.more[k]));
    }
    u.vi1 = decal.variant;
    return program == kOtherChunk ? OtherChunk(u) : SameChunk(u);
}

}
