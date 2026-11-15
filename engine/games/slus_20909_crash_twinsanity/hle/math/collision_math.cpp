#include "games/slus_20909_crash_twinsanity/hle/math/collision_math.h"

#include <algorithm>
#include <cstring>

#include "engine/ee/cpu/ps2_float.h"
#include "games/slus_20909_crash_twinsanity/hle/math/float_fence.h"

namespace title {
namespace {

using Vector = CollisionMath::Vector;
using PointGroup = CollisionMath::PointGroup;

float Product(float a, float b) {
    return ps2_float_clamp(FencedFloat(ps2_float_clamp(a) * ps2_float_clamp(b)));
}

float Sum(float a, float b) {
    return ps2_float_clamp(FencedFloat(ps2_float_clamp(a) + ps2_float_clamp(b)));
}

float Difference(float a, float b) {
    return ps2_float_clamp(FencedFloat(ps2_float_clamp(a) - ps2_float_clamp(b)));
}

Vector SubtractPoints(const Vector& a, const Vector& b) {
    return {Difference(a[0], b[0]), Difference(a[1], b[1]), Difference(a[2], b[2]), 0};
}

Vector MultiplyPoints(const Vector& a, const Vector& b) {
    return {Product(a[0], b[0]), Product(a[1], b[1]), Product(a[2], b[2]), 0};
}

Vector CrossTerms(const Vector& a, const Vector& b) {
    Vector value{};
    for (size_t lane = 0; lane < 3; ++lane) {
        value[lane] = FencedFloat(a[(lane + 1) % 3] * b[(lane + 2) % 3]);
    }
    return value;
}

Vector Cross(const Vector& a, const Vector& b) {
    const auto first = CrossTerms(a, b), second = CrossTerms(b, a);
    Vector value{};
    for (size_t lane = 0; lane < 3; ++lane) {
        value[lane] = FencedFloat(first[lane] - second[lane]);
    }
    return value;
}

float DotSum(const Vector& products) {
    return Sum(Sum(products[0], products[1]), products[2]);
}

Vector Minimum(const Vector& a, const Vector& b) {
    Vector value;
    for (size_t i = 0; i < 4; ++i) value[i] = std::min(a[i], b[i]);
    return value;
}

Vector Maximum(const Vector& a, const Vector& b) {
    Vector value;
    for (size_t i = 0; i < 4; ++i) value[i] = std::max(a[i], b[i]);
    return value;
}

}

CollisionMath::EdgeSetup CollisionMath::EvaluateStartEdges(
    const Vector& start, const Vector& end, const Triangle& triangle) {
    EdgeSetup result{};
    result.direction = SubtractPoints(end, start);
    result.diagonal = SubtractPoints(triangle[2], triangle[0]);
    for (size_t edge = 0; edge < 3; ++edge) {
        result.edges[edge] = SubtractPoints(triangle[(edge + 1) % 3], triangle[edge]);
        result.edge_normals[edge] = Cross(result.direction, result.edges[edge]);
    }
    result.normal = Cross(result.edges[0], result.diagonal);
    result.plane_products[0] = MultiplyPoints(SubtractPoints(start, triangle[0]), result.normal);
    result.plane_products[1] = MultiplyPoints(SubtractPoints(end, triangle[0]), result.normal);
    for (size_t i = 0; i < 2; ++i) result.plane_values[i] = DotSum(result.plane_products[i]);
    constexpr size_t vertices[] = {0, 2, 1, 0, 2, 1};
    for (size_t i = 0; i < 6; ++i) result.pending[i] = SubtractPoints(triangle[vertices[i]], start);
    for (size_t i = 0; i < 2; ++i) result.pending[i] = MultiplyPoints(result.pending[i], result.edge_normals[0]);
    result.last_cross_products = CrossTerms(result.direction, result.edges[2]);
    return result;
}

CollisionMath::EdgeValues CollisionMath::EvaluateFinishEdges(const EdgeSetup& pending) {
    EdgeValues result{pending.pending, {}};
    for (size_t i = 2; i < 6; ++i)
        result.products[i] = MultiplyPoints(pending.pending[i], pending.edge_normals[i / 2]);
    for (size_t i = 0; i < 6; ++i) result.values[i] = DotSum(result.products[i]);
    return result;
}

CollisionMath::PointGroup CollisionMath::PackPoints(const std::array<Vector, 4>& points) {
    PointGroup result;
    for (size_t component = 0; component < 3; ++component)
        for (size_t point = 0; point < 4; ++point)
            std::memcpy(&result[component][point], &points[point][component], sizeof(float));
    return result;
}

CollisionMath::Projection CollisionMath::ProjectGroup(const PointGroup& group, const Vector& axis) {
    Projection result;
    for (size_t lane = 0; lane < 4; ++lane) {
        result.partial[lane] = Sum(Product(group[0][lane], axis[0]), Product(group[1][lane], axis[1]));
        result.values[lane] = Sum(result.partial[lane], Product(group[2][lane], axis[2]));
    }
    return result;
}

CollisionMath::HullSupport CollisionMath::BuildHullSupport(
    const Triangle& triangle, const std::array<Vector, 8>& hull) {
    HullSupport result;
    for (size_t group = 0; group < 6; ++group) {
        std::array<Vector, 4> points;
        for (size_t point = 0; point < 4; ++point)
            points[point] = SubtractPoints(triangle[group / 2], hull[(group % 2) * 4 + point]);
        result[group] = PackPoints(points);
    }
    return result;
}

CollisionMath::SupportProjection CollisionMath::ProjectHullSupport(
    const HullSupport& support, const Vector& axis) {
    std::array<Projection, 6> projections;
    for (size_t i = 0; i < 6; ++i) projections[i] = ProjectGroup(support[i], axis);
    Vector minimum = Minimum(projections[0].values, projections[1].values);
    Vector maximum = Maximum(projections[0].values, projections[1].values);
    SupportProjection result{};
    minimum = Minimum(minimum, projections[2].values);
    maximum = Maximum(maximum, projections[2].values);
    result.lower_three = minimum;
    minimum = Minimum(minimum, projections[3].values);
    maximum = Maximum(maximum, projections[3].values);
    result.upper_four = maximum;
    minimum = Minimum(minimum, projections[4].values);
    maximum = Maximum(maximum, projections[4].values);
    result.upper_five = maximum;
    result.minimum = Minimum(minimum, projections[5].values);
    result.maximum = Maximum(maximum, projections[5].values);
    result.penultimate = projections[4].values;
    result.partial = projections[5].partial;
    result.range = {std::min(std::min(result.minimum[0], result.minimum[1]),
                             std::min(result.minimum[2], result.minimum[3])),
                    std::max(std::max(result.maximum[0], result.maximum[1]),
                             std::max(result.maximum[2], result.maximum[3]))};
    return result;
}

}
