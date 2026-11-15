#pragma once

#include <cstddef>
#include <cstdint>

namespace ps2r {
namespace gs {

struct VertexRec {
    float x, y, z;
    float s, t, q;
    uint16_t uv_u, uv_v;
    uint8_t r, g, b, a;
    uint8_t fog;
    uint8_t draw_kick;
    uint8_t pad[2];
};

static_assert(sizeof(VertexRec) == 36, "VertexRec is 36 bytes on disk");
static_assert(offsetof(VertexRec, uv_u) == 24 && offsetof(VertexRec, r) == 28 && offsetof(VertexRec, fog) == 32,
              "VertexRec's on-disk field offsets");

}
}
