#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <vector>

#include "engine/sound/voice_source.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/sample_voice.h"

namespace title {

class Audio {
public:
    static constexpr uint32_t kVoices = 48, kGroups = 4, kFramesPerTick = 800;
    struct LoadTicket {
        uint32_t sound = UINT32_MAX;
        uint64_t generation = 0, sequence = 0;
        explicit operator bool() const { return generation != 0 && sequence != 0; }
    };
    struct Sample {
        uint64_t generation = 0, load_sequence = 0;
        SampleVoice::Samples bytes;
        bool ready = false;
    };
    struct Voice {
        SampleVoice player;
        uint32_t sound = UINT32_MAX, group = 0;
        uint16_t left = 0, right = 0;
        uint32_t repeats = 0;
        uint32_t music_stream = UINT32_MAX;
        uint64_t music_generation = 0;
        bool music_started = false;

        bool held = false;

        int16_t fade = 0;
        float last_left = 0, last_right = 0;
        bool was_playing = false;
    };
    static constexpr int kPauseFade = 240;
    struct State {
        uint64_t generation = 1, ticks = 0;
        uint32_t sample_limit = 0;
        bool paused = false, movie_owned = false;
        std::map<uint32_t, Sample> samples;
        std::array<Voice, kVoices> voices;
        std::array<uint16_t, kGroups> group_left{0x1000, 0x1000, 0x1000, 0x1000};
        std::array<uint16_t, kGroups> group_right{0x1000, 0x1000, 0x1000, 0x1000};

        ps2r::sound::EffectSettings effects;
    };
    int32_t SetEffect(uint32_t core, int32_t mode, uint16_t depth_left, uint16_t depth_right, uint16_t delay,
                      uint16_t feedback);
    int32_t ClearEffect(uint32_t core);
    int32_t SetEffectVolume(uint32_t core, uint16_t left, uint16_t right);
    int32_t SetEffectSend(uint32_t voice, bool on);
    void ResetEffects();
    void ReserveSounds(uint16_t count);

    LoadTicket BeginSoundLoad(uint32_t id);
    bool CompleteSoundLoad(LoadTicket ticket, std::vector<uint8_t> bytes);

    bool StoreSound(uint32_t id, std::vector<uint8_t> bytes);
    bool SoundBankLoaded(uint32_t id);
    bool ReleaseSound(uint32_t id);
    int32_t PlaySound(uint32_t id, uint32_t voice_and_group, int16_t left, int16_t right,
                      uint16_t pitch, uint32_t attack, uint32_t release, uint32_t loops);

    int32_t PrepareSound(uint32_t id, uint32_t voice_and_group, int16_t left, int16_t right,
                         uint16_t pitch, uint32_t attack, uint32_t release, uint32_t loops,
                         Voice& voice) const;
    bool StartVoice(uint32_t index, Voice voice);
    bool ReserveMusicVoice(uint32_t stream, uint64_t generation, uint32_t voice_and_group,
                           uint16_t pitch, uint16_t left, uint16_t right, uint16_t raw_left,
                           uint16_t raw_right, uint8_t attack = 0, uint8_t release = 0);
    bool StartMusicVoice(uint32_t stream, uint64_t generation, uint32_t channel,
                         SampleVoice::Samples samples);
    void StopMusicVoice(uint32_t stream, uint64_t generation, uint32_t channel);
    bool IsMusicVoice(uint32_t channel) const;
    int32_t SetVoiceVolume(uint32_t voice, int16_t left, int16_t right);
    int32_t SetVoicePitch(uint32_t voice, uint16_t pitch);
    int32_t SetVoiceRelease(uint32_t voice, uint32_t rate);
    int32_t KeyOff(uint32_t voice);
    int32_t ReleaseVoice(uint32_t voice, uint32_t rate);
    int32_t IsVoiceFree(uint32_t voice) const;
    bool OwnsVoice(uint32_t voice) const;

    bool StepVoice(uint32_t voice, float& left, float& right);

    int StepChannel(uint32_t voice, int frames, float* left, float* right, bool* owned);
    void BeginMixTick();

    void AdvanceControls();

    void HandOffVoice(uint32_t voice);

    uint32_t SampleGroup(uint32_t voice) const;

    void LatchEnded(uint32_t voice);
    void StopVoices();
    int32_t SetGroupVolume(uint32_t group, uint32_t left, uint32_t right);
    int32_t StoreGroupVolume(uint32_t group, uint32_t left, uint32_t right);

    void PauseAll();
    void ResumeAll();
    void LendToMovie();
    void ReclaimFromMovie();

    const State& Inspect() const;

    static int32_t PitchOfRate(int32_t rate);

private:
    static uint16_t GroupScale(uint16_t volume, uint16_t group);
    static void ApplyFade(Voice& voice, float& left, float& right);
    void ApplyVolumes(Voice& voice, bool smooth = false);
    bool Current(LoadTicket ticket) const;
    State state_;

    struct Ghost { float left = 0, right = 0; int16_t fade = 0; };
    std::array<Ghost, kVoices> ghost_{};
    void Ghostify(uint32_t index);
    bool StepGhost(uint32_t index, float& left, float& right);
    uint64_t load_generation_ = 1, load_sequence_ = 0;
    std::map<uint32_t, uint64_t> pending_loads_;
};
}
