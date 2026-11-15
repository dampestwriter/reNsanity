#include "audio_service.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <utility>

#include "engine/platform/host/platform.h"
#include "engine/platform/platform.h"
#include "engine/platform/sdl/audio_pull_ring.h"
#include "engine/platform/sdl/sdl_audio_sink.h"
#include "engine/sound/pcm_source.h"
#include "engine/sound/vag_decoder.h"
#include "movie_player.h"
#include "stream_service.h"
#include "te_audio.h"
#include "te_services.h"

#include <cstdio>

namespace te::audio {
namespace {
constexpr uint32_t kGroupMax = 4;
}

class AudioService::BackVoices final : public ps2r::sound::VoiceSource {
public:
    explicit BackVoices(title::Audio& a) : a_(a) {}
    void BeginMixTick() override { a_.BeginMixTick(); }
    bool StepVoice(uint32_t channel, float& left, float& right) override { return a_.StepVoice(channel, left, right); }
    int StepChannel(uint32_t channel, int frames, float* left, float* right, bool* owned) override {
        return a_.StepChannel(channel, frames, left, right, owned);
    }
    uint32_t ChannelGroup(uint32_t channel) const override { return a_.SampleGroup(channel); }
    const ps2r::sound::EffectSettings* Effects() const override { return &a_.Inspect().effects; }
private:
    title::Audio& a_;
};

class AudioService::MovieFeed final : public ps2r::sound::PcmSource {
public:
    std::array<float, 2> Gains() const override { return gains; }
    int Pop(int16_t* stereo, int frames) override {
        const int n = std::min(frames, static_cast<int>((pcm.size() - pos) / 2));
        if (n > 0) std::memcpy(stereo, pcm.data() + pos, sizeof(int16_t) * 2 * size_t(n));
        pos += size_t(n) * 2;
        return n;
    }
    std::vector<int16_t> pcm;
    size_t pos = 0;
    std::array<float, 2> gains{0.f, 0.f};
};

template <class F> auto AudioService::Apply(int voice, F f, bool keys) -> decltype(f(std::declval<title::Audio&>())) {
    if (decoupled_) Post(voice, f, keys);
    return f(audio_);
}

namespace {
std::atomic<bool> g_voice_latch{true};
}

namespace {
std::atomic<bool> g_stereo_fold{true};

int16_t FoldPhase(int16_t v) {
    const uint16_t u = static_cast<uint16_t>(v);
    return g_stereo_fold.load(std::memory_order_relaxed) && u > 0x3FFF && u <= 0x7FFF ? static_cast<int16_t>(0x7FFF - u) : v;
}
}

void AudioService::Post(int voice, std::function<void(title::Audio&)> apply, bool keys) {
    Command c;
    c.seq = ++seq_;
    c.boundary = te::Services::Get().clock().BoundaryCount();
    c.apply = std::move(apply);
    if ((keys || !g_voice_latch.load(std::memory_order_relaxed)) && voice >= 0 && voice < static_cast<int>(kVoices)) last_seq_[static_cast<size_t>(voice)] = c.seq;
    batch_.push_back(std::move(c));
}

AudioService::AudioService()
    : ring_(std::make_unique<ps2r::platform::sdl::AudioPullRing>()),
      sink_(std::make_unique<ps2r::platform::sdl::SdlAudioSink>()) {

    constexpr float gain = 16383.0f / 32767.0f;
    for (uint32_t group = 0; group < ps2r::sound::AudioMixer::kGroups; ++group) mixer_.SetGroup(group, gain, gain);

    ring_->SetPadWithTail(true);

    ring_->SetDeclick(true);
}

AudioService::~AudioService() { CloseDevice(); }

void AudioService::AsyncLoop() {
    ps2r::platform::host::PinCurrentThreadInteractive("te-audio-tick", ps2r::host::ThreadRole::AudioMixer);
    std::unique_lock<std::mutex> lock(async_mu_);
    for (;;) {
        async_cv_.wait(lock, [&] { return async_posted_ || async_stop_; });
        if (!async_posted_) return;
        lock.unlock();
        MixTick(tick_);
        if (device_open_) ring_->Push(tick_.data(), static_cast<int>(tick_.size() / 2));
        lock.lock();
        async_posted_ = false;
        async_cv_.notify_all();
    }
}

void AudioService::FinishAsyncTick() {
    {
        std::unique_lock<std::mutex> lock(async_mu_);
        if (async_posted_) {
            async_cv_.wait(lock, [&] { return !async_posted_; });
        }
    }
    async_inflight_.store(false, std::memory_order_release);
}

void AudioService::SelectMode() {
    mode_ = ClockMode::Tick;
    if (!device_open_) return;

    mode_ = ClockMode::Device;
    StartMixer();
}

void AudioService::StartMixer() {
    if (decoupled_) return;
    back_ = std::make_unique<title::Audio>(audio_);
    back_voices_ = std::make_unique<BackVoices>(*back_);
    back_mixer_ = std::make_unique<ps2r::sound::AudioMixer>(*back_voices_);
    back_mixer_->Restore(mixer_.Inspect());
    back_movie_ = std::make_unique<MovieFeed>();
    back_mixer_->SetMovieSource(back_movie_.get());
    batch_.clear();
    queue_.clear();
    seq_ = applied_seq_ = 0;
    last_seq_.fill(0);
    for (uint32_t v = 0; v < kVoices; ++v) back_playing_[v] = back_->Inspect().voices[v].player.Playing();
    decoupled_ = true;
    mixer_stop_.store(false, std::memory_order_release);
    mixer_thread_ = std::thread([this] { MixerLoop(); });
}

void AudioService::StopMixer() {
    if (!mixer_thread_.joinable()) return;
    mixer_stop_.store(true, std::memory_order_release);
    mixer_thread_.join();
}

void AudioService::TakeMovie(int frames) {
    auto& out = back_movie_->pcm;
    out.clear();
    const size_t n = static_cast<size_t>(frames);
    const size_t avail = movie_fifo_.size() / 2;
    const size_t take = std::min(n, avail);
    out.assign(movie_fifo_.begin(), movie_fifo_.begin() + static_cast<long>(take * 2));
    movie_fifo_.erase(movie_fifo_.begin(), movie_fifo_.begin() + static_cast<long>(take * 2));
}

bool AudioService::MovieUnheardFrames(int64_t& frames) {
    if (mode_ != ClockMode::Device || !ring_ || !mixer_thread_.joinable()) return false;
    constexpr int sink = 360;
    std::lock_guard<std::mutex> lock(mu_);
    frames = int64_t(movie_fifo_.size() / 2) + ring_->Available() + sink;
    return true;
}

void AudioService::MixerLoop() {
    ps2r::platform::host::PinCurrentThreadInteractive("te-audio-mix", ps2r::host::ThreadRole::AudioMixer);
    const int frames = static_cast<int>(kSampleRate / hz_);
    std::vector<float> out(static_cast<size_t>(frames) * 2);
    std::vector<Command> local;
    const int target = ps2r::platform::sdl::AudioPullRing::LeadFrames();
    while (!mixer_stop_.load(std::memory_order_acquire)) {
        const int avail = ring_->Available();
        if (avail >= target) {

            const int us = std::clamp((avail - target + 1) * 1000 / 48, 250, 2000);
            std::this_thread::sleep_for(std::chrono::microseconds(us));
            continue;
        }
        std::lock_guard<std::mutex> tick(mix_mu_);
        {
            std::lock_guard<std::mutex> lock(mu_);
            local.swap(queue_);
            TakeMovie(frames);
            back_movie_->pos = 0;
            back_movie_->gains = movie_gains_;
        }
        uint64_t applied = 0;
        for (Command& c : local) { c.apply(*back_); applied = c.seq; }
        local.clear();
        back_->AdvanceControls();
        back_mixer_->MixInto(out.data(), frames);
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (applied) applied_seq_ = applied;
            for (uint32_t v = 0; v < kVoices; ++v) back_playing_[v] = back_->Inspect().voices[v].player.Playing();
        }
        ring_->Push(out.data(), frames);
    }
}

