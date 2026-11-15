#include "engine/ipu/hle/fmv_audio_capture.h"

#include <cstring>

namespace ps2r::ipu {

namespace {

uint32_t ReadLe32(const uint8_t* q) {
    return static_cast<uint32_t>(q[0]) | (static_cast<uint32_t>(q[1]) << 8) |
           (static_cast<uint32_t>(q[2]) << 16) | (static_cast<uint32_t>(q[3]) << 24);
}

}

FmvAudioCapture::FmvAudioCapture(FmvPcmSink& sink) : sink_(sink) {}

void FmvAudioCapture::Disable() { disabled_ = true; }

void FmvAudioCapture::OnAudioPes(const uint8_t* p, size_t n) {
    if (disabled_ || !p || n < 4) return;

    if (p[0] != 0xFF) return;
    if (channel_ != kAnyChannel) {
        const int channel = (static_cast<int>(p[2]) << 8) | static_cast<int>(p[3]);
        if (channel != channel_) return;
    }
    p += 4;
    n -= 4;
    if (n == 0) return;

    if (header_parsed_) {
        if (body_left_ == 0) return;
        if (n > body_left_) n = body_left_;
        body_left_ -= static_cast<uint32_t>(n);
        stage_.insert(stage_.end(), p, p + n);
    } else {
        stage_.insert(stage_.end(), p, p + n);
        if (!ParseHeader()) return;
    }

    PushCompletePairs();
}

bool FmvAudioCapture::ParseHeader() {
    if (stage_.size() < 8) return false;
    const uint8_t* h = stage_.data();
    if (std::memcmp(h, "SShd", 4) != 0) {
        Disable();
        return false;
    }
    const uint32_t hdr_len = ReadLe32(h + 4);
    if (hdr_len < 16 || hdr_len > 0x100) {
        Disable();
        return false;
    }
    if (stage_.size() < static_cast<size_t>(8) + hdr_len + 8) return false;
    const uint32_t format = ReadLe32(h + 8);
    const uint32_t rate = ReadLe32(h + 12);
    const uint32_t channels = ReadLe32(h + 16);
    const uint32_t interleave = ReadLe32(h + 20);
    if (format != 1 || rate != 48000 || channels != 2 || interleave != 0x200) {
        Disable();
        return false;
    }
    if (std::memcmp(h + 8 + hdr_len, "SSbd", 4) != 0) {
        Disable();
        return false;
    }
    const uint32_t body = ReadLe32(h + 8 + hdr_len + 4);
    stage_.erase(stage_.begin(), stage_.begin() + 8 + hdr_len + 8);
    header_parsed_ = true;

    uint32_t staged = static_cast<uint32_t>(stage_.size());
    if (staged > body) {
        stage_.resize(body);
        staged = body;
    }
    body_left_ = body - staged;
    return true;
}

void FmvAudioCapture::PushCompletePairs() {
    const size_t pairs = stage_.size() / kPairBytes;
    if (pairs == 0) return;
    const uint8_t* q = stage_.data();
    for (size_t pr = 0; pr < pairs; ++pr) {
        int16_t fr[kFramesPerPair * 2];
        const uint8_t* left = q + pr * kPairBytes;
        const uint8_t* right = left + kPairBytes / 2;
        for (uint32_t i = 0; i < kFramesPerPair; ++i) {
            fr[i * 2] = static_cast<int16_t>(static_cast<uint16_t>(left[i * 2]) |
                                             (static_cast<uint16_t>(left[i * 2 + 1]) << 8));
            fr[i * 2 + 1] = static_cast<int16_t>(static_cast<uint16_t>(right[i * 2]) |
                                                 (static_cast<uint16_t>(right[i * 2 + 1]) << 8));
        }
        sink_.Push(fr, kFramesPerPair);
    }
    stage_.erase(stage_.begin(), stage_.begin() + static_cast<std::ptrdiff_t>(pairs * kPairBytes));
}

}
