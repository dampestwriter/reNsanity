#pragma once

#include <cstdint>

namespace ps2r {
namespace render {

struct GpuVertex {
    float x, y, z;
    float u, v, q;
    uint8_t r, g, b, a;
    uint8_t fog;
    uint8_t pad[3];
};

static_assert(sizeof(GpuVertex) == 32, "GpuVertex is the 32-byte attribute layout");

}
}