void AudioService::CommitBoundary() {
    std::array<bool, kVoices> playing{};
    uint64_t applied = 0;
    const uint32_t frames = kSampleRate / hz_;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (Command& c : batch_) queue_.push_back(std::move(c));
        if (ps2r::sound::PcmSource* movie = movie_source_) {
            int16_t pcm[2 * 960];
            movie_gains_ = movie->Gains();

            const int64_t depth = int64_t(frames) * movie_prime_ticks_;
            int got = 0, asked = 0;
            for (int64_t have = static_cast<int64_t>(movie_fifo_.size() / 2); have < depth;) {
                const int n = static_cast<int>(std::min<int64_t>(960, depth - have));
                const int g = movie->Pop(pcm, n);
                asked += n;
                if (g > 0) { movie_fifo_.insert(movie_fifo_.end(), pcm, pcm + 2 * g); got += g; have += g; }
                if (g < n) break;
            }
            if (asked > 0) movie_live_ = got > 0;
        }
        applied = applied_seq_;
        playing = back_playing_;
    }
    batch_.clear();
    for (uint32_t v = 0; v < kVoices; ++v)
        if (applied >= last_seq_[v] && !playing[v] && audio_.Inspect().voices[v].player.Playing()) {
            audio_.LatchEnded(v);
        }
    for (uint32_t parent = 0; parent < kStreams; ++parent) RefreshMusicPhase(parent);
}

