#include "engine/gs/hle/texsrc/texture_decoder.h"

#include "engine/gs/format/block_decode.h"
#include "engine/gs/format/pixel_convert.h"
#include "engine/gs/format/psm_layout.h"
#include "engine/gs/format/texa_expand.h"

namespace ps2r {
namespace gs {

namespace {

bool ViewTexture(const TexReadView& v, uint32_t tbp_bytes, uint32_t tbw_px, uint32_t w, uint32_t h,
                 BlockDecode::Texture* t) {
    Psm psm;
    if (!PsmLayout::FromField(v.psm, &psm)) return false;
    t->psm = psm;
    t->tbp = tbp_bytes / PsmLayout::kBlockBytes;
    t->tbw = tbw_px / 64;
    t->w = w;
    t->h = h;
    return true;
}

TexaExpand::Texa ViewTexa(const TexReadView& v) {
    TexaExpand::Texa t;
    t.ta0 = v.texa_ta0;
    t.ta1 = v.texa_ta1;
    t.aem = v.texa_aem;
    return t;
}

void ScaleAlpha(std::vector<uint32_t>* out) {
    for (uint32_t& p : *out) p = TextureDecoder::GpuAlpha(p);
}

}

uint32_t TextureDecoder::GpuAlpha(uint32_t rgba) {
    const uint8_t a = PixelConvert::ScaleAlphaPs2ToGpu(PixelConvert::A(rgba));
    return (rgba & 0x00FFFFFFu) | (static_cast<uint32_t>(a) << 24);
}

bool TextureDecoder::ReadPalette(const uint8_t* vram, const TexReadView& v, ClutHealCache* heal,
                                 const VramLayout* layout, uint32_t* out) {
    Psm psm;
    if (!PsmLayout::FromField(v.psm, &psm)) return false;
    const uint32_t entries = BlockDecode::PaletteEntries(psm);
    if (entries == 0) return false;
    BlockDecode::Palette pal;
    Psm cpsm = Psm::CT32;
    PsmLayout::FromField(v.cpsm, &cpsm);
    pal.cpsm = cpsm;
    pal.cbp = v.cbp / PsmLayout::kBlockBytes;
    pal.csm2 = v.csm != 0;
    BlockDecode::ReadPalette(vram, pal, entries, ViewTexa(v), out);
    if (heal && layout && entries == 256) heal->Heal(v.cbp, out, *layout);
    return true;
}

void TextureDecoder::DecodeLevel(const uint8_t* vram, const TexReadView& v, uint32_t tbp_bytes,
                                 uint32_t tbw_px, uint32_t w, uint32_t h, ClutHealCache* heal,
                                 const VramLayout* layout, std::vector<uint32_t>* out) {
    out->assign(static_cast<size_t>(w) * h, 0u);
    BlockDecode::Texture t;
    if (!ViewTexture(v, tbp_bytes, tbw_px, w, h, &t)) return;
    uint32_t palette[256];
    const bool indexed = ReadPalette(vram, v, heal, layout, palette);
    BlockDecode::Decode(vram, t, ViewTexa(v), indexed ? palette : nullptr, out->data());
    ScaleAlpha(out);
}

void TextureDecoder::Decode(const uint8_t* vram, const TexReadView& v, ClutHealCache* heal,
                            const VramLayout* layout, std::vector<uint32_t>* out) {
    DecodeLevel(vram, v, v.tbp, v.tbw, 1u << v.tw, 1u << v.th, heal, layout, out);
}

}
}
