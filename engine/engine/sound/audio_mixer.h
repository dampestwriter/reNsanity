#pragma once

#include <array>
#include <cstdint>

#include "engine/sound/mix_source.h"
#include "engine/sound/peak_limiter.h"
#include "engine/sound/spu2_dsp.h"

namespace ps2r::sound {

class PcmSource;
class VoiceSource;

class AudioMixer final : public MixSource {
public:
    static constexpr uint32_t kGroups = 2, kChannelsPerGroup = 24;
    struct State {
        float master = 1;
        std::array<float, kGroups> left{1, 1}, right{1, 1};
    };
    explicit AudioMixer(VoiceSource& voices) : voices_(voices) {}

    void SetMovieSource(PcmSource* source) { movie_ = source; }
    const PcmSource* MovieSource() const { return movie_; }
    bool SetGroup(uint32_t group, float left, float right);
    const State& Inspect() const { return state_; }
    static bool Validate(const State& state);
    bool Restore(const State& state);
    void MixInto(float* stereo, int frames) override;

private:
    void ProgramEffects();
    VoiceSource& voices_;
    std::array<spu2::Reverb, kGroups> reverb_;
    std::array<uint32_t, kGroups> reverb_generation_{~0u, ~0u};
    PcmSource* movie_ = nullptr;
    State state_;

    float sfx_gain_ = 1.0f;
    PeakLimiter limiter_;
};
}
