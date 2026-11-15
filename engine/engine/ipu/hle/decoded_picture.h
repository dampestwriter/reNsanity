#pragma once

#include <cstdint>
#include <vector>

namespace ps2r::ipu {

struct DecodedPicture {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
    std::vector<uint8_t> yuv;
};

}
