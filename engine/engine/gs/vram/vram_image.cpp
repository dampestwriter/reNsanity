#include "engine/gs/vram/vram_image.h"

#include <cstring>

namespace ps2r {
namespace gs {

VramImage::VramImage() : bytes_(kBytes, 0), pages_(kPageCount), settle_pages_(kPageCount / 64, 0) {}

void VramImage::PagesOf(Span span, uint32_t* first, uint32_t* count) {
    const uint32_t start = span.start % kBytes;
    *first = start / kPageBytes;
    uint64_t n = (static_cast<uint64_t>(start % kPageBytes) + span.size + kPageBytes - 1) / kPageBytes;
    if (span.size == 0) n = 0;
    if (n > kPageCount) n = kPageCount;
    *count = static_cast<uint32_t>(n);
}

void VramImage::StampPages(Span span, uint64_t epoch) {
    uint32_t first, count;
    PagesOf(span, &first, &count);
    for (uint32_t i = 0; i < count; ++i) {
        PageRecord& p = pages_[(first + i) % kPageCount];
        p.upload_epoch = epoch;
        p.author = PageRecord::Author::Cpu;
        p.author_fbp = 0;
        p.ClearBand();
        ++p.view_gen;
    }
}

void VramImage::WriteStream(uint32_t byte_off, const uint8_t* src, size_t len) {
    if (len == 0) return;
    if (mirror_) mirror_->OnWriteStream(byte_off, src, len);

    uint32_t addr = byte_off % kBytes;
    size_t done = 0;
    while (done < len) {
        const uint32_t page = addr / kPageBytes;
        const uint32_t in_page = addr % kPageBytes;
        size_t n = kPageBytes - in_page;
        if (n > len - done) n = len - done;
        std::memcpy(bytes_.data() + addr, src + done, n);
        PageRecord& p = pages_[page];
        const uint32_t sub0 = in_page / PageRecord::kSubPageBytes;
        const uint32_t sub1 = (in_page + static_cast<uint32_t>(n) - 1) / PageRecord::kSubPageBytes;
        const uint32_t nsub = sub1 - sub0 + 1;
        const uint64_t bits = nsub >= 64 ? ~0ull : (((1ull << nsub) - 1) << sub0);
        p.write_bits |= bits;
        p.author = PageRecord::Author::Cpu;
        ++p.view_gen;
        settle_pages_[page / 64] |= 1ull << (page % 64);
        if (addr < settle_lo_) settle_lo_ = addr;
        const uint32_t last = addr + static_cast<uint32_t>(n) - 1;
        if (last > settle_hi_) settle_hi_ = last;
        done += n;
        addr = (addr + static_cast<uint32_t>(n)) % kBytes;
    }
}

void VramImage::NoteWriteNoBytes(uint32_t byte_off, size_t len) {
    if (len == 0) return;
    if (mirror_) mirror_->OnNoteWriteNoBytes(byte_off, len);

    uint32_t addr = byte_off % kBytes;
    size_t done = 0;
    while (done < len) {
        const uint32_t page = addr / kPageBytes;
        const uint32_t in_page = addr % kPageBytes;
        size_t n = kPageBytes - in_page;
        if (n > len - done) n = len - done;
        PageRecord& p = pages_[page];
        const uint32_t sub0 = in_page / PageRecord::kSubPageBytes;
        const uint32_t sub1 = (in_page + static_cast<uint32_t>(n) - 1) / PageRecord::kSubPageBytes;
        const uint32_t nsub = sub1 - sub0 + 1;
        const uint64_t bits = nsub >= 64 ? ~0ull : (((1ull << nsub) - 1) << sub0);
        p.write_bits |= bits;
        p.author = PageRecord::Author::Cpu;
        settle_pages_[page / 64] |= 1ull << (page % 64);
        if (addr < settle_lo_) settle_lo_ = addr;
        const uint32_t last = addr + static_cast<uint32_t>(n) - 1;
        if (last > settle_hi_) settle_hi_ = last;
        done += n;
        addr = (addr + static_cast<uint32_t>(n)) % kBytes;
    }
}

uint64_t VramImage::NoteStreamWrite(Span dest) {
    if (dest.size == 0) return upload_seq_;
    if (mirror_) mirror_->OnNoteStreamWrite(dest.start, dest.size);
    const uint64_t epoch = ++upload_seq_;
    StampPages(dest, epoch);
    return epoch;
}

void VramImage::SettleWrites() {
    if (mirror_) mirror_->OnSettleWrites();
    if (settle_lo_ <= settle_hi_) {
        const uint64_t epoch = ++upload_seq_;
        for (uint32_t pg = 0; pg < kPageCount; ++pg) {
            if (!(settle_pages_[pg / 64] & (1ull << (pg % 64)))) continue;
            PageRecord& p = pages_[pg];
            p.upload_epoch = epoch;
            p.author = PageRecord::Author::Cpu;
            p.author_fbp = 0;
            p.ClearBand();
            p.write_bits = 0;
            ++p.view_gen;
        }
        for (auto& w : settle_pages_) w = 0;
        settle_lo_ = 0xFFFFFFFFu;
        settle_hi_ = 0;
    }
    CommitDeposits();
}

void VramImage::CommitDeposits() {
    if (pending_ == 0 || !shadow_) return;
    for (uint32_t pg = 0; pg < kPageCount; ++pg) {
        PageRecord& p = pages_[pg];
        if (p.state != PageRecord::State::Pending) continue;
        p.state = PageRecord::State::Applying;
        if (p.upload_epoch <= p.shadow_epoch) {
            std::memcpy(bytes_.data() + pg * kPageBytes, shadow_.get() + pg * kPageBytes, kPageBytes);
        }
        p.shadow_epoch = 0;
        p.state = PageRecord::State::Clean;
        ++p.view_gen;
        pending_--;
    }
}

}
}
