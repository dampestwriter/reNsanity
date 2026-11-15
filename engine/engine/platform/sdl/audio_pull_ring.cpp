#include "engine/platform/sdl/audio_pull_ring.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace ps2r::platform::sdl {

namespace {

inline int Slot(uint64_t pos) {
    return static_cast<int>(pos % static_cast<uint64_t>(AudioPullRing::kCapacityFrames));
}

}

AudioPullRing::AudioPullRing() {
    std::memset(samples_, 0, sizeof(samples_));
}

void AudioPullRing::CopyIn(const float* src, int frames, uint64_t write_pos) {
    const int first = std::min(frames, kCapacityFrames - Slot(write_pos));
    std::memcpy(&samples_[static_cast<size_t>(Slot(write_pos)) * kChannels], src,
                static_cast<size_t>(first) * kChannels * sizeof(float));
    if (first < frames) {
        std::memcpy(&samples_[0], src + static_cast<size_t>(first) * kChannels,
                    static_cast<size_t>(frames - first) * kChannels * sizeof(float));
    }
}

void AudioPullRing::CopyOut(float* dst, int frames, uint64_t read_pos) const {
    const int first = std::min(frames, kCapacityFrames - Slot(read_pos));
    std::memcpy(dst, &samples_[static_cast<size_t>(Slot(read_pos)) * kChannels],
                static_cast<size_t>(first) * kChannels * sizeof(float));
    if (first < frames) {
        std::memcpy(dst + static_cast<size_t>(first) * kChannels, &samples_[0],
                    static_cast<size_t>(frames - first) * kChannels * sizeof(float));
    }
}

void AudioPullRing::Prime() {
    std::vector<float> silence(static_cast<size_t>(kQuantumFrames) * kChannels, 0.0f);
    for (int q = 0; q < LeadQuanta(); ++q) Push(silence.data(), kQuantumFrames);
}

void AudioPullRing::Declick() {
    held_[0] = last_out_[0];
    held_[1] = last_out_[1];
    blend_left_ = kDeclickFrames;
}

void AudioPullRing::Deliver(float* dst, int frames) {
    if (!declick_on_.load(std::memory_order_relaxed) || frames <= 0) return;
    for (int i = 0; i < frames; ++i) {
        float* f = dst + static_cast<size_t>(i) * kChannels;
        if (blend_left_ > 0) {
            const float w = static_cast<float>(blend_left_) / static_cast<float>(kDeclickFrames + 1);
            f[0] += (held_[0] - f[0]) * w;
            f[1] += (held_[1] - f[1]) * w;
            --blend_left_;
        }
    }
    last_out_[0] = dst[static_cast<size_t>(frames - 1) * kChannels];
    last_out_[1] = dst[static_cast<size_t>(frames - 1) * kChannels + 1];
}

void AudioPullRing::PadFromTail(float* dst, int frames, uint64_t write_pos) {
    const uint64_t tail_start = write_pos - static_cast<uint64_t>(kQuantumFrames);
    const bool declick = declick_on_.load(std::memory_order_relaxed);
    int done = 0;
    for (int i = 0; i < frames; ++i) {
        if (declick && pad_phase_ != 0 && pad_phase_ % static_cast<uint64_t>(kQuantumFrames) == 0) {
            Deliver(dst + static_cast<size_t>(done) * kChannels, i - done);
            done = i;
            Declick();
        }
        const uint64_t pos = tail_start + (pad_phase_ % static_cast<uint64_t>(kQuantumFrames));
        const float* src = &samples_[static_cast<size_t>(Slot(pos)) * kChannels];
        dst[static_cast<size_t>(i) * kChannels]     = src[0];
        dst[static_cast<size_t>(i) * kChannels + 1] = src[1];
        ++pad_phase_;
    }
    Deliver(dst + static_cast<size_t>(done) * kChannels, frames - done);
}

