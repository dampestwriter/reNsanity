#include "games/slus_20909_crash_twinsanity/hle/audio/audio.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include "engine/sound/audio_mixer.h"

namespace title {
namespace {
bool ValidSample(const std::vector<uint8_t>& bytes) {
    return !bytes.empty() && bytes.size() % VagDecoder::kBlockBytes == 0 && bytes.size() <= UINT32_MAX;
}
}

void Audio::ResetEffects() {
    const auto effects = state_.effects;
    state_.effects = {};
    for (uint32_t core = 0; core < 2; ++core) state_.effects.cores[core].generation = effects.cores[core].generation + 1;
}
int32_t Audio::SetEffect(uint32_t core, int32_t mode, uint16_t depth_left, uint16_t depth_right, uint16_t delay,
                         uint16_t feedback) {
    if (core >= 2 || mode < 0 || mode >= ps2r::sound::spu2::kEffectModes) return -1;
    if (mode == 0) return ClearEffect(core);

    auto& c = state_.effects.cores[core];
    c.mode = mode;
    c.delay = delay;
    c.feedback = feedback;
    c.volume_left = depth_left;
    c.volume_right = depth_right;
    c.enabled = true;
    ++c.generation;
    return 0;
}
int32_t Audio::ClearEffect(uint32_t core) {
    if (core >= 2) return -1;
    auto& c = state_.effects.cores[core];
    const uint32_t generation = c.generation + 1;
    c = {};
    c.generation = generation;
    return 0;
}
int32_t Audio::SetEffectVolume(uint32_t core, uint16_t left, uint16_t right) {
    if (core >= 2) return -1;
    state_.effects.cores[core].volume_left = left;
    state_.effects.cores[core].volume_right = right;
    return 0;
}

int32_t Audio::SetEffectSend(uint32_t voice, bool on) {
    if (voice >= kVoices) return -1;
    const uint32_t core = voice / 24, bit = 1u << (voice % 24);
    auto& shadow = state_.effects.shadow[core];
    shadow = on ? shadow | bit : shadow & ~bit;
    for (uint32_t v = 0; v < 24; ++v) state_.effects.send[core * 24 + v] = (shadow >> v) & 1;
    return 0;
}
void Audio::ReserveSounds(uint16_t count) {

    if (count == 0 || state_.sample_limit != 0) return;
    state_.sample_limit = count;
}
Audio::LoadTicket Audio::BeginSoundLoad(uint32_t id) {
    if (id == UINT32_MAX || load_sequence_ == UINT64_MAX ||
        (!state_.samples.count(id) && !pending_loads_.count(id) &&
         state_.samples.size() + pending_loads_.size() >= state_.sample_limit)) return {};
    const auto sequence = ++load_sequence_;
    pending_loads_[id] = sequence;
    state_.samples.erase(id);
    return {id, load_generation_, sequence};
}
bool Audio::Current(LoadTicket ticket) const {
    return ticket && ticket.generation == load_generation_;
}
bool Audio::CompleteSoundLoad(LoadTicket ticket, std::vector<uint8_t> bytes) {
    const auto pending = pending_loads_.find(ticket.sound);
    if (!Current(ticket) || pending == pending_loads_.end() || pending->second != ticket.sequence ||
        !ValidSample(bytes)) return false;
    Sample sample;
    sample.generation = state_.generation;
    sample.load_sequence = ticket.sequence;
    sample.bytes = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
    state_.samples[ticket.sound] = std::move(sample);
    pending_loads_.erase(pending);
    return true;
}
bool Audio::StoreSound(uint32_t id, std::vector<uint8_t> bytes) {
    if (!ValidSample(bytes)) return false;
    const auto ticket = BeginSoundLoad(id);
    return ticket && CompleteSoundLoad(ticket, std::move(bytes));
}
bool Audio::SoundBankLoaded(uint32_t id) {
    const auto it = state_.samples.find(id);
    if (it == state_.samples.end() || it->second.generation != state_.generation) return false;
    it->second.ready = true;
    return true;
}
bool Audio::ReleaseSound(uint32_t id) {

    const bool pending = pending_loads_.erase(id) != 0;
    return state_.samples.erase(id) != 0 || pending;
}
uint16_t Audio::GroupScale(uint16_t volume, uint16_t group) {
    const bool inverted = volume > 0x3fff;
    const uint32_t magnitude = inverted ? 0x7fff - volume : volume;
    const uint32_t scaled = magnitude * group / 0x1000;
    return static_cast<uint16_t>(inverted ? 0x7fff - scaled : scaled);
}
void Audio::ApplyVolumes(Voice& voice, bool smooth) {
    const uint32_t group = voice.group;
    const auto left = group ? GroupScale(voice.left, state_.group_left[group - 1]) : voice.left;
    const auto right = group ? GroupScale(voice.right, state_.group_right[group - 1]) : voice.right;
    if (smooth) voice.player.RampVolumes(left, right);
    else voice.player.SetVolumes(left, right);
}
int32_t Audio::PlaySound(uint32_t id, uint32_t voice_and_group, int16_t left, int16_t right,
                        uint16_t pitch, uint32_t attack, uint32_t release, uint32_t loops) {
    Voice voice;
    const auto result = PrepareSound(id, voice_and_group, left, right, pitch, attack, release, loops, voice);
    if (result) return result;
    state_.voices[voice_and_group & 0xffff] = std::move(voice);
    return 0;
}
int32_t Audio::PrepareSound(uint32_t id, uint32_t voice_and_group, int16_t left, int16_t right,
                           uint16_t pitch, uint32_t attack, uint32_t release, uint32_t loops,
                           Voice& prepared) const {
    const uint32_t index = voice_and_group & 0xffff;
    const uint32_t group = voice_and_group >> 16;
    if (index >= kVoices || group > kGroups) return -1;
    if (loops > 0xffff) return -2;
    const auto sample = state_.samples.find(id);
    if (sample == state_.samples.end() || !sample->second.ready) return -3;
    if (attack > 127 || release > 31 || left < 0 || right < 0) return -4;
    Voice voice;
    voice.sound = id;
    voice.group = group;
    voice.left = static_cast<uint16_t>(left);
    voice.right = static_cast<uint16_t>(right);
    voice.repeats = loops;
    SampleVoice::Parameters parameters;
    parameters.pitch = pitch;
    parameters.attack = static_cast<uint8_t>(attack);
    parameters.decay = 15;
    parameters.release = static_cast<uint8_t>(release);
    if (!voice.player.Start(sample->second.bytes, parameters)) return -4;
    voice.player.SetVolumes(group ? GroupScale(voice.left, state_.group_left[group - 1]) : voice.left,
                            group ? GroupScale(voice.right, state_.group_right[group - 1]) : voice.right);
    prepared = std::move(voice);
    return 0;
}
bool Audio::StartVoice(uint32_t index, Voice voice) {
    if (index >= kVoices || voice.sound == UINT32_MAX || voice.group > kGroups ||
        !voice.player.Playing()) return false;
    voice.held = false;
    state_.voices[index] = std::move(voice);
    return true;
}
bool Audio::ReserveMusicVoice(uint32_t stream, uint64_t generation, uint32_t encoded,
                             uint16_t pitch, uint16_t left, uint16_t right, uint16_t raw_left,
                             uint16_t raw_right, uint8_t attack, uint8_t release) {
    const uint32_t channel = encoded & 0xffff, group = encoded >> 16;
    if (stream >= kVoices || generation == 0 || channel >= kVoices || group > kGroups ||
        ((left | right | raw_left | raw_right) & 0x8000) || attack > 127 || release > 31) return false;
    Voice voice;
    voice.group = group; voice.left = raw_left; voice.right = raw_right;
    voice.music_stream = stream; voice.music_generation = generation;
    voice.player.SetPitch(pitch);
    voice.player.SetVolumes(left, right);
    voice.player.Params().attack = attack;
    voice.player.Params().release = release;
    state_.voices[channel] = std::move(voice);
    return true;
}
bool Audio::StartMusicVoice(uint32_t stream, uint64_t generation, uint32_t channel,
                           SampleVoice::Samples samples) {
    if (channel >= kVoices) return false;
    auto& voice = state_.voices[channel];
    if (voice.music_stream != stream || voice.music_generation != generation || voice.music_started) return false;
    SampleVoice player;
    auto parameters = voice.player.Params();
    parameters.decay = 15;
    if (!player.Start(std::move(samples), parameters)) return false;

    voice.player = std::move(player);
    voice.music_started = true;
    voice.held = false;
    return true;
}
void Audio::StopMusicVoice(uint32_t stream, uint64_t generation, uint32_t channel) {
    if (channel >= kVoices) return;
    auto& voice = state_.voices[channel];
    if (voice.music_stream != stream || voice.music_generation != generation) return;
    voice.player.Stop();
    voice.music_started = true;
}
bool Audio::IsMusicVoice(uint32_t channel) const {
    return channel < kVoices && state_.voices[channel].music_stream != UINT32_MAX;
}
int32_t Audio::SetVoiceVolume(uint32_t index, int16_t left, int16_t right) {
    if (index >= kVoices) return -1;
    if (left < 0 || right < 0) return -2;
    auto& voice = state_.voices[index];
    voice.left = static_cast<uint16_t>(left);
    voice.right = static_cast<uint16_t>(right);
    ApplyVolumes(voice, true);
    return 0;
}
int32_t Audio::SetVoicePitch(uint32_t index, uint16_t pitch) {
    if (index >= kVoices) return -1;
    state_.voices[index].player.SetPitch(pitch);
    return 0;
}
int32_t Audio::ReleaseVoice(uint32_t index, uint32_t rate) {
    const auto result = SetVoiceRelease(index, rate);
    return result ? result : KeyOff(index);
}
int32_t Audio::SetVoiceRelease(uint32_t index, uint32_t rate) {
    if (rate >= 32) return -1;
    if (index >= kVoices) return -2;
    state_.voices[index].player.SetRelease(static_cast<uint8_t>(rate));
    return 0;
}
int32_t Audio::KeyOff(uint32_t index) {
    if (index >= kVoices) return -1;
    auto& player = state_.voices[index].player;
    player.Release(player.Params().release);
    return 0;
}
int32_t Audio::IsVoiceFree(uint32_t index) const {
    if (index >= kVoices) return -1;
    const auto& voice = state_.voices[index];
    if (IsMusicVoice(index) && !voice.music_started) return 0;
    return !voice.player.Playing();
}
bool Audio::OwnsVoice(uint32_t index) const {
    return index < kVoices && (state_.voices[index].sound != UINT32_MAX || IsMusicVoice(index));
}
bool Audio::StepVoice(uint32_t index, float& left, float& right) {
    if (!OwnsVoice(index)) {
        left = right = 0;
        return index < kVoices && StepGhost(index, left, right);
    }
    left = right = 0;
    auto& voice = state_.voices[index];
    ghost_[index].fade = 0;
    if (!voice.held) {
        voice.player.Step(left, right);
        if (voice.repeats && voice.player.Ends() >= voice.repeats) voice.player.Stop();
    } else voice.player.StepPaused();
    ApplyFade(voice, left, right);
    return true;
}
uint32_t Audio::SampleGroup(uint32_t voice) const {
    if (voice >= kVoices) return 0;
    const auto& v = state_.voices[voice];
    return v.music_stream == UINT32_MAX && v.player.Playing() ? v.group : 0;
}
int Audio::StepChannel(uint32_t index, int frames, float* left, float* right, bool* owned) {

    const bool owner = OwnsVoice(index);
    std::fill_n(owned, frames, owner);
    if (!owner) {
        if (index >= kVoices || ghost_[index].fade <= 0) return 0;
        std::fill_n(owned, frames, true);
        for (int frame = 0; frame < frames; ++frame) {
            left[frame] = right[frame] = 0;
            StepGhost(index, left[frame], right[frame]);
        }
        return frames;
    }
    auto& voice = state_.voices[index];
    ghost_[index].fade = 0;

    if (frames > 0 && !voice.player.Playing()) {
        if (!voice.held && voice.repeats && voice.player.Ends() >= voice.repeats) voice.player.Stop();
        std::fill_n(left, frames, 0.0f);
        std::fill_n(right, frames, 0.0f);

        if (voice.was_playing && !voice.held && voice.fade == 0) voice.fade = int16_t(kPauseFade);
        voice.was_playing = false;
        if (voice.fade > 0)
            for (int frame = 0; frame < frames; ++frame) ApplyFade(voice, left[frame], right[frame]);
        else voice.fade = 0;
        return frames;
    }
    int first = 0;
    for (int frame = first; frame < frames; ++frame) {
        left[frame] = right[frame] = 0;
        const bool was = voice.player.Playing();
        if (!voice.held) {
            voice.player.Step(left[frame], right[frame]);
            if (voice.repeats && voice.player.Ends() >= voice.repeats) voice.player.Stop();
        } else voice.player.StepPaused();
        ApplyFade(voice, left[frame], right[frame]);
        if (was && !voice.player.Playing() && !voice.held && voice.fade == 0) voice.fade = int16_t(kPauseFade);
    }
    voice.was_playing = voice.player.Playing();
    return frames;
}

void Audio::ApplyFade(Voice& voice, float& left, float& right) {
    if (voice.fade > 0) {
        left = voice.last_left * float(voice.fade) / float(kPauseFade + 1);
        right = voice.last_right * float(voice.fade) / float(kPauseFade + 1);
        --voice.fade;
    } else if (voice.fade < 0) {
        const float g = 1.0f - float(-voice.fade) / float(kPauseFade + 1);
        left *= g;
        right *= g;
        ++voice.fade;
        voice.last_left = left / g;
        voice.last_right = right / g;
    } else if (!voice.held) {
        voice.last_left = left;
        voice.last_right = right;
    }
}
void Audio::BeginMixTick() { ++state_.ticks; }
void Audio::AdvanceControls() {
    for (auto& voice : state_.voices) if (!voice.held) voice.player.AdvanceControls();
}
void Audio::Ghostify(uint32_t index) {
    const auto& voice = state_.voices[index];
    if (voice.was_playing && !voice.held) ghost_[index] = {voice.last_left, voice.last_right, int16_t(kPauseFade)};
}
bool Audio::StepGhost(uint32_t index, float& left, float& right) {
    auto& g = ghost_[index];
    if (g.fade <= 0) return false;
    left = g.left * float(g.fade) / float(kPauseFade + 1);
    right = g.right * float(g.fade) / float(kPauseFade + 1);
    --g.fade;
    return true;
}
void Audio::HandOffVoice(uint32_t index) {
    if (index < kVoices) { Ghostify(index); state_.voices[index] = {}; }
}
void Audio::LatchEnded(uint32_t index) {
    if (index < kVoices) state_.voices[index].player.Stop();
}
void Audio::StopVoices() {
    for (uint32_t index = 0; index < kVoices; ++index) Ghostify(index);
    state_.voices = {};
    state_.paused = false;
}
int32_t Audio::StoreGroupVolume(uint32_t encoded, uint32_t left, uint32_t right) {
    const auto group = encoded >> 16;
    if (group == 0 || group > kGroups) return -1;
    if (left > 0x1000) return -2;
    if (right > 0x1000) return -3;
    state_.group_left[group - 1] = static_cast<uint16_t>(left);
    state_.group_right[group - 1] = static_cast<uint16_t>(right);
    return 0;
}
int32_t Audio::SetGroupVolume(uint32_t encoded, uint32_t left, uint32_t right) {
    const auto result = StoreGroupVolume(encoded, left, right);
    if (result) return result;
    const auto group = encoded >> 16;
    for (auto& voice : state_.voices) if (voice.group == group) ApplyVolumes(voice);
    return 0;
}
void Audio::PauseAll() {

    state_.paused = true;
    for (auto& voice : state_.voices) if (voice.player.Playing()) { voice.held = true; voice.fade = int16_t(kPauseFade); }
}
void Audio::ResumeAll() {

    for (auto& voice : state_.voices) {
        const auto parameters = voice.player.Params();
        voice.player.SetVolumes(parameters.left, parameters.right);
        if (voice.held && voice.player.Playing()) voice.fade = int16_t(-kPauseFade);
        else voice.fade = 0;
        voice.held = false;
    }
    state_.paused = false;
}
void Audio::LendToMovie() { state_.movie_owned = true; }
void Audio::ReclaimFromMovie() { state_.movie_owned = false; }
const Audio::State& Audio::Inspect() const { return state_; }
int32_t Audio::PitchOfRate(int32_t rate) {
    return static_cast<int32_t>(static_cast<int64_t>(rate) * 0x1000 / 48000);
}
}
