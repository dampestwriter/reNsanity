#include "games/slus_20909_crash_twinsanity/hle/audio/sample_voice.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace title {
namespace spu2 = ps2r::sound::spu2;

bool SampleVoice::Start(Samples samples, Parameters parameters) {
    if (!samples || samples->empty() || samples->size() % VagDecoder::kBlockBytes != 0 ||
        samples->size() > UINT32_MAX || parameters.attack > 127 ||
        parameters.decay > 15 || parameters.sustain > 15 || parameters.release > 31 ||
        (parameters.left & 0x8000) || (parameters.right & 0x8000)) return false;
    state_ = {};
    state_.samples = std::move(samples);
    state_.parameters = parameters;

    adsr_.Set(Adsr1(parameters), Adsr2(parameters));
    adsr_.KeyOn();
    left_.Write(parameters.left);
    right_.Write(parameters.right);

    Decode();
    return true;
}

void SampleVoice::SetPitch(uint16_t pitch) { state_.parameters.pitch = pitch; }
void SampleVoice::SetVolumes(uint16_t left, uint16_t right) {
    state_.parameters.left = left;
    state_.parameters.right = right;
    left_.Write(left);
    right_.Write(right);
    state_.left_ramp.Reset();
    state_.right_ramp.Reset();
}
void SampleVoice::RampVolumes(uint16_t left, uint16_t right) {
    state_.parameters.left = left;
    state_.parameters.right = right;
    state_.left_ramp.Target(left, left_.level());
    state_.right_ramp.Target(right, right_.level());
}

void SampleVoice::AdvanceControls() {
    state_.left_ramp.Control(left_);
    state_.right_ramp.Control(right_);
}
void SampleVoice::SetRelease(uint8_t rate) {
    if (rate > 31) return;
    state_.parameters.release = rate;
    adsr_.SetAdsr2(Adsr2(state_.parameters));
}
void SampleVoice::Release(uint8_t rate) {
    if (rate > 31 || !Playing()) return;
    SetRelease(rate);
    adsr_.KeyOff();
}
void SampleVoice::Stop() {
    adsr_.Mute();
    state_.samples.reset();
    state_.sample_index = state_.fraction = 0;
}
bool SampleVoice::Playing() const { return adsr_.phase() != spu2::Adsr::Off; }
uint64_t SampleVoice::Ends() const { return state_.ends; }

void SampleVoice::Decode() {
    state_.flags = VagDecoder::DecodeBlock(state_.samples->data() + state_.block_offset,
                                          state_.history, state_.block.data());
    state_.sample_index = 0;
    if (state_.flags & VagDecoder::kFlagLoopStart) state_.loop_offset = state_.block_offset;
}

void SampleVoice::StepInt(int32_t& out_left, int32_t& out_right) {
    out_left = out_right = 0;
    if (!Playing()) return;
    left_.Tick();
    right_.Tick();
    auto& s = state_;
    const int32_t sample = spu2::Interpolate(s.past[0], s.past[1], s.past[2], s.block[s.sample_index], s.fraction);
    adsr_.Tick();
    const int32_t value = spu2::ApplyVolume(sample, adsr_.level());
    out_left = spu2::ApplyVolume(value, left_.level());
    out_right = spu2::ApplyVolume(value, right_.level());
    if (!Playing()) return;
    Advance();
}
void SampleVoice::Advance() {
    auto& s = state_;
    s.fraction += std::min<uint32_t>(s.parameters.pitch, 0x3FFF);
    while (s.fraction >= 0x1000) {
        s.fraction -= 0x1000;
        s.past = {s.past[1], s.past[2], s.block[s.sample_index++]};
        if (s.sample_index < VagDecoder::kBlockSamples) continue;
        if (s.flags & VagDecoder::kFlagLoopEnd) {
            ++s.ends;
            if (!(s.flags & VagDecoder::kFlagLoopRepeat)) {
                Stop();
                return;
            }
            s.block_offset = s.loop_offset;
        } else {
            s.block_offset += VagDecoder::kBlockBytes;
            if (s.block_offset >= s.samples->size()) {
                Stop();
                return;
            }
        }
        Decode();
    }
}
void SampleVoice::Step(float& left, float& right) {
    int32_t l = 0, r = 0;
    StepInt(l, r);
    left = float(l) / 32768.0f;
    right = float(r) / 32768.0f;
}
void SampleVoice::StepPaused() {
    if (Playing()) adsr_.Tick();
}
}
