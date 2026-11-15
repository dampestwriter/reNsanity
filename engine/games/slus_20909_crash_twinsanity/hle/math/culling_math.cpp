#include "games/slus_20909_crash_twinsanity/hle/math/culling_math.h"

#include <cfenv>
#include <cmath>
#include <cstring>
#include <cstdlib>

#include "engine/vu/vu_float.h"
#include <limits>

namespace title {

namespace {
using CullLanes = float __attribute__((vector_size(16)));
using CullBits = uint32_t __attribute__((vector_size(16)));
CullLanes CullLoad(const CullingMath::Vector& v) {
    CullLanes q;
    std::memcpy(&q, v.data(), 16);
    return q;
}
CullBits CullRaw(CullLanes q) {
    CullBits v;
    std::memcpy(&v, &q, 16);
    return v;
}
CullLanes CullFloat(CullBits q) {
    CullLanes v;
    std::memcpy(&v, &q, 16);
    return v;
}
CullLanes CullFinite(CullLanes q) {
    const CullBits b = CullRaw(q), mag = b & 0x7fffffffu, sign = b & 0x80000000u;
    const CullBits full = (CullBits)(mag >= 0x7f800000u), zero = (CullBits)(mag < 0x00800000u);
    return CullFloat((full & (sign | 0x7f7fffffu)) | (zero & sign) | (~(full | zero) & b));
}
bool CullNaN(CullLanes q) {
    const CullBits b = CullRaw(q) & 0x7fffffffu;
    const CullBits n = (CullBits)(b > 0x7f800000u);
    return (n[0] | n[1] | n[2] | n[3]) != 0;
}
uint32_t CullMask(CullBits signs) {
    const CullBits b = (CullBits)((signs & 0x80000000u) != 0u) & CullBits{0x80u, 0x40u, 0x20u, 0x10u};
    return b[0] | b[1] | b[2] | b[3];
}
}

class CullingMath::Rounding { public: Rounding() {} };

float CullingMath::Finite(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t magnitude = bits & 0x7fffffff;
    if (magnitude >= 0x7f800000) bits = (bits & 0x80000000) | 0x7f7fffff;
    else if (magnitude < 0x00800000) bits &= 0x80000000;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

float CullingMath::Product(float a, float b) { return a * b; }
float CullingMath::Add(float a, float b) { return a + b; }
float CullingMath::Difference(float a, float b) { return a - b; }
float CullingMath::MultiplyAdd(float a, float b, float sum) { return a * b + sum; }

CullingMath::Vector CullingMath::Transform(const Matrix& basis, const Vector& coordinates) {
    Vector result;
    for (size_t lane = 0; lane < result.size(); ++lane) {
        float sum = Product(basis[0][lane], coordinates[0]);
        sum = MultiplyAdd(basis[1][lane], coordinates[1], sum);
        sum = MultiplyAdd(basis[2][lane], coordinates[2], sum);
        result[lane] = MultiplyAdd(basis[3][lane], coordinates[3], sum);
    }
    return result;
}

CullingMath::Link CullingMath::LoadLink(
        const View& view, const Matrix& chunk, const Matrix& object) {
    Rounding rounding;
    Link link;
    for (size_t row = 0; row < chunk.size(); ++row) {
        link.to_clip[row] = Transform(view.to_clip, chunk[row]);
        link.to_screen[row] = Transform(view.to_screen, chunk[row]);
    }

    for (size_t lane = 0; lane < link.camera.size(); ++lane) {
        float sum = Product(object[3][lane], 1.0f);
        sum = MultiplyAdd(object[0][lane], view.camera[0], sum);
        sum = MultiplyAdd(object[1][lane], view.camera[1], sum);
        link.camera[lane] = MultiplyAdd(object[2][lane], view.camera[2], sum);
    }
    return link;
}

int32_t CullingMath::Distance(const Vector& camera, const Vector& position) {
    Vector squared;
    for (size_t lane = 0; lane < 3; ++lane) {
        const float delta = Difference(camera[lane], position[lane]);
        squared[lane] = Product(delta, delta);
    }
    const float sum = Add(Add(squared[0], squared[1]), squared[2]);
    if (sum >= 2147483648.0f) return std::numeric_limits<int32_t>::max();
    return static_cast<int32_t>(sum);
}

CullingMath::Model CullingMath::LoadModel(const Link& link, const Matrix& model) {
    Rounding rounding;
    Model result;
    for (size_t row = 0; row < model.size(); ++row)
        result.to_screen[row] = Transform(link.to_screen, model[row]);
    result.distance = Distance(link.camera, model[3]);
    return result;
}

CullingMath::Classification CullingMath::Classify(
        const PlaneColumns& planes, const Vector& center, const Vector& half) {
    {
        CullLanes radius = CullLoad(planes.absolute_normals[0]) * half[0];
        radius = CullLoad(planes.absolute_normals[1]) * half[1] + radius;
        radius = CullLoad(planes.absolute_normals[2]) * half[2] + radius;
        CullLanes distance = CullLoad(planes.normals[0]) * center[0];
        distance = CullLoad(planes.normals[1]) * center[1] + distance;
        distance = CullLoad(planes.normals[2]) * center[2] + distance;
        distance = CullLoad(planes.offsets) * 1.0f + distance;
        const CullBits high = CullRaw(radius * 1.0f + distance);
        const CullBits low = CullRaw((-CullFinite(radius)) * 1.0f + distance);

        if (!CullNaN(CullFloat(high)) && !CullNaN(CullFloat(low)))
            return {CullMask(high & low), CullMask(high | low)};
    }
    Classification result{};
    for (size_t plane = 0; plane < 4; ++plane) {
        float radius = Product(planes.absolute_normals[0][plane], half[0]);
        radius = MultiplyAdd(planes.absolute_normals[1][plane], half[1], radius);
        radius = MultiplyAdd(planes.absolute_normals[2][plane], half[2], radius);
        float distance = Product(planes.normals[0][plane], center[0]);
        distance = MultiplyAdd(planes.normals[1][plane], center[1], distance);
        distance = MultiplyAdd(planes.normals[2][plane], center[2], distance);
        distance = MultiplyAdd(planes.offsets[plane], 1.0f, distance);
        const bool high_negative = std::signbit(MultiplyAdd(radius, 1.0f, distance));
        const bool low_negative = std::signbit(MultiplyAdd(-Finite(radius), 1.0f, distance));

        const uint32_t mask = 0x80u >> plane;
        if (high_negative && low_negative) result.outside |= mask;
        if (high_negative || low_negative) result.clipped |= mask;
    }
    return result;
}

CullingMath::Classification CullingMath::TestBox(const Frustum& frustum, const Box& box) {
    Rounding rounding;
    Vector center{}, half{};
    for (size_t axis = 0; axis < 3; ++axis) {
        half[axis] = Product(Difference(box.maximum[axis], box.minimum[axis]), 0.5f);
        center[axis] = MultiplyAdd(box.maximum[axis], 0.5f, Product(box.minimum[axis], 0.5f));
    }
    const auto sides = Classify(frustum.sides, center, half);
    const auto depth = Classify(frustum.depth, center, half);
    return {sides.outside | depth.outside, sides.clipped | depth.clipped};
}

CullingMath::PlaneColumns CullingMath::TransformPlanes(
        const PlaneColumns& planes, const Matrix& model) {
    PlaneColumns result;
    for (size_t plane = 0; plane < 4; ++plane) {
        for (size_t axis = 0; axis < 3; ++axis) {
            float sum = Product(planes.normals[0][plane], model[axis][0]);
            sum = MultiplyAdd(planes.normals[1][plane], model[axis][1], sum);
            sum = MultiplyAdd(planes.normals[2][plane], model[axis][2], sum);
            result.normals[axis][plane] = sum;
            result.absolute_normals[axis][plane] = std::fabs(sum);
        }
        float sum = Product(planes.normals[0][plane], model[3][0]);
        sum = MultiplyAdd(planes.normals[1][plane], model[3][1], sum);
        sum = MultiplyAdd(planes.normals[2][plane], model[3][2], sum);
        result.offsets[plane] = MultiplyAdd(planes.offsets[plane], 1.0f, sum);
    }
    return result;
}

CullingMath::BoxView CullingMath::TestBoxAt(
        const Frustum& frustum, const Link& link, const Box& box, const Matrix& model) {
    Rounding rounding;
    Vector center{}, half{};
    for (size_t axis = 0; axis < 3; ++axis) {
        half[axis] = Product(Difference(box.maximum[axis], box.minimum[axis]), 0.5f);
        center[axis] = MultiplyAdd(box.maximum[axis], 0.5f, Product(box.minimum[axis], 0.5f));
    }
    const auto depth = Classify(TransformPlanes(frustum.depth, model), center, half);
    const auto sides = Classify(TransformPlanes(frustum.sides, model), center, half);
    BoxView result{};
    result.distance = Distance(link.camera, model[3]);
    result.visibility.clipped = sides.outside | (depth.clipped & 0x40u);
    result.visibility.outside = depth.outside | result.visibility.clipped;
    if (result.visibility.outside) return result;

    const auto far = Classify(TransformPlanes(frustum.far_sides, model), center, half);
    result.visibility.clipped = far.clipped | depth.clipped;
    Matrix screen;
    for (size_t row = 0; row < 4; ++row) screen[row] = Transform(link.to_screen, model[row]);
    result.to_screen = screen;
    if (result.visibility.clipped) {
        Matrix clip;
        for (size_t row = 0; row < 4; ++row) clip[row] = Transform(link.to_clip, model[row]);
        result.to_clip = clip;
    }
    return result;
}

}