void AudioService::Configure(bool open_device) { open_device_ = open_device; }

int32_t AudioService::InitialiseRemote() {
    Join();
    EnsureDevice();
    return 0;
}

void AudioService::HookClock() {

    te::Clock& clock = te::Services::Get().clock();
    if (&clock == hooked_clock_) return;
    hooked_clock_ = &clock;
    SetBoundaryHz(60);
    clock.AddBoundaryHook("audio mix", [this](uint64_t) { OnBoundary(); });
}

void AudioService::EnsureDevice() {
    HookClock();
    if (open_device_ && !device_open_) {
        ps2r::platform::AudioSpec spec;
        spec.sample_rate = static_cast<int>(kSampleRate);
        spec.channels = 2;
        spec.frames = static_cast<int>(kSampleRate / hz_);
        using Ring = ps2r::platform::sdl::AudioPullRing;

        Ring::SetLeadQuanta(2);
        ring_->Prime();
        device_open_ = sink_->Open(spec, *ring_);
        if (device_open_ && sink_->DeviceBufferFrames() > Ring::kQuantumFrames && Ring::LeadQuanta() < Ring::kDefaultLeadQuanta) {
            const int more = Ring::kDefaultLeadQuanta - Ring::LeadQuanta();
            Ring::SetLeadQuanta(Ring::kDefaultLeadQuanta);
            std::vector<float> silence(size_t(Ring::kQuantumFrames) * Ring::kChannels, 0.0f);
            for (int q = 0; q < more; ++q) ring_->Push(silence.data(), Ring::kQuantumFrames);
        }
        SelectMode();
    }
}

void AudioService::CloseDevice() {
    StopMixer();
    Join();
    if (async_thread_.joinable()) {
        { std::lock_guard<std::mutex> lock(async_mu_); async_stop_ = true; }
        async_cv_.notify_all();
        async_thread_.join();
        async_stop_ = false;
    }
    if (device_open_) { sink_->Close(); device_open_ = false; }
}