void AudioPullRing::TrimToLead(uint64_t write_pos) {
    uint64_t r = read_.load(std::memory_order_acquire);
    for (;;) {
        const uint64_t content = write_pos - r;
        if (content <= static_cast<uint64_t>(TrimFrames())) return;
        const uint64_t new_read = write_pos - static_cast<uint64_t>(LeadFrames());
        if (read_.compare_exchange_weak(r, new_read, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return;
        }
    }
}

void AudioPullRing::Push(const float* interleaved, int frames) {
    if (frames <= 0) return;
    std::lock_guard<std::mutex> lock(samples_mutex_);
    if (PadWithTail() && reprime_.exchange(false, std::memory_order_acq_rel)) {

        relatch_.store(false, std::memory_order_release);
        static const float silence[kQuantumFrames * kChannels] = {};
        for (int have = Available() + frames; have < LeadFrames(); have += kQuantumFrames)
            PushRaw(silence, std::min(kQuantumFrames, LeadFrames() - have));
        const int keep = std::min(frames, kQuantumFrames);
        std::memcpy(last_quantum_, interleaved + static_cast<size_t>(frames - keep) * kChannels,
                    static_cast<size_t>(keep) * kChannels * sizeof(float));
        last_quantum_frames_ = keep;
        float faded[kDeclickFrames * kChannels];
        const int ramp = std::min(frames, kDeclickFrames);
        for (int i = 0; i < ramp * kChannels; ++i)
            faded[i] = interleaved[i] * static_cast<float>(i / kChannels + 1) / static_cast<float>(kDeclickFrames + 1);
        PushRaw(faded, ramp);
        if (frames > ramp) PushRaw(interleaved + static_cast<size_t>(ramp) * kChannels, frames - ramp);
        TrimToLead(write_.load(std::memory_order_relaxed));
        return;
    }
    if (PadWithTail()) {

        if (relatch_.exchange(false, std::memory_order_acq_rel) && last_quantum_frames_ > 0) {
            PushRaw(last_quantum_, last_quantum_frames_);
        }
        const int keep = std::min(frames, kQuantumFrames);
        std::memcpy(last_quantum_, interleaved + static_cast<size_t>(frames - keep) * kChannels,
                    static_cast<size_t>(keep) * kChannels * sizeof(float));
        last_quantum_frames_ = keep;
    }
    PushRaw(interleaved, frames);
    if (PadWithTail()) TrimToLead(write_.load(std::memory_order_relaxed));
}

void AudioPullRing::PushRaw(const float* interleaved, int frames) {

    if (frames > kCapacityFrames) {
        const int excess = frames - kCapacityFrames;
        interleaved += static_cast<size_t>(excess) * kChannels;
        frames = kCapacityFrames;
    }

    const uint64_t w = write_.load(std::memory_order_relaxed);
    uint64_t r = read_.load(std::memory_order_acquire);
    for (;;) {
        const uint64_t used = w - r;
        const uint64_t free_frames = static_cast<uint64_t>(kCapacityFrames) - used;
        if (static_cast<uint64_t>(frames) <= free_frames) break;

        const uint64_t need     = static_cast<uint64_t>(frames) - free_frames;
        const uint64_t quantum  = static_cast<uint64_t>(kQuantumFrames);
        const uint64_t new_read = std::min(w, ((r + need + quantum - 1) / quantum) * quantum);
        if (read_.compare_exchange_weak(r, new_read, std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
            r = new_read;
            break;
        }

    }

    CopyIn(interleaved, frames, w);
    write_.store(w + static_cast<uint64_t>(frames), std::memory_order_release);
    if (w + static_cast<uint64_t>(frames) >= static_cast<uint64_t>(kQuantumFrames)) tail_valid_.store(true, std::memory_order_release);
}

void AudioPullRing::Pull(float* out, int frames) {
    if (frames <= 0) return;
    std::lock_guard<std::mutex> lock(samples_mutex_);

    int got = 0;
    uint64_t read_at = 0;
    for (;;) {
        uint64_t r = read_.load(std::memory_order_acquire);
        const uint64_t w = write_.load(std::memory_order_acquire);
        const uint64_t avail = w - r;
        got = static_cast<int>(std::min<uint64_t>(avail, static_cast<uint64_t>(frames)));
        CopyOut(out, got, r);

        if (read_.compare_exchange_strong(r, r + static_cast<uint64_t>(got),
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
            read_at = r;
            break;
        }
    }
    if (declick_on_.load(std::memory_order_relaxed) && got > 0) {

        if (padding_ || (expect_read_ != UINT64_MAX && read_at != expect_read_)) Declick();
        padding_ = false;
        Deliver(out, got);
        expect_read_ = read_at + static_cast<uint64_t>(got);
    }

    if (got < frames) {
        const int missing = frames - got;
        const uint64_t w = write_.load(std::memory_order_acquire);
        if (PadWithTail()) {
            if (tail_valid_.load(std::memory_order_acquire) && w >= static_cast<uint64_t>(kQuantumFrames)) {

                if (declick_on_.load(std::memory_order_relaxed) && !padding_) {
                    padding_ = true;
                    pad_phase_ = 0;
                    Declick();
                }
                PadFromTail(out + static_cast<size_t>(got) * kChannels, missing, w);
                if (declick_on_.load(std::memory_order_relaxed) && pad_phase_ > static_cast<uint64_t>(LeadFrames()))
                    reprime_.store(true, std::memory_order_release);
                relatch_.store(true, std::memory_order_release);
                return;
            }

            std::memset(out + static_cast<size_t>(got) * kChannels, 0,
                        static_cast<size_t>(missing) * kChannels * sizeof(float));
            return;
        }
        std::memset(out + static_cast<size_t>(got) * kChannels, 0,
                    static_cast<size_t>(missing) * kChannels * sizeof(float));
    }
}

int AudioPullRing::Available() const {
    const uint64_t r = read_.load(std::memory_order_acquire);
    const uint64_t w = write_.load(std::memory_order_acquire);
    return static_cast<int>(w - r);
}


}
