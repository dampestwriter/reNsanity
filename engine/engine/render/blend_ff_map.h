#pragma once

#include <cstdint>

namespace ps2r {
namespace render {

constexpr uint32_t kGlZero = 0;
constexpr uint32_t kGlOne = 1;
constexpr uint32_t kGlSrcAlpha = 0x0302;
constexpr uint32_t kGlOneMinusSrcAlpha = 0x0303;
constexpr uint32_t kGlConstantAlpha = 0x8003;
constexpr uint32_t kGlOneMinusConstantAlpha = 0x8004;
constexpr uint32_t kGlFuncAdd = 0x8006;
constexpr uint32_t kGlFuncSubtract = 0x800A;
constexpr uint32_t kGlFuncReverseSubtract = 0x800B;

struct FfBlendMap {
    bool matched;
    bool uses_fix;
    uint32_t eq_rgb;
    uint32_t src_rgb;
    uint32_t dst_rgb;
};

class BlendFfMap {
public:
    static FfBlendMap Map(uint8_t a, uint8_t b, uint8_t c, uint8_t d);

    static bool RouteWrapBlend(bool alpha_blend, bool colclamp);
};

}
}
