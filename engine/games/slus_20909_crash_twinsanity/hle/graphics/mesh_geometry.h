#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "engine/gs/hle/ir/draw_ir.h"

#define TITLE_MESH_GEOMETRY_PREPARED 1

namespace title {

class MeshGeometry {
public:
    using Vector = std::array<float, 4>;
    using Matrix = std::array<Vector, 4>;
    struct Vertex {
        std::array<float, 3> position;
        std::array<float, 3> stq;
        std::array<uint8_t, 4> colour;
        bool restart = false;
    };
    struct Strip { std::vector<Vertex> vertices; };
    struct Mesh { std::vector<Strip> strips; };
    struct View {
        Matrix screen{};
        Matrix clip{};
        Vector guard{};
        std::array<float, 2> scroll{};
        bool clipped = true;
    };
    enum class Result { Ok, InvalidAsset, InvalidGeometry, UnsupportedState, LimitExceeded };
    static constexpr size_t kMaxVertices = 65536;
    static constexpr size_t kMaxOutputVertices = kMaxVertices * 24;

    struct Prepared {
        struct StripRange { uint32_t first, count; };
        std::vector<StripRange> strips;
        std::vector<Vector> positions;
        std::vector<Vector> stq;
        std::vector<std::array<uint8_t, 4>> colours;
        std::vector<uint8_t> restart;
        size_t vertex_count = 0;
    };
    using Sink = void (*)(void* user, const ps2r::gs::VertexRec* vertices, size_t count, bool fan);
    static Result Prepare(const Mesh& mesh, Prepared& out);
    static Result BuildPrepared(const Prepared& prepared, const View& view, const ps2r::gs::DrawIr& state,
                                const std::array<uint8_t, 4>* colours, Sink sink, void* user);

    using OrderSink = void (*)(void* user, const ps2r::gs::VertexRec* vertices, const uint8_t* order, size_t count);
    static Result BuildPrepared(const Prepared& prepared, const View& view, const ps2r::gs::DrawIr& state,
                                const std::array<uint8_t, 4>* colours, Sink sink, OrderSink body, void* user);
};

}
