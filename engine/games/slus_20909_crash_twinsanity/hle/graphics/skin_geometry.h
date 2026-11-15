#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "engine/gs/hle/ir/draw_ir.h"

namespace title {

class SkinGeometry {
public:
    using Qw = std::array<uint32_t, 4>;
    struct Batch {
        Qw tag{};
        Qw count{};
        Qw scale{};
        std::vector<Qw> vertices;
        uint32_t Vertices() const { return uint32_t(vertices.size() / 4); }
    };
    struct Part { std::vector<Batch> batches; uint32_t vertices = 0; };

    struct Inputs {
        std::vector<Qw> joints;
        std::array<Qw, 4> to_screen{};
        std::array<Qw, 3> lights{};
        std::array<Qw, 3> colours{};
        Qw ambient{};
        bool clipped = false;
        std::array<Qw, 4> to_camera{};
        Qw clip{};
        std::array<float, 2> scroll{};
    };
    enum class Result { Ok, InvalidAsset, InvalidGeometry, UnsupportedState, LimitExceeded };

    struct GpuVertex {
        float position[3], normal[3], st[2];
        float bone[3], weight[3];
        int32_t colour[4];
        uint32_t index;
    };
    static Result GpuTriangles(const Part& part, std::vector<GpuVertex>& out);
    static constexpr uint32_t kMaxBatchVertices = 64;
    static constexpr uint32_t kMaxJoints = 100;
    static constexpr size_t kMaxVertices = 65536;

    static Result Decode(const uint8_t* bytes, size_t size, uint32_t vertices, Part& out);

    static Result Build(const Part& part, const Inputs& inputs, const ps2r::gs::DrawIr& state,
                        std::vector<ps2r::gs::DrawIr>& out);

    using Sink = void (*)(void* user, const ps2r::gs::VertexRec* vertices, size_t count, bool fan);
    static Result Build(const Part& part, const Inputs& inputs, const ps2r::gs::DrawIr& state, Sink sink, void* user);

    static Result DecodePart(const uint8_t* bytes, size_t size, uint32_t vertices, Part& out);
    struct Shape {
        float weight = 0.f;
        const uint8_t* rows = nullptr;
        uint32_t count = 0;
    };
    struct BlendInputs {
        std::array<float, 3> factors{};
        std::vector<Shape> shapes;
    };
    static Result BuildBlend(const Part& part, const Inputs& inputs, const BlendInputs& blend,
                             const ps2r::gs::DrawIr& state, std::vector<ps2r::gs::DrawIr>& out);
    static Result BuildBlend(const Part& part, const Inputs& inputs, const BlendInputs& blend,
                             const ps2r::gs::DrawIr& state, Sink sink, void* user);

    static bool BlendPositions(const Part& part, const BlendInputs& blend, float (*out)[4], uint32_t* count);
};

}
