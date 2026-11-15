#pragma once

#include <array>
#include <cstdint>

namespace title {

class ParticleViewMath {
public:
    using Vector = std::array<float, 4>;
    using Matrix = std::array<Vector, 4>;
    struct View {
        std::array<Vector, 19> qw{};
    };
    struct Result {
        Matrix matrix;
        float distance;
        uint32_t flags;
    };

    static Result BlockView(const View& view, const Vector& extents, const Matrix& gravity, const Vector& place, float scale,
                            bool keepsTranslation);
};

}
