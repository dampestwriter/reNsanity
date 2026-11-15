#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace title {

class CullingMath {
public:
    using Vector = std::array<float, 4>;
    using Matrix = std::array<Vector, 4>;

    struct View {
        Matrix to_clip;
        Matrix to_screen;
        Vector camera;
    };
    struct Link {
        Matrix to_clip;
        Matrix to_screen;
        Vector camera;
    };
    struct Model {
        Matrix to_screen;
        int32_t distance;
    };
    struct PlaneColumns {
        std::array<Vector, 3> absolute_normals;
        std::array<Vector, 3> normals;
        Vector offsets;
    };
    struct Frustum {
        PlaneColumns sides;
        PlaneColumns depth;
        PlaneColumns far_sides;
    };
    struct Box {
        Vector minimum;
        Vector maximum;
    };
    struct Classification {
        uint32_t outside;
        uint32_t clipped;
    };
    struct BoxView {
        Classification visibility;
        int32_t distance;
        std::optional<Matrix> to_screen;
        std::optional<Matrix> to_clip;
    };

    static Link LoadLink(const View& view, const Matrix& chunk, const Matrix& object);

    static Model LoadModel(const Link& link, const Matrix& model);

    static Classification TestBox(const Frustum& frustum, const Box& box);

    static BoxView TestBoxAt(
        const Frustum& frustum, const Link& link, const Box& box, const Matrix& model);

private:
    class Rounding;
    static float Finite(float value);
    static float Product(float a, float b);
    static float Add(float a, float b);
    static float Difference(float a, float b);
    static float MultiplyAdd(float a, float b, float sum);
    static Vector Transform(const Matrix& basis, const Vector& coordinates);
    static int32_t Distance(const Vector& camera, const Vector& position);
    static Classification Classify(
        const PlaneColumns& planes, const Vector& center, const Vector& half);
    static PlaneColumns TransformPlanes(const PlaneColumns& planes, const Matrix& model);
};

}
