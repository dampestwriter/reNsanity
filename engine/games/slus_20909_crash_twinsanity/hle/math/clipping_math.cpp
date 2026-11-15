#include "games/slus_20909_crash_twinsanity/hle/math/clipping_math.h"

#include <cmath>
#include <limits>

#include "engine/ee/cpu/ps2_float.h"

namespace title {
namespace {

float Product(float a, float b) {
    volatile float result = ps2_float_clamp(a) * ps2_float_clamp(b);
    return ps2_float_clamp(result);
}

}

ClippingMath::Corners ClippingMath::BoxCorners(const Box& box) {
    Corners corners{{box.minimum, box.maximum, box.minimum, box.minimum,
                     box.minimum, box.maximum, box.maximum, box.maximum}};
    for (size_t axis = 0; axis < 3; ++axis) {
        corners[2 + axis][axis] = box.maximum[axis];
        corners[5 + axis][axis] = box.minimum[axis];
    }
    return corners;
}

uint32_t ClippingMath::Outcode(const Vector& point) {
    const float limit = std::fabs(point[3]);
    uint32_t code = 0;
    for (size_t axis = 0; axis < 3; ++axis) {
        if (point[axis] > limit) code |= 1u << (axis * 2);
        if (point[axis] < -limit) code |= 2u << (axis * 2);
    }
    return code;
}

ClippingMath::Result ClippingMath::ClipCorners(
        const Corners& corners, const Matrix& view, const Vector& guard, float translation_scale) {
    Result result{};
    result.matrix = view;
    result.common = 0xfffu;
    for (size_t index = 0; index < corners.size(); ++index) {
        const auto point = MathLibrary::EvaluateTransformPoint(view, corners[index], translation_scale);
        result.clipped[index] = point.output;
        result.accumulator = point.accumulator;
        for (size_t lane = 0; lane < 4; ++lane)
            result.guarded[index][lane] = Product(point.output[lane], guard[lane]);
        const auto code = (Outcode(point.output) << 6) | Outcode(result.guarded[index]);
        result.masks[index] = code;
        result.any |= code;
        result.common &= code;
    }
    return result;
}

ClippingMath::Result ClippingMath::ClipBox(
        const Box& box, const Matrix& view, const Vector& guard, const Matrix* model, float translation_scale) {
    const auto matrix = model ? MathLibrary::EvaluateMatrixProduct(*model, view).output : view;
    return ClipCorners(BoxCorners(box), matrix, guard, translation_scale);
}

}
