#include "engine/render/blend_ff_map.h"

namespace ps2r {
namespace render {

FfBlendMap BlendFfMap::Map(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    FfBlendMap m;
    m.matched = true;
    m.uses_fix = false;
    m.eq_rgb = kGlFuncAdd;
    m.src_rgb = kGlOne;
    m.dst_rgb = kGlZero;
    auto is = [&](uint8_t A, uint8_t B, uint8_t C, uint8_t D) {
        return a == A && b == B && c == C && d == D;
    };
    if (is(0, 1, 0, 1)) {
        m.src_rgb = kGlSrcAlpha;
        m.dst_rgb = kGlOneMinusSrcAlpha;
    } else if (is(0, 2, 0, 1)) {
        m.src_rgb = kGlSrcAlpha;
        m.dst_rgb = kGlOne;
    } else if (is(0, 2, 0, 2)) {
        m.src_rgb = kGlSrcAlpha;
        m.dst_rgb = kGlZero;
    } else if (is(0, 1, 2, 1)) {
        m.src_rgb = kGlConstantAlpha;
        m.dst_rgb = kGlOneMinusConstantAlpha;
        m.uses_fix = true;
    } else if (is(0, 2, 2, 1)) {
        m.src_rgb = kGlConstantAlpha;
        m.dst_rgb = kGlOne;
        m.uses_fix = true;
    } else if (is(1, 0, 0, 0)) {
        m.eq_rgb = kGlFuncAdd;
        m.src_rgb = kGlOneMinusSrcAlpha;
        m.dst_rgb = kGlSrcAlpha;
    } else if (is(0, 2, 2, 2)) {
        m.src_rgb = kGlConstantAlpha;
        m.dst_rgb = kGlZero;
        m.uses_fix = true;
    } else if (is(0, 1, 0, 0)) {
        m.eq_rgb = kGlFuncSubtract;
        m.src_rgb = kGlSrcAlpha;
        m.dst_rgb = kGlSrcAlpha;
    } else if (is(2, 1, 0, 1)) {
        m.src_rgb = kGlZero;
        m.dst_rgb = kGlOneMinusSrcAlpha;
    } else if (is(1, 2, 0, 0)) {
        m.src_rgb = kGlZero;
        m.dst_rgb = kGlSrcAlpha;
    } else if (is(0, 2, 0, 0)) {
        m.src_rgb = kGlSrcAlpha;
        m.dst_rgb = kGlZero;
    } else if (is(2, 0, 0, 1)) {
        m.eq_rgb = kGlFuncReverseSubtract;
        m.src_rgb = kGlSrcAlpha;
        m.dst_rgb = kGlOne;
    } else if (is(2, 0, 2, 1)) {
        m.eq_rgb = kGlFuncReverseSubtract;
        m.src_rgb = kGlConstantAlpha;
        m.dst_rgb = kGlOne;
        m.uses_fix = true;
    } else if (is(0, 1, 2, 2)) {
        m.eq_rgb = kGlFuncSubtract;
        m.src_rgb = kGlConstantAlpha;
        m.dst_rgb = kGlConstantAlpha;
        m.uses_fix = true;
    } else {
        m.matched = false;
        m.src_rgb = kGlSrcAlpha;
        m.dst_rgb = kGlOneMinusSrcAlpha;
    }
    return m;
}

bool BlendFfMap::RouteWrapBlend(bool alpha_blend, bool colclamp) {
    return alpha_blend && !colclamp;
}

}
}
