#pragma once

#include <array>
#include <cstdint>

namespace title {

class MathLibrary {
public:
    using Vector = std::array<float, 4>;
    using Matrix = std::array<Vector, 4>;

    struct TransformPointResult {
        Vector output;
        Vector accumulator;
    };

    struct RotationResult {
        Vector output;
        std::array<float, 3> accumulator;
    };

    struct MatrixProductResult {
        Matrix output;
        Vector accumulator;
    };

    static TransformPointResult EvaluateTransformPoint(
        const Matrix& matrix, const Vector& point, float translation_scale = 1.0f);

    static RotationResult EvaluateRotation(const Matrix& matrix, const Vector& vector);
    static TransformPointResult EvaluateTransformVector(const Matrix& matrix, const Vector& vector);
    static MatrixProductResult EvaluateMatrixProduct(const Matrix& left, const Matrix& right);
};

}
