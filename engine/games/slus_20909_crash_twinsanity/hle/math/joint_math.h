#pragma once

#include "games/slus_20909_crash_twinsanity/hle/math/math_library.h"

namespace title {

class JointMath {
public:
    using Vector = MathLibrary::Vector;
    using Matrix = MathLibrary::Matrix;
    using Basis = std::array<Vector, 3>;
    struct Interpolation {
        Vector rotation;
        float from_weight, to_weight;
        bool curved;
    };
    static Interpolation Interpolate(const Vector& from, const Vector& to, float share);

    static Basis RotationBasis(const Vector& rotation, const Vector& parent_scale,
                               const Vector* own_scale = nullptr);

    static Matrix MultiplyByParent(const Matrix& matrix, const Matrix& parent);

    static Vector Turn(const Vector& by, const Vector& rotation);
    struct EulerResult {
        Vector rotation;
        Vector move;
    };

    static EulerResult EulerRotation(const Vector& angles_now, const Vector& angles_next,
                                     const Vector& move_now, const Vector& move_next);

    static Vector Lerp(const Vector& from, const Vector& to, float share);

    static float ScalarSum(float a, float b);
    static float ScalarDifference(float a, float b);
    static float ScalarProduct(float a, float b);

private:
    class Rounding;
    static float Float(uint32_t bits);
    static float Finite(float value);
    static float Product(float a, float b);
    static float Sum(float a, float b);
    static float Difference(float a, float b);
    static float MultiplyAdd(float a, float b, float sum);
    static float Reciprocal(float value);
    static float Fold(float angle);
    static float Polynomial(float folded);
    static float Arc(float cosine);
    static float Sine(float angle);
};

}