void AudioService::SetBoundaryHz(uint32_t hz) {
    if (hz == 50 || hz == 60) hz_ = hz;
}

void AudioService::MixTick(std::vector<float>& out) {
    const uint32_t frames = kSampleRate / hz_;
    out.assign(static_cast<std::size_t>(frames) * 2, 0.0f);
    audio_.AdvanceControls();
    for (uint32_t parent = 0; parent < kStreams; ++parent) RefreshMusicPhase(parent);
    mixer_.MixInto(out.data(), static_cast<int>(frames));
}

void AudioService::OnBoundary() {
    if (decoupled_) {
        CommitBoundary();
    } else if (mode_ == ClockMode::Tick) {
        Join();
        if (!async_thread_.joinable()) {
            async_movie_ = std::make_unique<MovieFeed>();
            async_thread_ = std::thread([this] { AsyncLoop(); });
        }

        if (movie_source_) {
            const uint32_t frames = kSampleRate / hz_;
            async_movie_->pcm.resize(size_t(frames) * 2);
            async_movie_->pcm.resize(size_t(std::max(0, movie_source_->Pop(async_movie_->pcm.data(), int(frames)))) * 2);
            async_movie_->pos = 0;
            async_movie_->gains = movie_source_->Gains();
            mixer_.SetMovieSource(async_movie_.get());
        } else mixer_.SetMovieSource(nullptr);
        async_inflight_.store(true, std::memory_order_release);
        { std::lock_guard<std::mutex> lock(async_mu_); async_posted_ = true; }
        async_cv_.notify_all();
        return;
    } else {
        uint64_t ticks = 1;
        if (mode_ == ClockMode::Field) {

            const uint64_t fields = te::Services::Get().clock().Fields();
            if (last_fields_ != 0 && fields > last_fields_) ticks = std::min<uint64_t>(fields - last_fields_, 8);
            last_fields_ = fields;
        }
        for (uint64_t k = 0; k < ticks; ++k) {
            MixTick(tick_);
            if (device_open_) ring_->Push(tick_.data(), static_cast<int>(tick_.size() / 2));
        }
    }
}

void AudioService::BeginMixTick() { audio_.BeginMixTick(); }
bool AudioService::StepVoice(uint32_t channel, float& left, float& right) { return audio_.StepVoice(channel, left, right); }
int AudioService::StepChannel(uint32_t channel, int frames, float* left, float* right, bool* owned) {
    return audio_.StepChannel(channel, frames, left, right, owned);
}
uint32_t AudioService::ChannelGroup(uint32_t channel) const { return audio_.SampleGroup(channel); }
const ps2r::sound::EffectSettings* AudioService::Effects() const { return &audio_.Inspect().effects; }

void AudioService::Reset() {

    EnsureDevice();

    for (uint32_t stream = 0; stream < kStreams; ++stream) StopStream(stream, Phase::Idle);
    Apply(-1, [](title::Audio& a) { a.StopVoices(); });
    Apply(-1, [](title::Audio& a) { a.ResetEffects(); });
    for (uint32_t group = 1; group <= kGroupMax; ++group)
        Apply(-1, [group](title::Audio& a) { return a.StoreGroupVolume(group << 16, 0x1000, 0x1000); });
}

void AudioService::PauseAll() { Apply(-1, [](title::Audio& a) { a.PauseAll(); }); }
void AudioService::ResumeAll() { Apply(-1, [](title::Audio& a) { a.ResumeAll(); }); }

int32_t AudioService::LendToMovie() { Apply(-1, [](title::Audio& a) { a.LendToMovie(); }); return 0; }
int32_t AudioService::ReclaimFromMovie() { Apply(-1, [](title::Audio& a) { a.ReclaimFromMovie(); }); return 0; }

void AudioService::CompactSoundMemory() {

}

