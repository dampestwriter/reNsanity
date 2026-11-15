#pragma once

#include "games/slus_20909_crash_twinsanity/hle/math/bounds_math.h"

namespace title {

class ClippingMath {
public:
    using Vector = MathLibrary::Vector;
    using Matrix = MathLibrary::Matrix;
    using Box = BoundsMath::Box;
    using Corners = std::array<Vector, 8>;
    struct Result {
        Matrix matrix;
        Corners clipped, guarded;
        Vector accumulator;
        std::array<uint32_t, 8> masks;
        uint32_t any, common;
    };

    static Corners BoxCorners(const Box& box);
    static Result ClipCorners(const Corners& corners, const Matrix& view, const Vector& guard,
                              float translation_scale = 1);
    static Result ClipBox(const Box& box, const Matrix& view, const Vector& guard,
                          const Matrix* model = nullptr, float translation_scale = 1);

private:
    static uint32_t Outcode(const Vector& point);
};

}
