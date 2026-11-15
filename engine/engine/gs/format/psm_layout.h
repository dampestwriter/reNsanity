#pragma once

#include <cstdint>

#include "engine/gs/format/psm.h"

namespace ps2r {
namespace gs {

class PsmLayout {
public:
    static constexpr uint32_t kVramBytes      = 4u * 1024u * 1024u;
    static constexpr uint32_t kPageBytes      = 8192;
    static constexpr uint32_t kBlockBytes     = 256;
    static constexpr uint32_t kColumnBytes    = 64;
    static constexpr uint32_t kBlocksPerPage  = 32;
    static constexpr uint32_t kColumnsPerBlock = 4;
    static constexpr uint32_t kPageCount      = kVramBytes / kPageBytes;
    static constexpr uint32_t kBlockCount     = kVramBytes / kBlockBytes;

    static const PsmDescriptor& Describe(Psm psm);

    static const PsmDescriptor* Table();

    static bool FromField(uint32_t field, Psm* out);

    static uint32_t ElementsPerPage(Psm psm);
    static uint32_t ElementsPerBlock(Psm psm);
    static uint32_t ElementsPerColumn(Psm psm);

    static uint32_t PagesPerRow(Psm psm, uint32_t bw);

    static uint32_t BlockInPage(Psm psm, uint32_t block_x, uint32_t block_y);

    static uint32_t ElementInBlock(Psm psm, uint32_t bx, uint32_t by);

    static uint32_t ElementInPage(Psm psm, uint32_t lx, uint32_t ly);

    static uint32_t PixelAddress(Psm psm, uint32_t x, uint32_t y, uint32_t bp, uint32_t bw);

    static uint32_t PixelByteAddress(Psm psm, uint32_t x, uint32_t y, uint32_t bp, uint32_t bw);

    struct RowAddresser {
        void Begin(Psm psm, uint32_t y, uint32_t bp, uint32_t bw);
        uint32_t ByteAddress(uint32_t x) const;

        Psm psm = Psm::CT32;
        uint32_t y = 0, bp = 0, bw = 0;
        uint32_t page_w = 1, block_w = 1;
        uint32_t block_y = 0, by = 0;
        uint64_t row_base = 0;
        uint64_t per_page = 1, per_block = 1, vram_elements = 1;
        uint32_t storage_bits = 32;

        uint32_t page_w_shift = 0, block_w_shift = 0, per_page_shift = 0, per_block_shift = 0;
        uint64_t vram_mask = 0;
        int byte_shift = 2;
    };
};

}
}