void AudioService::SetGroupVolume(int32_t group, uint32_t left, uint32_t right) {

        const uint32_t g = static_cast<uint32_t>(group), l = std::min<uint32_t>(left, 0x1000), r = std::min<uint32_t>(right, 0x1000);
    Apply(-1, [g, l, r](title::Audio& a) { return a.SetGroupVolume(g, l, r); });
}

int32_t AudioService::SetVoiceVolume(int32_t voice, int16_t left, int16_t right) {
    left = FoldPhase(left);
    right = FoldPhase(right);
    if (voice < 0 || voice >= static_cast<int32_t>(kVoices)) return -1;

    const uint32_t v = static_cast<uint32_t>(voice);
    return Apply(voice, [v, left, right](title::Audio& a) { return a.SetVoiceVolume(v, left, right); }, false) ? -1 : 0;
}

int32_t AudioService::SetVoicePitch(int32_t voice, uint16_t pitch) {
    if (voice < 0 || voice >= static_cast<int32_t>(kVoices)) return -1;
    const uint32_t v = static_cast<uint32_t>(voice);
    return Apply(voice, [v, pitch](title::Audio& a) { return a.SetVoicePitch(v, pitch); }, false) ? -1 : 0;
}

void AudioService::SetVoiceReverb(int32_t voice, bool on) {
    if (voice < 0 || voice >= static_cast<int32_t>(kVoices)) return;
    const uint32_t v = static_cast<uint32_t>(voice);
    Apply(voice, [v, on](title::Audio& a) { return a.SetEffectSend(v, on); }, false);
}

void AudioService::ReleaseVoice(int32_t voice, uint32_t rate) {
    if (voice < 0 || voice >= static_cast<int32_t>(kVoices)) return;
    const uint32_t v = static_cast<uint32_t>(voice), r = std::min<uint32_t>(rate, 31);
    Apply(voice, [v, r](title::Audio& a) { return a.ReleaseVoice(v, r); });
}

int32_t AudioService::IsVoiceFree(int32_t voice) {
    if (voice < 0 || voice >= static_cast<int32_t>(kVoices)) return -1;
    return audio_.IsVoiceFree(static_cast<uint32_t>(voice));
}

void AudioService::SetReverb(int32_t core, int32_t mode, uint16_t depthLeft, uint16_t depthRight, uint16_t delay, uint16_t feedback) {
    if (core < 0 || core > 1) return;
    const uint32_t c = static_cast<uint32_t>(core);
    Apply(-1, [=](title::Audio& a) { return a.SetEffect(c, mode, depthLeft, depthRight, delay, feedback); });
}

void AudioService::ClearReverb(int32_t core) {
    if (core < 0 || core > 1) return;
    const uint32_t c = static_cast<uint32_t>(core);
    Apply(-1, [c](title::Audio& a) { return a.ClearEffect(c); });
}

void AudioService::SetReverbVolume(int32_t core, int16_t left, int16_t right) {
    if (core < 0 || core > 1) return;
    const uint32_t c = static_cast<uint32_t>(core);
    const uint16_t l = static_cast<uint16_t>(left), r = static_cast<uint16_t>(right);
    Apply(-1, [c, l, r](title::Audio& a) { return a.SetEffectVolume(c, l, r); });
}

uint32_t AudioService::ReserveSoundMemory(uint32_t size) {

    const uint32_t location = sound_memory_cursor_;
    sound_memory_cursor_ += (size + 15u) & ~15u;
    return location;
}

void AudioService::SoundBankLoaded(uint16_t bank) { audio_.SoundBankLoaded(bank); }

void AudioService::ReserveSounds(uint16_t count) { audio_.ReserveSounds(count); }

void AudioService::ReleaseSound(uint32_t sound) { audio_.ReleaseSound(sound); }

