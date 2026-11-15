#include <cmath>
#include <cstring>

inline float VuFloat::Bits2F(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

inline uint32_t VuFloat::F2Bits(float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}

inline uint32_t VuFloat::ClampBits1(uint32_t bits) {
    const uint32_t exp = bits & kExpMask;
    if (exp == kExpMask) return (bits & kSignBit) | kMaxFloatBits;
    if (exp == 0 && (bits & kMantMask) != 0) return bits & kSignBit;
    return bits;
}

inline float VuFloat::ClampFloat(float v) {
    return Bits2F(ClampBits1(F2Bits(v)));
}

inline uint16_t VuFloat::MacFlags1(uint32_t bits, int field) {
    uint16_t flags = 0;
    const uint32_t exp = (bits >> 23) & 0xFFu;
    if (bits & kSignBit) flags |= static_cast<uint16_t>(1u << (field + 4));
    if (exp == 0) {
        flags |= static_cast<uint16_t>(1u << field);
        if (bits & kMantMask) flags |= static_cast<uint16_t>(1u << (field + 8));
    } else if (exp == 0xFFu) {
        flags |= static_cast<uint16_t>(1u << (field + 12));
    }
    return flags;
}

inline uint32_t VuFloat::FpMax(uint32_t a, uint32_t b) {
    const int32_t sa = static_cast<int32_t>(a), sb = static_cast<int32_t>(b);
    if (sa < 0 && sb < 0) return static_cast<uint32_t>(sa < sb ? sa : sb);
    return static_cast<uint32_t>(sa > sb ? sa : sb);
}

inline uint32_t VuFloat::FpMin(uint32_t a, uint32_t b) {
    const int32_t sa = static_cast<int32_t>(a), sb = static_cast<int32_t>(b);
    if (sa < 0 && sb < 0) return static_cast<uint32_t>(sa > sb ? sa : sb);
    return static_cast<uint32_t>(sa < sb ? sa : sb);
}

inline uint32_t VuFloat::FtoiSat(float f) {
    const uint32_t ub = F2Bits(f);
    if ((ub & kExpMask) >= 0x4F000000u) return (ub & kSignBit) ? 0x80000000u : 0x7FFFFFFFu;
    return static_cast<uint32_t>(static_cast<int32_t>(f));
}

inline uint32_t VuFloat::Itof0(uint32_t lane_bits) {
    return F2Bits(static_cast<float>(static_cast<int32_t>(lane_bits)));
}

inline float VuFloat::VuDiv(float num, float den) {
    num = ClampFloat(num);
    den = ClampFloat(den);
    if (den == 0.0f) {
        const uint32_t sign = (std::signbit(num) != std::signbit(den)) ? kSignBit : 0u;
        return Bits2F(sign | kMaxFloatBits);
    }
    return ClampFloat(num / den);
}

inline uint32_t VuFloat::Ftoi0(uint32_t lane_bits) {
    return FtoiSat(ClampFloat(Bits2F(lane_bits)));
}

inline uint32_t VuFloat::Ftoi4(uint32_t lane_bits) {
    return FtoiSat(Bits2F(lane_bits) * 16.0f);
}

inline uint32_t VuFloat::Ftoi12(uint32_t lane_bits) {
    return FtoiSat(Bits2F(lane_bits) * 4096.0f);
}

inline uint32_t VuFloat::Ftoi15(uint32_t lane_bits) {
    return FtoiSat(Bits2F(lane_bits) * 32768.0f);
}

inline uint32_t VuFloat::ClipNf(uint32_t xb_u, uint32_t yb_u, uint32_t zb_u, uint32_t wb_u) {
    const int32_t wb = (int32_t)wb_u;
    const int32_t value = (wb & 0x7f800000) ? (int32_t)(wb_u & 0x7fffffff) : 0x007fffff;
    uint32_t nf = 0;
    const int32_t xb = (int32_t)xb_u, yb = (int32_t)yb_u, zb = (int32_t)zb_u;
    if ((int32_t)(xb ^ (int32_t)0x00000000) > value) nf |= 0x01;
    if ((int32_t)(xb ^ (int32_t)0x80000000) > value) nf |= 0x02;
    if ((int32_t)(yb ^ (int32_t)0x00000000) > value) nf |= 0x04;
    if ((int32_t)(yb ^ (int32_t)0x80000000) > value) nf |= 0x08;
    if ((int32_t)(zb ^ (int32_t)0x00000000) > value) nf |= 0x10;
    if ((int32_t)(zb ^ (int32_t)0x80000000) > value) nf |= 0x20;
    return nf;
}


inline float VuFloat::Fma(float a, float b, float c) { return __builtin_fmaf(a, b, c); }
