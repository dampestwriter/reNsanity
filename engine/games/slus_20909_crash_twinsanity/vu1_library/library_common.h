#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

#include "engine/vu/hle/vu1_hle_contract.h"
#include "engine/vu/vu_float.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/qw_ptr.h"

namespace vu1lib {

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

struct V4 {
    u32 b[4];
};

inline float bits2f(u32 b) { return VuFloat::Bits2F(b); }
inline u32 f2bits(float f) { return VuFloat::F2Bits(f); }
inline float CF(float val) { return VuFloat::ClampFloat(val); }
inline float VuDiv(float num, float den) { return VuFloat::VuDiv(num, den); }
inline u32 Ftoi0(u32 lane_bits) { return VuFloat::Ftoi0(lane_bits); }
inline u32 ClipNf(u32 xb, u32 yb, u32 zb, u32 wb) { return VuFloat::ClipNf(xb, yb, zb, wb); }
inline u32 Fmin_raw(u32 a, u32 b) { return VuFloat::FpMin(a, b); }
inline u32 Fmax_raw(u32 a, u32 b) { return VuFloat::FpMax(a, b); }

constexpr u32 kAdcMark = 0xFFFF8000u;

constexpr int kMeshRingBound = 4096;

V4 XformClip(const V4& r0, const V4& r1, const V4& r2, const V4& r3, float vx, float vy, float vz);

V4 GuardScale(const V4& r4, const V4& p);

bool KickAt(QwPtr m, u32 qw, Vu1HleOutput& out);

void ApplyMac(uint16_t& mac_flag, uint16_t& status, u32 mac);

void PushMac(uint16_t& mac_flag, uint16_t& status, const u32 raw[4], u8 mask);

float DivQ(uint16_t& status, u32 num_bits, u32 den_bits);

std::vector<u8>& WorkingCopy(const Vu1HleInput& in);

void EmitDataWrites(const u8* mem, const Vu1HleInput& in, Vu1HleOutput& out);

}
