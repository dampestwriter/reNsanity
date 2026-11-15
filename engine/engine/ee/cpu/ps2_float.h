#pragma once

#include "engine/core/types.h"

class Ps2Float {
public:
    static float Clamp(float v);
    static float DivS(float a, float b);
    static float SqrtS(float v);
    static float RsqrtS(float a, float b);

    static void SetGuestThreadRounding();
};

inline float ps2_float_clamp(float v);
inline float ps2_div_s(float a, float b);
inline float ps2_sqrt_s(float v);
inline float ps2_rsqrt_s(float a, float b);

#include "ps2_float.inl"
