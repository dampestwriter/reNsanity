#pragma once

#include <cstddef>

#include "engine/render/gpu_vertex.h"

namespace ps2r {
namespace gs {
struct VertexRec;
}

namespace render {

class VertexConvert {
public:
    enum class UvMode : int { Untextured = 0, UvFixed = 1, Stq = 2 };

    struct Bbox {
        float x0, y0, x1, y1;
    };
    struct FusedBboxes {
        Bbox tri;
        Bbox all;
        bool valid;
    };

    static FusedBboxes Convert(const gs::VertexRec* src, size_t n, GpuVertex* dst, UvMode mode,
                               int uv_tex_w, int uv_tex_h);

private:
    static void ConvertRange(const gs::VertexRec* src, size_t i0, size_t i1, GpuVertex* dst,
                             UvMode mode, float fw, float fh, Bbox& box);
};

}
}
