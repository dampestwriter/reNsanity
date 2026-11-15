#pragma once

#include <cstdint>
#include <vector>

namespace ps2r {
namespace render {

void BoxHalve(const std::vector<uint8_t>& src, int cw, int ch, std::vector<uint8_t>& dst, int* nw, int* nh);

}
}
