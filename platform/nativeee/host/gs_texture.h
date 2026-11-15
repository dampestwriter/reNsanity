#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "te_gfx_layout.h"

namespace te::gfx {

struct DecodedTexture {
    int width = 0, height = 0;
    std::vector<uint32_t> rgba;
    std::vector<std::vector<uint32_t>> mips;
};

bool DecodeTextureResource(const te_gfx_texture_header& header, const uint8_t* packet, size_t size,
                           DecodedTexture& out, std::string* error);

}
