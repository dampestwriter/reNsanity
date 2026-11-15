#include "engine/gs/format/block_decode.h"

#include <cstring>
#include <memory>
#include <mutex>

#include "engine/gs/format/clut_layout.h"
#include "engine/gs/format/pixel_convert.h"
#include "engine/gs/format/psm_layout.h"

namespace ps2r {
namespace gs {

namespace {

int PsmIndex(Psm psm) {
    const PsmDescriptor* t = PsmLayout::Table();
    for (int i = 0; i < kPsmCount; i++)
        if (t[i].psm == psm) return i;
    return 0;
}

struct PageLutTable {
    std::once_flag once[kPsmCount];
    std::unique_ptr<uint16_t[]> lut[kPsmCount];
};
PageLutTable& Luts() {
    static PageLutTable t;
    return t;
}

struct Hoisted {
    const PsmDescriptor* d;
    const uint16_t* lut;
    uint32_t pages_per_row;
    uint64_t base_elements;
    uint64_t per_page;
    uint64_t vram_elements;
};

Hoisted Hoist(const BlockDecode::Texture& tex) {
    Hoisted h;
    h.d = &PsmLayout::Describe(tex.psm);
    h.lut = BlockDecode::PageLut(tex.psm);
    h.pages_per_row = PsmLayout::PagesPerRow(tex.psm, tex.tbw);
    h.base_elements = uint64_t(tex.tbp) * PsmLayout::ElementsPerBlock(tex.psm);
    h.per_page = PsmLayout::ElementsPerPage(tex.psm);
    h.vram_elements = uint64_t(BlockDecode::kVramBytes) * 8u / h.d->storage_bits;
    return h;
}

inline uint32_t FetchElement(const uint8_t* vram, const PsmDescriptor& d, uint32_t e) {
    switch (d.storage_bits) {
        case 32: { uint32_t v; std::memcpy(&v, vram + size_t(e) * 4u, 4); return v; }
        case 16: { uint16_t v; std::memcpy(&v, vram + size_t(e) * 2u, 2); return v; }
        case 8:  return vram[e];
        default: return uint32_t(vram[e >> 1] >> ((e & 1u) * 4u)) & 0xFu;
    }
}

template <typename Emit>
void WalkRows(const uint8_t* vram, const Hoisted& h, const BlockDecode::Rect& rect,
              uint32_t* out, Emit&& emit) {
    const PsmDescriptor& d = *h.d;
    uint32_t scratch[128];
    for (uint32_t ry = 0; ry < rect.h; ry++) {
        const uint32_t y = rect.y + ry;
        const uint16_t* lrow = h.lut + (y % d.page_h) * d.page_w;
        const uint64_t row_page = uint64_t(y / d.page_h) * h.pages_per_row;
        uint32_t* orow = out + size_t(ry) * rect.w;
        uint32_t x = rect.x;
        const uint32_t xend_all = rect.x + rect.w;
        while (x < xend_all) {
            const uint32_t page_x = x / d.page_w;
            const uint32_t xend = (page_x + 1u) * d.page_w < xend_all ? (page_x + 1u) * d.page_w : xend_all;
            const uint64_t page_base = h.base_elements + (row_page + page_x) * h.per_page;
            const uint32_t n = xend - x;
            for (uint32_t i = 0; i < n; i++) {
                uint64_t e = page_base + lrow[(x + i) % d.page_w];
                if (e >= h.vram_elements) e %= h.vram_elements;
                scratch[i] = FetchElement(vram, d, uint32_t(e));
            }
            emit(orow + (x - rect.x), scratch, n);
            x = xend;
        }
    }
}

}

uint32_t BlockDecode::PaletteEntries(Psm psm) {
    const PsmDescriptor& d = PsmLayout::Describe(psm);
    if (!d.is_indexed) return 0;
    return d.index_bits == 8 ? ClutLayout::kEntries8 : ClutLayout::kEntries4;
}

const uint16_t* BlockDecode::PageLut(Psm psm) {
    const int i = PsmIndex(psm);
    PageLutTable& t = Luts();
    std::call_once(t.once[i], [&] {
        const PsmDescriptor& d = PsmLayout::Describe(psm);
        const uint32_t n = uint32_t(d.page_w) * d.page_h;
        std::unique_ptr<uint16_t[]> lut(new uint16_t[n]);
        for (uint32_t ly = 0; ly < d.page_h; ly++)
            for (uint32_t lx = 0; lx < d.page_w; lx++)
                lut[ly * d.page_w + lx] = uint16_t(PsmLayout::ElementInPage(psm, lx, ly));
        t.lut[i] = std::move(lut);
    });
    return t.lut[i].get();
}

uint32_t BlockDecode::TexelValue(Psm psm, uint32_t element, const TexaExpand::Texa& texa,
                                 const uint32_t* palette) {
    switch (psm) {
        case Psm::CT32:  return element;
        case Psm::CT24:  return TexaExpand::ApplyTexa24(element, texa);
        case Psm::CT16:
        case Psm::CT16S: return TexaExpand::ApplyTexa16(uint16_t(element), texa);
        case Psm::T8: case Psm::T4: case Psm::T8H: case Psm::T4HL: case Psm::T4HH:
            return palette[PixelConvert::UnpackIndex(psm, element)];
        case Psm::Z32:   return element;
        case Psm::Z24:   return element & 0x00FFFFFFu;
        case Psm::Z16:
        case Psm::Z16S:  return TexaExpand::Expand16To32(uint16_t(element));
    }
    return 0;
}

void BlockDecode::ReadPalette(const uint8_t* vram, const Palette& pal, uint32_t entries,
                              const TexaExpand::Texa& texa, uint32_t* out) {
    const bool wide = PsmLayout::Describe(pal.cpsm).storage_bits == 32;
    for (uint32_t i = 0; i < entries; i++) {
        if (wide) out[i] = ClutLayout::ReadClut32(vram, pal.cbp, pal.csm2, entries, i);
        else      out[i] = TexaExpand::ApplyTexa16(ClutLayout::ReadClut16(vram, pal.cpsm, pal.cbp, pal.csm2, entries, i), texa);
    }
}

void BlockDecode::DecodeScalar(const uint8_t* vram, const Texture& tex, const Rect& rect,
                               const TexaExpand::Texa& texa, const uint32_t* palette, uint32_t* out) {
    const Hoisted h = Hoist(tex);
    const Psm psm = tex.psm;
    WalkRows(vram, h, rect, out, [&](uint32_t* o, const uint32_t* el, uint32_t n) {
        for (uint32_t i = 0; i < n; i++) o[i] = TexelValue(psm, el[i], texa, palette);
    });
}

void BlockDecode::Decode(const uint8_t* vram, const Texture& tex, const Rect& rect,
                         const TexaExpand::Texa& texa, const uint32_t* palette, uint32_t* out) {
    DecodeScalar(vram, tex, rect, texa, palette, out);
}

void BlockDecode::Decode(const uint8_t* vram, const Texture& tex,
                         const TexaExpand::Texa& texa, const uint32_t* palette, uint32_t* out) {
    Decode(vram, tex, Rect{0, 0, tex.w, tex.h}, texa, palette, out);
}

}
}
