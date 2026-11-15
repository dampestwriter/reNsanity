#pragma once

#include <cstdio>
#include <utility>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "engine/sound/audio_mixer.h"
#include "engine/sound/voice_source.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/audio.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/music_asset.h"

namespace ps2r::platform::sdl { class AudioPullRing; class SdlAudioSink; }

namespace te::audio {

class AudioService final : public ps2r::sound::VoiceSource {
public:
    static constexpr uint32_t kVoices = 48;
    static constexpr uint32_t kStreams = 48;
    static constexpr uint32_t kSampleRate = 48000;

    AudioService();
    ~AudioService() override;

    void Configure(bool open_device);

    int32_t InitialiseRemote();
    void CloseDevice();

    void SetBoundaryHz(uint32_t hz);
    uint32_t BoundaryHz() const { return hz_; }

    void OnBoundary();

    void MixTick(std::vector<float>& out);

    enum class ClockMode : uint8_t { Tick, Field, Device };
    ClockMode Mode() const { return mode_; }

    void Reset();
    void PauseAll();
    void ResumeAll();
    int32_t LendToMovie();
    int32_t ReclaimFromMovie();
    void CompactSoundMemory();
    void SetGroupVolume(int32_t group, uint32_t left, uint32_t right);
    int32_t SetVoiceVolume(int32_t voice, int16_t left, int16_t right);
    int32_t SetVoicePitch(int32_t voice, uint16_t pitch);
    void SetVoiceReverb(int32_t voice, bool on);
    void ReleaseVoice(int32_t voice, uint32_t rate);
    int32_t IsVoiceFree(int32_t voice);
    void SetReverb(int32_t core, int32_t mode, uint16_t depthLeft, uint16_t depthRight, uint16_t delay, uint16_t feedback);
    void ClearReverb(int32_t core);
    void SetReverbVolume(int32_t core, int16_t left, int16_t right);
    uint32_t ReserveSoundMemory(uint32_t size);
    void SoundBankLoaded(uint16_t bank);
    void ReserveSounds(uint16_t count);
    void ReleaseSound(uint32_t sound);
    int32_t PlaySound(uint32_t sound, uint32_t voiceAndGroup, int16_t left, int16_t right, uint16_t pitch,
                      uint32_t attack, uint32_t release, uint32_t loops);
    int32_t PitchOfRate(int32_t rate);
    uint32_t ChannelBufferAddress(int32_t channel);
    void ReadMusic(int32_t file, uint32_t offset, uint32_t size);
    int32_t StreamMusic(int32_t file, int32_t channel, uint32_t voiceAndGroup, uint16_t pitch, bool once);
    void InterleaveMusic(int32_t channel, uint32_t blockSize);
    void AddMusicChannel(int32_t child, int32_t parent, uint32_t voiceAndGroup, uint32_t soundAddress);
    void SetMusicEnd(int32_t channel, uint32_t end);
    void PrepareMusic(int32_t channel);
    bool IsMusicReady(int32_t channel);
    int32_t PlayMusic(int32_t channel);
    bool IsMusicPlaying(int32_t channel);
    void StopMusic(int32_t channel);

    bool StoreSoundBank(uint32_t bank, std::vector<uint8_t> bytes);

    uint32_t SoundMemoryCursor() const { return sound_memory_cursor_; }

    void SetMovieSource(ps2r::sound::PcmSource* source) { Join(); mixer_.SetMovieSource(source); movie_source_ = source; }

    bool MovieUnheardFrames(int64_t& frames);

    void Join() const { if (async_inflight_.load(std::memory_order_acquire)) const_cast<AudioService*>(this)->FinishAsyncTick(); }

