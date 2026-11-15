#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "engine/gs/hle/ir/draw_ir.h"

namespace title {

class ShadowGeometry {
public:
    using Vector = std::array<float, 4>;
    using Matrix = std::array<Vector, 4>;
    struct Vertex {
        std::array<float, 3> position{};
        bool restart = false;

        bool reverse_fan = false;
    };
    struct Strip { std::vector<Vertex> vertices; };
    struct Mesh { std::vector<Strip> strips; };
    struct View {
        Matrix screen{}, clip{};
        Vector guard{};
        std::array<float, 2> offset{};
    };
    enum class Result { Ok, InvalidAsset, InvalidGeometry, LimitExceeded };
    static constexpr size_t kMaxVertices = 65536;
    static constexpr size_t kMaxOutputVertices = 2 * 24 * kMaxVertices;

    static Result Decode(const uint8_t*, size_t, uint32_t vertices, Mesh&);

    static Result Build(const Mesh&, const std::vector<View>&,
                        std::vector<ps2r::gs::DrawIr>& out);

    using Sink = void (*)(void* user, const ps2r::gs::VertexRec* vertices, size_t count, bool fan);
    static Result Build(const Mesh&, const View* views, size_t count, Sink sink, void* user);

    static Result GpuTriangles(const Mesh&, std::vector<std::array<float, 3>>& out, std::array<float, 6>& box);

    static bool InsideGuard(const std::array<float, 6>& box, const View& view);
};

}
