#pragma once

#include <cstdint>

namespace ps2r {
namespace render {

enum class SurfaceFormat : uint8_t { Rgba8 = 0, Depth32f = 1, R8 = 2, DepthStencil24_8 = 3 };

}
}
