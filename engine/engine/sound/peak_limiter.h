#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ps2r::sound {

class PeakLimiter {
public:
    static constexpr int kLook = 96;
    static constexpr float kThreshold = 0.97f;
    static constexpr float kAttack = 0.08f;
    static constexpr float kRelease = 1.4e-4f;

    PeakLimiter() { std::fill(req_, req_ + kLook, 1.0f); }
    void Reset() { *this = PeakLimiter{}; }

    void Process(float* stereo, int frames) {
        for (int i = 0; i < frames; ++i) {
            float* f = stereo + size_t(i) * 2;
            const float l = f[0], r = f[1];
            const float peak = std::max(std::fabs(l), std::fabs(r));
            const float req = peak > kThreshold ? kThreshold / peak : 1.0f;

            const float oldest = req_[pos_];
            req_[pos_] = req;
            if (req <= min_) min_ = req;
            else if (oldest <= min_) {
                min_ = req_[0];
                for (int k = 1; k < kLook; ++k) min_ = std::min(min_, req_[k]);
            }
            const float out_l = delay_[pos_ * 2], out_r = delay_[pos_ * 2 + 1];
            delay_[pos_ * 2] = l;
            delay_[pos_ * 2 + 1] = r;
            pos_ = pos_ + 1 == kLook ? 0 : pos_ + 1;
            if (min_ < gain_) gain_ += (min_ - gain_) * kAttack;
            else if (min_ > gain_) gain_ = min_ - gain_ < 1e-3f ? min_ : gain_ + (min_ - gain_) * kRelease;
            f[0] = out_l * gain_;
            f[1] = out_r * gain_;
            reduction_max_ = std::min(reduction_max_, gain_);
        }
    }
    float gain() const { return gain_; }

    float TakeLowestGain() { const float g = reduction_max_; reduction_max_ = 1.0f; return g; }

private:
    float delay_[kLook * 2] = {};
    float req_[kLook];
    int pos_ = 0;
    float min_ = 1.0f, gain_ = 1.0f, reduction_max_ = 1.0f;
};

}
