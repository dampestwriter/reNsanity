#pragma once

#include <cstdint>

#include "engine/gs/format/psm.h"
#include "engine/gs/format/texa_expand.h"

namespace ps2r {
namespace gs {

class BlockDecode {
public:
    static constexpr uint32_t kVramBytes = 4u * 1024u * 1024u;
    static constexpr uint32_t kPageBytes = 8192;

    struct Texture {
        Psm psm;
        uint32_t tbp;
        uint32_t tbw;
        uint32_t w, h;
    };

    struct Palette {
        Psm cpsm;
        uint32_t cbp;
        bool csm2;
    };

    struct Rect { uint32_t x, y, w, h; };

    struct Footprint { uint32_t start, size; };

    static uint32_t PaletteEntries(Psm psm);

    static const uint16_t* PageLut(Psm psm);

    static void ReadPalette(const uint8_t* vram, const Palette& pal, uint32_t entries,
                            const TexaExpand::Texa& texa, uint32_t* out);

    static void Decode(const uint8_t* vram, const Texture& tex, const Rect& rect,
                       const TexaExpand::Texa& texa, const uint32_t* palette, uint32_t* out);

    static void Decode(const uint8_t* vram, const Texture& tex,
                       const TexaExpand::Texa& texa, const uint32_t* palette, uint32_t* out);

    static void DecodeScalar(const uint8_t* vram, const Texture& tex, const Rect& rect,
                             const TexaExpand::Texa& texa, const uint32_t* palette, uint32_t* out);

    static uint32_t TexelValue(Psm psm, uint32_t element, const TexaExpand::Texa& texa,
                               const uint32_t* palette);
};

}
}
