#pragma once

#include <array>
#include <cstdint>

namespace title {

class RayMath {
public:
    using Vector = std::array<float, 4>;
    using Triangle = std::array<Vector, 3>;
    static constexpr uint32_t kNearer = 0x900;

    struct Ray {
        Vector start, end;
    };
    struct Result {
        bool nearer;
        uint32_t flags;
        Vector hit;
    };
    static Ray SetRay(const Vector& start, const Vector& end) { return Ray{start, end}; }
    static Result Test(const Ray& ray, const Triangle& triangle, const Vector& nearest);
};

}
