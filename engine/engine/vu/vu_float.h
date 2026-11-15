#pragma once

#include <cstdint>

#include "engine/ee/cpu/cpu_state.h"


class VuFloat {
public:
    using MinMaxFn = uint32_t (*)(uint32_t, uint32_t);

    alignas(16) static const uint32_t kLaneMask[16][4];

    static constexpr uint32_t kMaxFloatBits = 0x7F7FFFFFu;
    static constexpr uint32_t kSignBit = 0x80000000u;
    static constexpr uint32_t kExpMask = 0x7F800000u;
    static constexpr uint32_t kMantMask = 0x007FFFFFu;

    static float Bits2F(uint32_t bits);
    static uint32_t F2Bits(float f);

    static uint32_t ClampBits1(uint32_t bits);
    static float ClampFloat(float v);

    static uint16_t MacFlags1(uint32_t bits, int field);
    static uint16_t PackMacBits(const uint32_t bits[4], uint8_t mask);

    static uint32_t FpMax(uint32_t a, uint32_t b);
    static uint32_t FpMin(uint32_t a, uint32_t b);

    static float VuDiv(float num, float den);

    static float Fma(float a, float b, float c);

    static uint32_t FtoiSat(float f);

    static uint32_t Ftoi0(uint32_t lane_bits);
    static uint32_t Ftoi4(uint32_t lane_bits);
    static uint32_t Ftoi12(uint32_t lane_bits);
    static uint32_t Ftoi15(uint32_t lane_bits);

    static uint32_t Itof0(uint32_t lane_bits);

    static uint32_t ClipNf(uint32_t xb, uint32_t yb, uint32_t zb, uint32_t wb);

};

#include "engine/vu/vu_float.inl"
