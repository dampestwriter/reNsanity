#pragma once

#include <cstdint>
#include <vector>

#include "engine/gs/hle/ir/tex_read_view.h"
#include "engine/gs/hle/texsrc/clut_heal_cache.h"

namespace ps2r {
namespace gs {

class TextureDecoder {
public:
    struct Dispfb {
        uint32_t fbp_bytes = 0;
        uint32_t fbw_px = 0;
        uint32_t psm = 0;
        uint32_t dbx = 0, dby = 0;
    };
    struct Display {
        uint32_t dx = 0, dy = 0, magh = 0, magv = 0, dw = 0, dh = 0;
        int pixel_width = 0, pixel_height = 0;
    };

    static void Decode(const uint8_t* vram, const TexReadView& v, ClutHealCache* heal,
                       const VramLayout* layout, std::vector<uint32_t>* out);

    static void DecodeLevel(const uint8_t* vram, const TexReadView& v, uint32_t tbp_bytes,
                            uint32_t tbw_px, uint32_t w, uint32_t h, ClutHealCache* heal,
                            const VramLayout* layout, std::vector<uint32_t>* out);

    static bool ReadPalette(const uint8_t* vram, const TexReadView& v, ClutHealCache* heal,
                            const VramLayout* layout, uint32_t* out256);

    static uint32_t GpuAlpha(uint32_t rgba_ps2);
};

}
}
