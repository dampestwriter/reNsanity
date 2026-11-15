#include "engine/gs/format/clut_layout.h"

#include <cstddef>

#include "engine/gs/format/psm_layout.h"

namespace ps2r {
namespace gs {

uint32_t ClutLayout::Csm1Index(uint32_t entry) {

    const uint32_t in_group = entry & 31u;
    if (in_group >= 8u && in_group < 16u) return entry + 8u;
    if (in_group >= 16u && in_group < 24u) return entry - 8u;
    return entry;
}

ClutLayout::Position ClutLayout::Csm1Position(uint32_t entry, uint32_t entries) {
    if (entries <= kEntries4) {

        return Position{entry & 7u, entry >> 3};
    }
    const uint32_t p = Csm1Index(entry);
    return Position{p & 15u, p >> 4};
}

uint32_t ClutLayout::EntryAddress(Psm cpsm, uint32_t cbp, bool csm2, uint32_t entries, uint32_t entry) {
    const PsmDescriptor& d = PsmLayout::Describe(cpsm);
    if (csm2 && d.storage_bits == 16) {

        const uint64_t elements = uint64_t(PsmLayout::kVramBytes) / 2u;
        return static_cast<uint32_t>((uint64_t(cbp) * PsmLayout::ElementsPerBlock(cpsm) + entry) % elements);
    }
    Position pos;
    if (csm2) {

        pos = entries <= kEntries4 ? Position{entry & 7u, entry >> 3} : Position{entry & 15u, entry >> 4};
    } else {
        pos = Csm1Position(entry, entries);
    }
    return PsmLayout::PixelAddress(cpsm, pos.x, pos.y, cbp, 1u);
}

uint32_t ClutLayout::ReadClut32(const uint8_t* vram, uint32_t cbp, bool csm2, uint32_t entries, uint32_t entry) {
    const uint8_t* p = vram + static_cast<size_t>(EntryAddress(Psm::CT32, cbp, csm2, entries, entry)) * 4u;
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

uint16_t ClutLayout::ReadClut16(const uint8_t* vram, Psm cpsm, uint32_t cbp, bool csm2, uint32_t entries, uint32_t entry) {
    const uint8_t* p = vram + static_cast<size_t>(EntryAddress(cpsm, cbp, csm2, entries, entry)) * 2u;
    return static_cast<uint16_t>(uint32_t(p[0]) | (uint32_t(p[1]) << 8));
}

}
}
