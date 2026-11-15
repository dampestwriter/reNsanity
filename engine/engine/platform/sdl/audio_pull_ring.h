#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

namespace ps2r::platform::sdl {

class AudioPullRing {
public:
    static constexpr int kChannels       = 2;
    static constexpr int kQuantumFrames  = 800;
    static constexpr int kCapacityQuanta = 16;
    static constexpr int kCapacityFrames = kQuantumFrames * kCapacityQuanta;

    AudioPullRing();
    AudioPullRing(const AudioPullRing&) = delete;
    AudioPullRing& operator=(const AudioPullRing&) = delete;

    void Push(const float* interleaved, int frames);

    void Pull(float* out, int frames);

    static constexpr int kDefaultLeadQuanta = 6;

    static void SetLeadQuanta(int quanta) { lead_quanta_ = quanta < 1 ? 1 : quanta > kCapacityQuanta - 3 ? kCapacityQuanta - 3 : quanta; }
    static int LeadQuanta() { return lead_quanta_; }
    static int LeadFrames() { return kQuantumFrames * lead_quanta_; }
    static int TrimFrames() { return LeadFrames() + 2 * kQuantumFrames; }
    void SetPadWithTail(bool on) { pad_with_tail_.store(on, std::memory_order_release); }

    static constexpr int kDeclickFrames = 128;
    void SetDeclick(bool on) { declick_on_.store(on, std::memory_order_release); }
    bool PadWithTail() const { return pad_with_tail_.load(std::memory_order_acquire); }

    void Prime();

    int Available() const;


private:
    static inline int lead_quanta_ = kDefaultLeadQuanta;
    void CopyIn(const float* src, int frames, uint64_t write_pos);
    void CopyOut(float* dst, int frames, uint64_t read_pos) const;
    void PadFromTail(float* dst, int frames, uint64_t write_pos);
    void PushRaw(const float* interleaved, int frames);
    void TrimToLead(uint64_t write_pos);
    void Declick();
    void Deliver(float* dst, int frames);

    std::mutex            samples_mutex_;
    float                 samples_[kCapacityFrames * kChannels];
    std::atomic<uint64_t> read_{0};
    std::atomic<uint64_t> write_{0};
    std::atomic<bool>     pad_with_tail_{false};
    std::atomic<bool>     tail_valid_{false};
    std::atomic<bool>     relatch_{false};
    std::atomic<bool>     declick_on_{false};
    std::atomic<bool>     reprime_{false};

    float                 last_out_[kChannels] = {}, held_[kChannels] = {};
    int                   blend_left_ = 0;
    bool                  padding_ = false;
    uint64_t              expect_read_ = UINT64_MAX;
    uint64_t              pad_phase_ = 0;
    float                 last_quantum_[kQuantumFrames * kChannels] = {};
    int                   last_quantum_frames_ = 0;
};

}