    const title::Audio& Audio() const { return audio_; }
    title::Audio& Audio() { return audio_; }
    const ps2r::sound::AudioMixer& Mixer() const { return mixer_; }
    void BeginMixTick() override;
    bool StepVoice(uint32_t channel, float& left, float& right) override;
    int StepChannel(uint32_t channel, int frames, float* left, float* right, bool* owned) override;
    uint32_t ChannelGroup(uint32_t channel) const override;
    const ps2r::sound::EffectSettings* Effects() const override;

private:
    enum class Phase : uint8_t { Idle, Reading, Prepared, Playing, Stopped, Failed };
    struct Window { uint32_t offset = 0, size = 0; bool set = false; };
    struct Stream {
        bool owned = false;
        uint32_t parent = UINT32_MAX, child = UINT32_MAX;
        int32_t file = -1;
        uint32_t voice_and_group = 0;
        uint16_t pitch = 0x1000;
        bool looping = true;
        uint32_t block = 0, end = 0, track = 0;
        uint32_t sound_address = 0;
        Window window;
        uint64_t generation = 0;
        bool key_on_disabled = false;
        bool issued = false;
        Phase phase = Phase::Idle;
        title::MusicAsset::Data data;
    };
    void EnsureDevice();
    void HookClock();
    void IssueMusicRead(uint32_t parent);
    void StartMusic(uint32_t parent);

    struct Command { uint64_t seq = 0, boundary = 0; std::function<void(title::Audio&)> apply; };

    template <class F> auto Apply(int voice, F f, bool keys = true) -> decltype(f(std::declval<title::Audio&>()));
    void Post(int voice, std::function<void(title::Audio&)> apply, bool keys = true);
    void SelectMode();
    void StartMixer();
    void StopMixer();
    void MixerLoop();
    void CommitBoundary();
    void TakeMovie(int frames);
    void AsyncLoop();
    void FinishAsyncTick();
    class BackVoices;
    class MovieFeed;
    ClockMode mode_ = ClockMode::Tick;
    bool decoupled_ = false;

    uint64_t last_fields_ = 0;
    std::vector<Command> batch_;
    uint64_t seq_ = 0;

    std::array<uint64_t, kVoices> last_seq_{};
    std::mutex mu_;
    std::vector<Command> queue_;
    std::vector<int16_t> movie_fifo_;
    std::array<float, 2> movie_gains_{};
    int movie_prime_ticks_ = 3;
    bool movie_live_ = false;
    uint64_t applied_seq_ = 0;
    std::array<bool, kVoices> back_playing_{};
    std::mutex mix_mu_;
    std::unique_ptr<title::Audio> back_;
    std::unique_ptr<BackVoices> back_voices_;
    std::unique_ptr<ps2r::sound::AudioMixer> back_mixer_;
    std::unique_ptr<MovieFeed> back_movie_;
    std::thread mixer_thread_;
    std::atomic<bool> mixer_stop_{false};
    ps2r::sound::PcmSource* movie_source_ = nullptr;
    std::unique_ptr<MovieFeed> async_movie_;
    std::thread async_thread_;
    std::mutex async_mu_;
    std::condition_variable async_cv_;
    bool async_posted_ = false, async_stop_ = false;
    std::atomic<bool> async_inflight_{false};
    void StopStream(uint32_t stream, Phase phase);
    uint32_t ParentOf(uint32_t stream) const;
    bool VoicePlaying(uint32_t voice) const;
    void RefreshMusicPhase(uint32_t parent);

    title::Audio audio_;
    ps2r::sound::AudioMixer mixer_{*this};
    std::unique_ptr<ps2r::platform::sdl::AudioPullRing> ring_;
    std::unique_ptr<ps2r::platform::sdl::SdlAudioSink> sink_;
    bool open_device_ = true, device_open_ = false;
    const void* hooked_clock_ = nullptr;
    uint32_t hz_ = 60;
    std::vector<float> tick_;
    uint32_t sound_memory_cursor_ = kSoundMemoryBase;
    std::array<Window, 256> windows_{};
    std::array<Stream, kStreams> streams_{};
    uint64_t generation_ = 0;
    static constexpr uint32_t kSoundMemoryBase = 0x5010;
};

AudioService& Service();
}