int32_t AudioService::PlaySound(uint32_t sound, uint32_t voiceAndGroup, int16_t left, int16_t right, uint16_t pitch,
                                uint32_t attack, uint32_t release, uint32_t loops) {
    left = FoldPhase(left);
    right = FoldPhase(right);
    const int32_t result = audio_.PlaySound(sound, voiceAndGroup, left, right, pitch, std::min<uint32_t>(attack, 127),
                                            std::min<uint32_t>(release, 31), loops);
    if (result == 0) {
        if (decoupled_) {

            const uint32_t v = voiceAndGroup & 0xffff;
            title::Audio::Voice voice = audio_.Inspect().voices[v];
            Post(static_cast<int>(v), [v, voice](title::Audio& a) { a.StartVoice(v, voice); });
        }
        return 0;
    }
    if (result == -3) return -1;
    return result == -2 ? -2 : -1;
}

int32_t AudioService::PitchOfRate(int32_t rate) { return title::Audio::PitchOfRate(rate); }

uint32_t AudioService::ChannelBufferAddress(int32_t channel) {
    return te::stream::Service().ChannelBufferAddress(channel);
}

bool AudioService::StoreSoundBank(uint32_t bank, std::vector<uint8_t> bytes) {
    Join();

    const std::size_t blocks = bytes.size() / VagDecoder::kBlockBytes * VagDecoder::kBlockBytes;
    if (blocks != bytes.size()) bytes.resize(blocks);
    return !bytes.empty() && audio_.StoreSound(bank, std::move(bytes));
}

void AudioService::ReadMusic(int32_t file, uint32_t offset, uint32_t size) {
    if (file < 0 || file >= static_cast<int32_t>(windows_.size())) { return; }
    windows_[static_cast<std::size_t>(file)] = Window{offset, size, true};
}

uint32_t AudioService::ParentOf(uint32_t stream) const {
    if (stream >= kStreams || !streams_[stream].owned) return UINT32_MAX;
    return streams_[stream].parent == UINT32_MAX ? stream : streams_[stream].parent;
}

bool AudioService::VoicePlaying(uint32_t voice) const {
    return voice < kVoices && audio_.Inspect().voices[voice].player.Playing();
}

void AudioService::StopStream(uint32_t stream, Phase phase) {
    const uint32_t parent = ParentOf(stream);
    if (parent == UINT32_MAX) return;
    const uint32_t child = streams_[parent].child;
    for (const uint32_t index : {parent, child}) {
        if (index >= kStreams) continue;
        auto& s = streams_[index];
        const uint32_t v = s.voice_and_group & 0xffff;
        const uint64_t generation = s.generation;
        Apply(static_cast<int>(v), [index, generation, v](title::Audio& a) { a.StopMusicVoice(index, generation, v); });
        if (phase == Phase::Idle) Apply(static_cast<int>(v), [v](title::Audio& a) { a.HandOffVoice(v); });
        s = Stream{};
        s.phase = phase;
        s.owned = phase != Phase::Idle;
    }
}

int32_t AudioService::StreamMusic(int32_t file, int32_t channel, uint32_t voiceAndGroup, uint16_t pitch, bool once) {
        if (channel < 0 || channel >= static_cast<int32_t>(kStreams) || (voiceAndGroup & 0xffff) >= kVoices ||
        (voiceAndGroup >> 16) > kGroupMax || file < 0 || file >= static_cast<int32_t>(windows_.size())) {
        return -1;
    }
    const auto stream = static_cast<uint32_t>(channel);
    StopStream(stream, Phase::Idle);
    auto& s = streams_[stream];
    s = Stream{};
    s.owned = true;
    s.file = file;
    s.voice_and_group = voiceAndGroup;
    s.pitch = pitch;
    s.looping = !once;
    s.generation = ++generation_;
    s.window = windows_[static_cast<std::size_t>(file)];
    s.phase = Phase::Reading;
    if (!s.window.set) { s.phase = Phase::Failed; return -1; }
    te::stream::Service().PrefetchFileBytes(file, s.window.offset, s.window.size);

    const uint32_t v = voiceAndGroup & 0xffff;
    const uint64_t generation = s.generation;
    Apply(static_cast<int>(v), [v](title::Audio& a) { a.HandOffVoice(v); });
    if (!Apply(static_cast<int>(v), [stream, generation, voiceAndGroup, pitch](title::Audio& a) {
            return a.ReserveMusicVoice(stream, generation, voiceAndGroup, pitch, 0, 0, 0, 0, 0, 0); })) {
        s.phase = Phase::Failed; return -1;
    }
    return channel;
}

