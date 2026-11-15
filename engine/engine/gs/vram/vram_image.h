#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "engine/gs/vram/page_record.h"
#include "engine/gs/vram/vram_mirror.h"

namespace ps2r {
namespace gs {

class VramImage {
public:
    static constexpr uint32_t kBytes = 4u * 1024u * 1024u;
    static constexpr uint32_t kPageBytes = PageRecord::kPageBytes;
    static constexpr uint32_t kPageCount = PageRecord::kPageCount;

    enum class Reason : uint8_t { Decode, Serve, Refresh, PreDraw, Scanout, Destroy, Save };

    struct Span {
        uint32_t start;
        uint32_t size;
    };
    VramImage();

    void WriteStream(uint32_t byte_off, const uint8_t* src, size_t len);

    void NoteWriteNoBytes(uint32_t byte_off, size_t len);

    uint64_t NoteStreamWrite(Span dest);

    void SettleWrites();

    void CommitDeposits();

    void SetMirror(VramMirror* mirror) { mirror_ = mirror; }
    VramMirror* GetMirror() const { return mirror_; }

    static uint32_t PageOf(uint32_t byte_addr) { return (byte_addr % kBytes) / kPageBytes; }

    static void PagesOf(Span span, uint32_t* first, uint32_t* count);

    const uint8_t* Bytes() const { return bytes_.data(); }
    uint8_t* MutableBytes() { return bytes_.data(); }
    const PageRecord& Page(uint32_t page) const { return pages_[page % kPageCount]; }
    PageRecord& MutablePage(uint32_t page) { return pages_[page % kPageCount]; }
    uint64_t UploadEpoch() const { return upload_seq_; }
    uint32_t PendingPages() const { return pending_; }
    bool HasShadow() const { return static_cast<bool>(shadow_); }

private:
    void StampPages(Span span, uint64_t epoch);

    std::vector<uint8_t> bytes_;
    std::vector<PageRecord> pages_;
    std::unique_ptr<uint8_t[]> shadow_;
    uint64_t upload_seq_ = 0;
    uint32_t pending_ = 0;
    uint32_t settle_lo_ = 0xFFFFFFFFu;
    uint32_t settle_hi_ = 0;
    std::vector<uint64_t> settle_pages_;
    VramMirror* mirror_ = nullptr;
};

}
}
