#pragma once

#include <cstdint>

#include "engine/gs/format/psm.h"

namespace ps2r {
namespace gs {

class ClutLayout {
public:
    static constexpr uint32_t kEntries8 = 256;
    static constexpr uint32_t kEntries4 = 16;

    struct Position { uint32_t x, y; };

    static uint32_t Csm1Index(uint32_t entry);

    static Position Csm1Position(uint32_t entry, uint32_t entries);

    static uint32_t EntryAddress(Psm cpsm, uint32_t cbp, bool csm2, uint32_t entries, uint32_t entry);

    static uint32_t ReadClut32(const uint8_t* vram, uint32_t cbp, bool csm2, uint32_t entries, uint32_t entry);
    static uint16_t ReadClut16(const uint8_t* vram, Psm cpsm, uint32_t cbp, bool csm2, uint32_t entries, uint32_t entry);
};

}
}