void AudioService::InterleaveMusic(int32_t channel, uint32_t blockSize) {
    if (channel < 0 || channel >= static_cast<int32_t>(kStreams) || blockSize == 0 || blockSize % 16) { return; }
    auto& s = streams_[static_cast<std::size_t>(channel)];
    if (!s.owned || s.issued) { return; }
    s.block = blockSize;
    s.track = 0;
}

void AudioService::AddMusicChannel(int32_t child, int32_t parent, uint32_t voiceAndGroup, uint32_t soundAddress) {
    if (child < 0 || child >= static_cast<int32_t>(kStreams) || parent < 0 || parent >= static_cast<int32_t>(kStreams) ||
        child == parent || (voiceAndGroup & 0xffff) >= kVoices) { return; }
    auto& p = streams_[static_cast<std::size_t>(parent)];
    if (!p.owned || p.parent != UINT32_MAX || p.issued || p.child != UINT32_MAX || p.phase != Phase::Reading) {
        return;
    }
    StopStream(static_cast<uint32_t>(child), Phase::Idle);
    auto& c = streams_[static_cast<std::size_t>(child)];
    c = Stream{};
    c.owned = true;
    c.parent = static_cast<uint32_t>(parent);
    c.file = p.file;
    c.voice_and_group = voiceAndGroup;
    c.pitch = p.pitch;
    c.looping = p.looping;
    c.generation = p.generation;
    c.track = 1;
    c.sound_address = soundAddress;
    c.phase = Phase::Reading;
    p.child = static_cast<uint32_t>(child);
    const uint32_t v = voiceAndGroup & 0xffff, stream = static_cast<uint32_t>(child);
    const uint64_t generation = c.generation;
    const uint16_t pitch = p.pitch;
    Apply(static_cast<int>(v), [v](title::Audio& a) { a.HandOffVoice(v); });
    if (!Apply(static_cast<int>(v), [stream, generation, voiceAndGroup, pitch](title::Audio& a) {
            return a.ReserveMusicVoice(stream, generation, voiceAndGroup, pitch, 0, 0, 0, 0, 0, 0); })) {
        StopStream(static_cast<uint32_t>(parent), Phase::Failed);
    }
}

void AudioService::SetMusicEnd(int32_t channel, uint32_t end) {
    if (channel < 0 || channel >= static_cast<int32_t>(kStreams) || end == 0 || end % 16) { return; }
    auto& s = streams_[static_cast<std::size_t>(channel)];
    if (!s.owned || s.issued) { return; }
    s.end = end;
}

void AudioService::IssueMusicRead(uint32_t parent) {
    auto& s = streams_[parent];
    if (!s.owned || s.parent != UINT32_MAX || s.issued || s.phase != Phase::Reading) return;
    s.issued = true;
    const title::MusicAsset::Layout layout{s.child < kStreams ? 2u : 1u, s.block, s.end, s.looping};
    std::string error;
    if (!te::stream::Service().ReadMusicAsset(s.file, s.window.offset, s.window.size, layout, s.data, &error)) {
        StopStream(parent, Phase::Failed);
        return;
    }
    s.phase = Phase::Prepared;
    if (s.child < kStreams) streams_[s.child].phase = Phase::Prepared;
    if (!s.key_on_disabled) StartMusic(parent);
}

