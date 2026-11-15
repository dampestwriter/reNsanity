#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "engine/sound/spu2_dsp.h"
#include "engine/sound/vag_decoder.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/volume_ramp.h"

namespace title {

class SampleVoice {
public:
    using Samples = std::shared_ptr<const std::vector<uint8_t>>;
    enum class Phase : uint8_t { Off, Attack, Decay, Sustain, Release };
    struct Parameters {
        uint16_t pitch = 0x1000;
        uint16_t left = 0x3fff, right = 0x3fff;
        uint8_t attack = 0, decay = 0, sustain = 15, release = 0;
    };
    struct State {
        Samples samples;
        Parameters parameters;
        uint32_t block_offset = 0, loop_offset = 0, fraction = 0, sample_index = 0;
        VagDecoder::History history;
        std::array<int16_t, VagDecoder::kBlockSamples> block{};
        uint8_t flags = 0;
        std::array<int16_t, 3> past{};
        VolumeRamp left_ramp, right_ramp;
        uint64_t ends = 0;
    };

    static uint16_t Adsr1(const Parameters& p) { return uint16_t((p.attack << 8) | (p.decay << 4) | p.sustain); }
    static uint16_t Adsr2(const Parameters& p) { return uint16_t(0x1FC0 | p.release); }

    bool Start(Samples samples, Parameters parameters);
    void SetPitch(uint16_t pitch);
    void SetVolumes(uint16_t left, uint16_t right);
    void RampVolumes(uint16_t left, uint16_t right);
    void AdvanceControls();
    void SetRelease(uint8_t rate);
    void Release(uint8_t rate);
    void Stop();

    void Step(float& left, float& right);

    void StepInt(int32_t& left, int32_t& right);

    void StepPaused();
    bool Playing() const;
    uint64_t Ends() const;
    Parameters& Params() { return state_.parameters; }
    const Parameters& Params() const { return state_.parameters; }

private:
    void Decode();
    void Advance();
    State state_;
    ps2r::sound::spu2::Adsr adsr_;
    ps2r::sound::spu2::Volume left_, right_;
};

}
