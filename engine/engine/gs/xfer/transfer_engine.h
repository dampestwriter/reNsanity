#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "engine/gs/hle/ir/draw_ir_sink.h"
#include "engine/gs/format/psm_layout.h"
#include "engine/gs/vram/vram_image.h"

namespace ps2r {
namespace gs {

class TransferEngine {
public:
    struct Regs {
        uint32_t sbp = 0, sbw = 0, spsm = 0;
        uint32_t dbp = 0, dbw = 0, dpsm = 0;
        uint16_t ssax = 0, ssay = 0, dsax = 0, dsay = 0;
        uint8_t dir = 0;
        uint16_t rrw = 0, rrh = 0;
    };
    struct Cursor {
        uint8_t xdir = 3;
        uint16_t cur_x = 0, cur_y = 0;
        bool active = false;
        size_t pixels_written = 0;
    };

    explicit TransferEngine(VramImage* vram) : vram_(vram) {}

    void SetSink(DrawIrSink* sink) { sink_ = sink; }

    void SetFlushHook(std::function<void()> hook) { flush_pending_ = std::move(hook); }

    using LocalToLocalPrepare = std::function<bool(VramImage::Span source, VramImage& image)>;
    void SetLocalToLocalPrepare(LocalToLocalPrepare prepare) { l2l_prepare_ = std::move(prepare); }

    bool WriteRegister(uint8_t addr, uint64_t value);

    void WriteImage(const uint8_t* data, size_t bytes);
    void WriteHwreg(uint64_t value);

    void WriteHwregWord(uint64_t value);

    static uint32_t PixelSize(uint32_t psm);
    static uint32_t PixelsPerWord(uint32_t psm);

    const Regs& Staged() const { return stage_; }
    const Regs& Active() const { return xfer_; }
    const Cursor& State() const { return cur_; }
    const std::vector<uint8_t>& LastUploadPixels() const { return last_upload_; }

private:
    void SetTrxDir(uint64_t value);
    void ArmHostToLocal();
    void StampDest();
    void StorePixel(uint32_t addr, uint32_t px, uint32_t lin_index);

    void StageBytes(uint32_t col, uint32_t addr, const uint8_t* bytes, uint32_t size);
    void FlushStageSlot(size_t slot);
    void FlushStage();

    size_t WriteWordsInside(const uint8_t* data, size_t nwords);
    void BeginRowOffsets(Psm mode, uint32_t y);
    void FinishHostToLocal();
    void LocalToLocal();
    void PushJob(JobKind kind, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4,
                 const std::vector<uint8_t>* payload);

    VramImage* vram_;
    DrawIrSink* sink_ = nullptr;
    std::function<void()> flush_pending_;
    LocalToLocalPrepare l2l_prepare_;

    PsmLayout::RowAddresser row_addr_;
    bool row_addr_valid_ = false;
    uint32_t* seq_ = nullptr;
    const uint32_t* src_frame_ = nullptr;
    Regs stage_;
    Regs xfer_;
    Regs prev_flush_;
    Cursor cur_;
    std::vector<uint8_t> linear_;
    std::vector<uint8_t> last_upload_;

    struct ColumnSlot {
        uint32_t base = 0xFFFFFFFFu;
        uint64_t mask = 0;
        uint8_t bytes[64];
    };
    std::vector<ColumnSlot> stage_cols_;
    uint32_t stage_live_ = 0;
    uint32_t stage_col_shift_ = 3;
    uint32_t stage_col0_ = 0;

    uint8_t row_off_[32] = {};
    uint32_t row_off_y_ = 0xFFFFFFFFu;
    Psm row_off_psm_ = Psm::CT32;
};

}
}
