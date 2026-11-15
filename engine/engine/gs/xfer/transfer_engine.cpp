#include "engine/gs/xfer/transfer_engine.h"

#include <algorithm>
#include <cstring>

#include "engine/gs/format/psm_layout.h"
#include "engine/gs/hle/ir/gsdl_format.h"

namespace ps2r {
namespace gs {

namespace {
constexpr uint32_t kVram = VramImage::kBytes;
constexpr uint32_t kBlock = PsmLayout::kBlockBytes;
}

uint32_t TransferEngine::PixelSize(uint32_t psm) {
    switch (psm) {
        case 0x00: case 0x01: case 0x30: case 0x31: return 4;
        case 0x02: case 0x0A: case 0x32: case 0x3A: return 2;
        case 0x13: case 0x14: return 1;
        default: return 4;
    }
}

uint32_t TransferEngine::PixelsPerWord(uint32_t psm) {
    switch (psm) {
        case 0x00: case 0x01: return 2;
        case 0x02: case 0x0A: return 4;
        case 0x13: return 8;
        case 0x14: return 16;
        default: return 2;
    }
}

bool TransferEngine::WriteRegister(uint8_t addr, uint64_t v) {
    switch (addr) {
        case 0x50:
            stage_.sbp = static_cast<uint32_t>(v & 0x3FFF) * kBlock;
            stage_.sbw = static_cast<uint32_t>((v >> 16) & 0x3F) * 64;
            stage_.spsm = static_cast<uint32_t>((v >> 24) & 0x3F);
            stage_.dbp = static_cast<uint32_t>((v >> 32) & 0x3FFF) * kBlock;
            stage_.dbw = static_cast<uint32_t>((v >> 48) & 0x3F) * 64;
            stage_.dpsm = static_cast<uint32_t>((v >> 56) & 0x3F);
            return true;
        case 0x51:
            stage_.ssax = static_cast<uint16_t>(v & 0x7FF);
            stage_.ssay = static_cast<uint16_t>((v >> 16) & 0x7FF);
            stage_.dsax = static_cast<uint16_t>((v >> 32) & 0x7FF);
            stage_.dsay = static_cast<uint16_t>((v >> 48) & 0x7FF);
            stage_.dir = static_cast<uint8_t>((v >> 59) & 3);
            return true;
        case 0x52:
            stage_.rrw = static_cast<uint16_t>(v & 0xFFF);
            stage_.rrh = static_cast<uint16_t>((v >> 32) & 0xFFF);
            return true;
        case 0x53:
            SetTrxDir(v);
            return true;
        case 0x54:
            WriteHwreg(v);
            return true;
        default:
            return false;
    }
}

void TransferEngine::PushJob(JobKind kind, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4,
                             const std::vector<uint8_t>* payload) {
    if (!sink_) return;
    JobRec j{};
    j.type = static_cast<uint32_t>(kind);
    j.seq = seq_ ? (*seq_)++ : 0;
    j.src_frame = src_frame_ ? *src_frame_ : 0;
    j.arg1 = a1;
    j.arg2 = a2;
    j.arg3 = a3;
    j.arg4 = a4;
    j.vram_epoch = vram_->UploadEpoch();
    if (payload && !payload->empty()) {
        j.payload_bytes = static_cast<uint32_t>(payload->size());
        j.payload_hash = GsdlFormat::Fnv1aSampled(payload->data(), payload->size());
    }
    sink_->OnJob(j);
}

void TransferEngine::StampDest() {
    if (cur_.xdir != 0 && cur_.xdir != 2) return;
    if (xfer_.rrw == 0 || xfer_.rrh == 0) return;
    const uint32_t dest_size = xfer_.rrw * xfer_.rrh * PixelSize(xfer_.dpsm);
    vram_->NoteStreamWrite(VramImage::Span{xfer_.dbp, dest_size});
}

void TransferEngine::ArmHostToLocal() {
    FlushStage();
    cur_.cur_x = xfer_.dsax;
    cur_.cur_y = xfer_.dsay;
    cur_.active = true;
    cur_.pixels_written = 0;

    Psm mode;
    const uint32_t cw = PsmLayout::FromField(xfer_.dpsm, &mode) ? PsmLayout::Describe(mode).column_w : 8u;
    stage_col_shift_ = 0;
    while ((1u << stage_col_shift_) < cw) ++stage_col_shift_;
    stage_col0_ = xfer_.dsax >> stage_col_shift_;
    const size_t slots = xfer_.rrw == 0 ? 0 : (((xfer_.dsax + xfer_.rrw - 1u) >> stage_col_shift_) - stage_col0_ + 1u);
    if (stage_cols_.size() < slots) stage_cols_.resize(slots);
    for (size_t i = 0; i < slots; ++i) {
        stage_cols_[i].base = 0xFFFFFFFFu;
        stage_cols_[i].mask = 0;
    }
}

void TransferEngine::StageBytes(uint32_t col, uint32_t addr, const uint8_t* bytes, uint32_t size) {
    if (col >= stage_cols_.size()) {
        vram_->WriteStream(addr, bytes, size);
        return;
    }
    ColumnSlot& slot = stage_cols_[col];
    const uint32_t base = addr & ~63u;
    if (slot.base != base) {
        if (slot.mask) FlushStageSlot(col);
        slot.base = base;
    }
    const uint32_t off = addr & 63u;
    std::memcpy(slot.bytes + off, bytes, size);
    const uint64_t bits = (size == 4 ? 0xFull : size == 2 ? 0x3ull : 0x1ull) << off;
    if (!slot.mask) ++stage_live_;
    slot.mask |= bits;
    if (slot.mask == ~0ull) {
        vram_->WriteStream(slot.base, slot.bytes, 64);
        slot.mask = 0;
        slot.base = 0xFFFFFFFFu;
        --stage_live_;
    }
}

void TransferEngine::FlushStageSlot(size_t i) {
    ColumnSlot& slot = stage_cols_[i];
    uint64_t m = slot.mask;
    if (!m) return;
    uint32_t off = 0;
    while (m) {
        if (!(m & 1ull)) {
            const uint32_t skip = static_cast<uint32_t>(__builtin_ctzll(m));
            m >>= skip;
            off += skip;
        }
        const uint32_t run = (~m == 0) ? static_cast<uint32_t>(64 - off) : static_cast<uint32_t>(__builtin_ctzll(~m));
        vram_->WriteStream(slot.base + off, slot.bytes + off, run);
        off += run;
        m = (run >= 64) ? 0 : (m >> run);
    }
    slot.mask = 0;
    slot.base = 0xFFFFFFFFu;
    --stage_live_;
}

void TransferEngine::FlushStage() {
    if (!stage_live_) return;
    for (size_t i = 0; i < stage_cols_.size() && stage_live_; ++i)
        if (stage_cols_[i].mask) FlushStageSlot(i);
    stage_live_ = 0;
}

void TransferEngine::SetTrxDir(uint64_t v) {
    if (flush_pending_) flush_pending_();
    FlushStage();
    vram_->SettleWrites();
    prev_flush_ = xfer_;
    xfer_ = stage_;
    cur_.xdir = static_cast<uint8_t>(v & 3);
    StampDest();

    if (cur_.xdir == 0) {

        if (cur_.active && cur_.pixels_written > 0 && !linear_.empty()) {
            const uint32_t pw = prev_flush_.rrw > 0 ? prev_flush_.rrw : 1;
            uint32_t ph = static_cast<uint32_t>(cur_.pixels_written / pw);
            if (ph < 1) ph = 1;
            if (ph > prev_flush_.rrh) ph = prev_flush_.rrh;
            const uint32_t bpp = PixelSize(prev_flush_.dpsm);

            std::vector<uint8_t> part(static_cast<size_t>(pw) * ph * 4, 0);
            std::memcpy(part.data(), linear_.data(), std::min(part.size(), linear_.size()));
            PushJob(JobKind::InvalidateTextures, prev_flush_.dbp, pw * ph * bpp, 0, 0, nullptr);
            PushJob(JobKind::UploadVRAMTexture, prev_flush_.dbp, pw, ph, pw * ph * bpp, &part);
            last_upload_ = std::move(part);
        }
        ArmHostToLocal();
        linear_.assign(static_cast<size_t>(xfer_.rrw) * xfer_.rrh * 4, 0);
    } else if (cur_.xdir == 2) {
        LocalToLocal();
    }
}

void TransferEngine::WriteImage(const uint8_t* data, size_t bytes) {

    bool flushed = false;
    for (size_t off = 0; off + 8 <= bytes; off += 8) {

        if (!cur_.active) {
            ArmHostToLocal();
            StampDest();
        }
        uint64_t w;
        std::memcpy(&w, data + off, 8);
        if (!flushed) {
            if (cur_.active && cur_.xdir == 0 && flush_pending_) flush_pending_();
            flushed = true;
        }

        if (cur_.active && cur_.xdir == 0) {
            const size_t ppw = PixelsPerWord(xfer_.dpsm);
            const size_t expected = static_cast<size_t>(xfer_.rrw) * xfer_.rrh;
            const size_t left_words = (bytes - off) / 8;
            if (expected > cur_.pixels_written) {
                const size_t safe_words = (expected - cur_.pixels_written - 1) / ppw;
                const size_t n = std::min(left_words, safe_words);
                if (n > 0) {
                    const size_t done = WriteWordsInside(data + off, n);
                    if (done > 0) {
                        off += done * 8 - 8;
                        continue;
                    }
                }
            }
        }
        WriteHwregWord(w);
    }
    FlushStage();
}

void TransferEngine::BeginRowOffsets(Psm mode, uint32_t y) {
    if (row_off_y_ == y && row_off_psm_ == mode) return;
    const PsmDescriptor& d = PsmLayout::Describe(mode);
    const uint32_t by = (y % d.page_h) % d.block_h;
    const uint32_t column = by / d.column_h;
    const uint32_t per_column = PsmLayout::ElementsPerColumn(mode);
    for (uint32_t bx = 0; bx < d.block_w && bx < 32; ++bx) {
        const uint32_t e = PsmLayout::ElementInBlock(mode, bx, by) - column * per_column;
        row_off_[bx] = static_cast<uint8_t>(e * d.storage_bits / 8u);
    }
    row_off_y_ = y;
    row_off_psm_ = mode;
}

size_t TransferEngine::WriteWordsInside(const uint8_t* data, size_t nwords) {
    const uint32_t psm = xfer_.dpsm;
    Psm mode;
    if (!PsmLayout::FromField(psm, &mode)) return 0;
    uint32_t size;
    switch (psm) {
        case 0x00: case 0x01: size = 4; break;
        case 0x02: case 0x0A: size = 2; break;
        case 0x13: size = 1; break;
        default: return 0;
    }
    const uint32_t ppw = PixelsPerWord(psm);
    const uint32_t bits = 64 / ppw;
    const uint64_t pix_mask = (bits == 64) ? ~0ull : ((1ull << bits) - 1);
    const uint32_t row_end = xfer_.dsax + xfer_.rrw;
    const uint32_t cw = 1u << stage_col_shift_;
    const uint32_t bp = xfer_.dbp / kBlock, bw = xfer_.dbw / 64;
    const size_t total = nwords * ppw;
    size_t k = 0;
    uint64_t word = 0;
    while (k < total) {
        if (cur_.cur_x >= row_end) {
            cur_.cur_x = xfer_.dsax;
            cur_.cur_y++;
        }
        PsmLayout::RowAddresser& row = row_addr_;
        if (!row_addr_valid_ || row.psm != mode || row.y != cur_.cur_y || row.bp != bp || row.bw != bw) {
            row.Begin(mode, cur_.cur_y, bp, bw);
            row_addr_valid_ = true;
        }
        BeginRowOffsets(mode, cur_.cur_y);

        uint32_t n = std::min(cw - (cur_.cur_x & (cw - 1)), row_end - cur_.cur_x);
        if (n > total - k) n = static_cast<uint32_t>(total - k);
        const uint32_t addr0 = row.ByteAddress(cur_.cur_x);
        const uint32_t base = addr0 & ~63u;
        const uint32_t col = (cur_.cur_x >> stage_col_shift_) - stage_col0_;
        ColumnSlot& slot = stage_cols_[col];
        if (slot.base != base) {
            if (slot.mask) FlushStageSlot(col);
            slot.base = base;
        }
        if (!slot.mask) ++stage_live_;
        const uint32_t bx0 = cur_.cur_x & (cw - 1);
        uint8_t* lin = linear_.data() + static_cast<size_t>((cur_.cur_y - xfer_.dsay) * xfer_.rrw + (cur_.cur_x - xfer_.dsax)) * 4;
        const bool lin_ok = linear_.size() >= static_cast<size_t>((cur_.cur_y - xfer_.dsay) * xfer_.rrw + (cur_.cur_x - xfer_.dsax) + n) * 4;
        uint64_t seg_mask = 0;
        for (uint32_t i = 0; i < n; ++i, ++k) {
            if ((k % ppw) == 0) std::memcpy(&word, data + (k / ppw) * 8, 8);
            uint32_t px = static_cast<uint32_t>((word >> ((k % ppw) * bits)) & pix_mask);
            const uint32_t off = row_off_[bx0 + i];
            uint8_t* dst = slot.bytes + off;
            uint8_t* l = lin + static_cast<size_t>(i) * 4;
            switch (psm) {
                case 0x00: {
                    dst[0] = px & 0xFF; dst[1] = (px >> 8) & 0xFF; dst[2] = (px >> 16) & 0xFF; dst[3] = (px >> 24) & 0xFF;
                    seg_mask |= 0xFull << off;
                    if (lin_ok) {
                        const uint32_t a = (px >> 24) & 0xFF;
                        l[0] = px & 0xFF; l[1] = (px >> 8) & 0xFF; l[2] = (px >> 16) & 0xFF;
                        l[3] = static_cast<uint8_t>(a * 2 > 255 ? 255 : a * 2);
                    }
                    break;
                }
                case 0x01:
                    px &= 0xFFFFFF;
                    dst[0] = px & 0xFF; dst[1] = (px >> 8) & 0xFF; dst[2] = (px >> 16) & 0xFF; dst[3] = 0x80;
                    seg_mask |= 0xFull << off;
                    if (lin_ok) { l[0] = px & 0xFF; l[1] = (px >> 8) & 0xFF; l[2] = (px >> 16) & 0xFF; l[3] = 255; }
                    break;
                case 0x02: case 0x0A:
                    dst[0] = px & 0xFF; dst[1] = (px >> 8) & 0xFF;
                    seg_mask |= 0x3ull << off;
                    if (lin_ok) {
                        l[0] = static_cast<uint8_t>((px & 0x1F) << 3);
                        l[1] = static_cast<uint8_t>(((px >> 5) & 0x1F) << 3);
                        l[2] = static_cast<uint8_t>(((px >> 10) & 0x1F) << 3);
                        l[3] = (px & 0x8000) ? 255 : 0;
                    }
                    break;
                default:
                    dst[0] = px & 0xFF;
                    seg_mask |= 0x1ull << off;
                    if (lin_ok) { l[0] = px & 0xFF; l[1] = 0; l[2] = 0; l[3] = 255; }
                    break;
            }
        }
        slot.mask |= seg_mask;
        if (slot.mask == ~0ull) {
            vram_->WriteStream(slot.base, slot.bytes, 64);
            slot.mask = 0;
            slot.base = 0xFFFFFFFFu;
            --stage_live_;
        }
        cur_.cur_x += n;
        cur_.pixels_written += n;
        (void)size;
    }
    return nwords;
}

void TransferEngine::StorePixel(uint32_t addr, uint32_t px, uint32_t lin_index) {
    const uint32_t psm = xfer_.dpsm;
    const uint32_t col = (cur_.cur_x >> stage_col_shift_) - stage_col0_;
    uint8_t buf[4];
    switch (psm) {
        case 0x00:
            if (addr + 4 <= kVram) {
                buf[0] = px & 0xFF; buf[1] = (px >> 8) & 0xFF; buf[2] = (px >> 16) & 0xFF; buf[3] = (px >> 24) & 0xFF;
                StageBytes(col, addr, buf, 4);
            }
            break;
        case 0x01:
            if (addr + 4 <= kVram) {
                buf[0] = px & 0xFF; buf[1] = (px >> 8) & 0xFF; buf[2] = (px >> 16) & 0xFF; buf[3] = 0x80;
                StageBytes(col, addr, buf, 4);
            }
            break;
        case 0x02: case 0x0A:
            if (addr + 2 <= kVram) {
                buf[0] = px & 0xFF; buf[1] = (px >> 8) & 0xFF;
                StageBytes(col, addr, buf, 2);
            }
            break;
        case 0x13:
            buf[0] = px & 0xFF;
            StageBytes(col, addr, buf, 1);
            break;
        default:

            if (addr < kVram) vram_->NoteWriteNoBytes(addr, PixelSize(psm));
            break;
    }
    if (static_cast<size_t>(lin_index) + 4 <= linear_.size()) {
        uint8_t* l = linear_.data() + lin_index;
        switch (psm) {
            case 0x00: {
                const uint32_t a = (px >> 24) & 0xFF;
                l[0] = px & 0xFF; l[1] = (px >> 8) & 0xFF; l[2] = (px >> 16) & 0xFF;
                l[3] = static_cast<uint8_t>(a * 2 > 255 ? 255 : a * 2);
                break;
            }
            case 0x01:
                l[0] = px & 0xFF; l[1] = (px >> 8) & 0xFF; l[2] = (px >> 16) & 0xFF; l[3] = 255;
                break;
            case 0x02: case 0x0A:
                l[0] = static_cast<uint8_t>((px & 0x1F) << 3);
                l[1] = static_cast<uint8_t>(((px >> 5) & 0x1F) << 3);
                l[2] = static_cast<uint8_t>(((px >> 10) & 0x1F) << 3);
                l[3] = (px & 0x8000) ? 255 : 0;
                break;
            default:
                l[0] = px & 0xFF; l[1] = 0; l[2] = 0; l[3] = 255;
                break;
        }
    }
}

void TransferEngine::WriteHwreg(uint64_t value) {
    if (!cur_.active || cur_.xdir != 0) return;
    if (flush_pending_) flush_pending_();
    WriteHwregWord(value);
    FlushStage();
}

void TransferEngine::WriteHwregWord(uint64_t value) {
    if (!cur_.active || cur_.xdir != 0) return;
    const uint32_t psm = xfer_.dpsm;
    const uint32_t ppw = PixelsPerWord(psm);
    const uint32_t bits = 64 / ppw;
    const uint64_t pix_mask = (bits == 64) ? ~0ull : ((1ull << bits) - 1);
    const uint32_t row_end = xfer_.dsax + xfer_.rrw;
    const uint32_t col_end = xfer_.dsay + xfer_.rrh;
    Psm mode;
    const bool known = PsmLayout::FromField(psm, &mode);
    for (uint32_t p = 0; p < ppw; ++p) {
        if (cur_.cur_x >= row_end) {
            cur_.cur_x = xfer_.dsax;
            cur_.cur_y++;
        }
        if (cur_.cur_y >= col_end) {
            if (cur_.active) FinishHostToLocal();
            return;
        }
        uint32_t addr;
        switch (psm) {
            case 0x00: case 0x01: case 0x02: case 0x0A: case 0x13: case 0x14: {

                PsmLayout::RowAddresser& row = row_addr_;
                const uint32_t bp = xfer_.dbp / kBlock, bw = xfer_.dbw / 64;
                if (!row_addr_valid_ || row.psm != mode || row.y != cur_.cur_y || row.bp != bp || row.bw != bw) {
                    row.Begin(mode, cur_.cur_y, bp, bw);
                    row_addr_valid_ = true;
                }
                addr = row.ByteAddress(cur_.cur_x);
                break;
            }
            default:
                addr = xfer_.dbp + (cur_.cur_y * xfer_.dbw + cur_.cur_x) * PixelSize(psm);
                if (addr >= kVram) addr %= kVram;
                break;
        }
        (void)known;
        uint32_t px = static_cast<uint32_t>((value >> (p * bits)) & pix_mask);
        if (psm == 0x01) px &= 0xFFFFFF;
        const uint32_t lin0 = ((cur_.cur_y - xfer_.dsay) * xfer_.rrw + (cur_.cur_x - xfer_.dsax)) * 4;
        StorePixel(addr, px, lin0);
        cur_.cur_x++;
        cur_.pixels_written++;
    }
    const size_t expected = static_cast<size_t>(xfer_.rrw) * xfer_.rrh;
    if (cur_.active && expected > 0 && cur_.pixels_written >= expected) FinishHostToLocal();
}

void TransferEngine::FinishHostToLocal() {
    FlushStage();
    vram_->SettleWrites();
    cur_.active = false;
    const uint32_t dest_size = xfer_.rrw * xfer_.rrh * PixelSize(xfer_.dpsm);
    PushJob(JobKind::InvalidateTextures, xfer_.dbp, dest_size, 0, 0, nullptr);
    if (!linear_.empty()) {
        PushJob(JobKind::UploadVRAMTexture, xfer_.dbp, xfer_.rrw, xfer_.rrh, dest_size, &linear_);
        last_upload_ = std::move(linear_);
        linear_.clear();
    }
}

void TransferEngine::LocalToLocal() {
    const uint32_t w = xfer_.rrw, h = xfer_.rrh;
    if (w == 0 || h == 0) return;

    const VramImage::Span src_span{xfer_.sbp, w * h * PixelSize(xfer_.spsm)};
    if (l2l_prepare_) l2l_prepare_(src_span, *vram_);
    auto is16 = [](uint32_t psm) { return psm == 0x02 || psm == 0x0A || psm == 0x32 || psm == 0x3A; };
    auto address = [](uint32_t psm, uint32_t x, uint32_t y, uint32_t bp, uint32_t bw) {
        Psm m = Psm::CT32;
        switch (psm) {
            case 0x02: case 0x0A: case 0x32: case 0x3A: m = Psm::CT16; break;
            default: m = Psm::CT32; break;
        }
        return PsmLayout::PixelByteAddress(m, x, y, bp / kBlock, bw / 64);
    };
    const bool s16 = is16(xfer_.spsm), d16 = is16(xfer_.dpsm);
    const bool d24 = xfer_.dpsm == 0x01 || xfer_.dpsm == 0x31;
    const uint8_t* src = vram_->Bytes();
    for (uint32_t j = 0; j < h; ++j) {
        const uint32_t sy = (xfer_.dir & 1) ? xfer_.ssay + (h - 1 - j) : xfer_.ssay + j;
        const uint32_t dy = (xfer_.dir & 1) ? xfer_.dsay + (h - 1 - j) : xfer_.dsay + j;
        for (uint32_t i = 0; i < w; ++i) {
            const uint32_t sx = (xfer_.dir & 2) ? xfer_.ssax + (w - 1 - i) : xfer_.ssax + i;
            const uint32_t dx = (xfer_.dir & 2) ? xfer_.dsax + (w - 1 - i) : xfer_.dsax + i;
            const uint32_t sa = address(xfer_.spsm, sx, sy, xfer_.sbp, xfer_.sbw);
            const uint32_t da = address(xfer_.dpsm, dx, dy, xfer_.dbp, xfer_.dbw);
            uint32_t px = 0;
            const uint32_t sn = s16 ? 2 : 4;
            for (uint32_t k = 0; k < sn; ++k) px |= static_cast<uint32_t>(src[(sa + k) % kVram]) << (8 * k);
            uint8_t buf[4] = {static_cast<uint8_t>(px & 0xFF), static_cast<uint8_t>((px >> 8) & 0xFF),
                              static_cast<uint8_t>((px >> 16) & 0xFF), static_cast<uint8_t>((px >> 24) & 0xFF)};
            vram_->WriteStream(da, buf, d16 ? 2 : (d24 ? 3 : 4));
        }
    }
    PushJob(JobKind::InvalidateTextures, xfer_.dbp, w * h * PixelSize(xfer_.dpsm), 0, 0, nullptr);
    vram_->SettleWrites();
}

}
}
