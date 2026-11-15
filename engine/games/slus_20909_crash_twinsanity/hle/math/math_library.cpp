#include "games/slus_20909_crash_twinsanity/hle/math/math_library.h"

#include "engine/ee/cpu/ps2_float.h"

namespace title {
namespace {

inline __attribute__((always_inline)) float Rounded(float value) {
    asm("" : "+w"(value));
    return value;
}

inline __attribute__((always_inline)) float Multiply(float a, float b) {
    return ps2_float_clamp(Rounded(ps2_float_clamp(a) * ps2_float_clamp(b)));
}

inline __attribute__((always_inline)) float Add(float a, float b) {
    return ps2_float_clamp(Rounded(ps2_float_clamp(a) + ps2_float_clamp(b)));
}

}

MathLibrary::TransformPointResult MathLibrary::EvaluateTransformPoint(
    const Matrix& matrix, const Vector& point, float translation_scale) {
    TransformPointResult result;
    for (size_t lane = 0; lane < 4; ++lane) {
        const float x = Multiply(matrix[0][lane], point[0]);
        const float xy = Add(x, Multiply(matrix[1][lane], point[1]));
        result.accumulator[lane] = Add(xy, Multiply(matrix[2][lane], point[2]));
        result.output[lane] =
            Add(result.accumulator[lane], Multiply(matrix[3][lane], translation_scale));
    }
    return result;
}

MathLibrary::RotationResult MathLibrary::EvaluateRotation(
    const Matrix& matrix, const Vector& vector) {
    RotationResult result{vector, {}};
    for (size_t lane = 0; lane < 3; ++lane) {
        result.accumulator[lane] = Add(Multiply(matrix[0][lane], vector[0]),
                                       Multiply(matrix[1][lane], vector[1]));
        result.output[lane] = Add(result.accumulator[lane], Multiply(matrix[2][lane], vector[2]));
    }
    return result;
}

MathLibrary::TransformPointResult MathLibrary::EvaluateTransformVector(
    const Matrix& matrix, const Vector& vector) {
    return EvaluateTransformPoint(matrix, vector, vector[3]);
}

MathLibrary::MatrixProductResult MathLibrary::EvaluateMatrixProduct(
    const Matrix& left, const Matrix& right) {
    MatrixProductResult result;
    for (size_t row = 0; row < 4; ++row) {
        const auto transformed = EvaluateTransformVector(right, left[row]);
        result.output[row] = transformed.output;
        result.accumulator = transformed.accumulator;
    }
    return result;
}

}
