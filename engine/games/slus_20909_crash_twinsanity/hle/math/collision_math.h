#pragma once

#include <array>
#include <cstdint>

#include "games/slus_20909_crash_twinsanity/hle/math/math_library.h"

namespace title {

class CollisionMath {
public:
    using Vector = MathLibrary::Vector;
    using Matrix = MathLibrary::Matrix;
    using Triangle = std::array<Vector, 3>;
    using PointGroup = std::array<Vector, 3>;
    using HullSupport = std::array<PointGroup, 6>;

    struct EdgeSetup {
        Vector direction;
        std::array<Vector, 3> edges;
        Vector diagonal;
        Vector normal;
        std::array<Vector, 3> edge_normals;

        std::array<Vector, 6> pending;
        std::array<Vector, 2> plane_products;
        std::array<float, 2> plane_values;
        Vector last_cross_products;
    };

    struct EdgeValues {
        std::array<Vector, 6> products;
        std::array<float, 6> values;
    };

    struct Projection {
        Vector values;
        Vector partial;
    };

    struct SupportProjection {
        std::array<float, 2> range;
        Vector minimum;
        Vector maximum;
        Vector lower_three;
        Vector upper_four;
        Vector upper_five;
        Vector penultimate;
        Vector partial;
    };

    static EdgeSetup EvaluateStartEdges(const Vector& start, const Vector& end, const Triangle& triangle);
    static EdgeValues EvaluateFinishEdges(const EdgeSetup& pending);
    static PointGroup PackPoints(const std::array<Vector, 4>& points);
    static Projection ProjectGroup(const PointGroup& group, const Vector& axis);
    static HullSupport BuildHullSupport(const Triangle& triangle, const std::array<Vector, 8>& hull);
    static SupportProjection ProjectHullSupport(const HullSupport& support, const Vector& axis);
};

}