void AudioService::StartMusic(uint32_t parent) {
    auto& s = streams_[parent];
    if (s.phase != Phase::Prepared) return;

    if (s.data.header_bytes && s.data.source_rate) {
        const int32_t pitch = title::Audio::PitchOfRate(static_cast<int32_t>(s.data.source_rate));
        if (pitch > 0 && pitch <= 0x3fff) {
            for (const uint32_t index : {parent, s.child}) {
                if (index >= kStreams) continue;
                auto& member = streams_[index];
                member.pitch = static_cast<uint16_t>(pitch);
                const uint32_t v = member.voice_and_group & 0xffff;
                const uint16_t mp = member.pitch;
                Apply(static_cast<int>(v), [v, mp](title::Audio& a) { return a.SetVoicePitch(v, mp); }, false);
            }
        }
    }
    for (const uint32_t index : {parent, s.child}) {
        if (index >= kStreams) continue;
        auto& member = streams_[index];
        const uint32_t track = member.track < s.data.count ? member.track : 0;
        const uint32_t v = member.voice_and_group & 0xffff;
        const uint64_t generation = member.generation;
        const title::SampleVoice::Samples samples = s.data.tracks[track];
        if (!Apply(static_cast<int>(v), [index, generation, v, samples](title::Audio& a) {
                return a.StartMusicVoice(index, generation, v, samples); })) {
            StopStream(parent, Phase::Failed);
            return;
        }
        member.phase = Phase::Playing;
    }
}

void AudioService::RefreshMusicPhase(uint32_t parent) {
    auto& s = streams_[parent];
    if (!s.owned || s.parent != UINT32_MAX || s.phase != Phase::Playing) return;

    if (!VoicePlaying(s.voice_and_group & 0xffff)) {
        s.phase = Phase::Stopped;
        if (s.child < kStreams) streams_[s.child].phase = Phase::Stopped;
    }
}

void AudioService::PrepareMusic(int32_t channel) {
    const uint32_t parent = channel < 0 ? UINT32_MAX : ParentOf(static_cast<uint32_t>(channel));
    if (parent == UINT32_MAX) { return; }
    streams_[parent].key_on_disabled = true;
    IssueMusicRead(parent);
}

bool AudioService::IsMusicReady(int32_t channel) {
    const uint32_t parent = channel < 0 ? UINT32_MAX : ParentOf(static_cast<uint32_t>(channel));
    if (parent == UINT32_MAX) return false;
    IssueMusicRead(parent);
    RefreshMusicPhase(parent);
    const Phase phase = streams_[parent].phase;
    return phase == Phase::Prepared || phase == Phase::Playing;
}

int32_t AudioService::PlayMusic(int32_t channel) {
        const uint32_t parent = channel < 0 ? UINT32_MAX : ParentOf(static_cast<uint32_t>(channel));
    if (parent == UINT32_MAX) { return -1; }
    streams_[parent].key_on_disabled = false;
    IssueMusicRead(parent);
    StartMusic(parent);
    return streams_[parent].phase == Phase::Failed ? -1 : 0;
}

bool AudioService::IsMusicPlaying(int32_t channel) {
    const uint32_t parent = channel < 0 ? UINT32_MAX : ParentOf(static_cast<uint32_t>(channel));
    if (parent == UINT32_MAX) return false;
    RefreshMusicPhase(parent);
    const Phase phase = streams_[parent].phase;

    return phase == Phase::Reading || phase == Phase::Prepared || phase == Phase::Playing;
}

void AudioService::StopMusic(int32_t channel) {
        const uint32_t parent = channel < 0 ? UINT32_MAX : ParentOf(static_cast<uint32_t>(channel));
    if (parent == UINT32_MAX) return;
    StopStream(parent, Phase::Idle);
}

AudioService& Service() {
    static AudioService service;
    return service;
}

}

namespace te::audio {
void Configure(bool open_device) { Service().Configure(open_device); }
}
