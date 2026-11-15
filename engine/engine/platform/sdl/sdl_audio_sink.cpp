#include "engine/platform/sdl/sdl_audio_sink.h"

#include <SDL.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <switch.h>

#include <atomic>
#include <thread>
#include <vector>

#include "engine/platform/host/platform.h"

#include "engine/platform/sdl/audio_pull_ring.h"

namespace ps2r::platform::sdl {

namespace {

constexpr int kDeviceBufferSamples = 2048;
constexpr int kAllowedChanges = SDL_AUDIO_ALLOW_SAMPLES_CHANGE;
}

SdlAudioSink::~SdlAudioSink() { Close(); }

void SdlAudioSink::Callback(void* userdata, uint8_t* stream, int len) {
    auto* self = static_cast<SdlAudioSink*>(userdata);

    static thread_local bool placed = false;
    if (!placed) {
        placed = true;
        ps2r::platform::host::PinCurrentThreadInteractive("te-audio-pull", ps2r::host::ThreadRole::AudioCallback);
    }
    float* out = reinterpret_cast<float*>(stream);
    const int frames = len / (AudioPullRing::kChannels * static_cast<int>(sizeof(float)));
    self->ring_->Pull(out, frames);
}

struct SdlAudioSink::Audren {
    static constexpr int kFrames = 240, kBuffers = 3;
    AudioDriver driver{};
    void* pool = nullptr;
    size_t pool_bytes = 0;
    AudioDriverWaveBuf buffers[kBuffers]{};
    std::thread thread;
    std::atomic<bool> stop{false}, paused{false};
    bool renderer = false, created = false;

};

bool SdlAudioSink::OpenAudren(const AudioSpec& spec, AudioPullRing& ring) {
    static const AudioRendererConfig config = {AudioRendererOutputRate_48kHz, 24, 0, 1, 1, 2};
    if (spec.sample_rate != 48000) return false;
    auto* a = new Audren;
    Result rc = audrenInitialize(&config);
    if (R_SUCCEEDED(rc)) a->renderer = true;
    if (R_SUCCEEDED(rc)) rc = audrvCreate(&a->driver, &config, AudioPullRing::kChannels);
    if (R_SUCCEEDED(rc)) a->created = true;
    if (R_SUCCEEDED(rc)) {
        a->pool_bytes = (size_t(Audren::kBuffers) * Audren::kFrames * AudioPullRing::kChannels * sizeof(int16_t) + 0xFFF) & ~size_t(0xFFF);
        a->pool = memalign(AUDREN_MEMPOOL_ALIGNMENT, a->pool_bytes);
        if (!a->pool) rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }
    if (R_SUCCEEDED(rc)) {
        std::memset(a->pool, 0, a->pool_bytes);
        armDCacheFlush(a->pool, a->pool_bytes);
        const int pool_id = audrvMemPoolAdd(&a->driver, a->pool, a->pool_bytes);
        if (pool_id < 0 || !audrvMemPoolAttach(&a->driver, pool_id)) rc = MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }
    if (R_SUCCEEDED(rc)) {
        static const u8 sink_channels[] = {0, 1};
        if (audrvDeviceSinkAdd(&a->driver, AUDREN_DEFAULT_DEVICE_NAME, 2, sink_channels) < 0) rc = MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }
    if (R_SUCCEEDED(rc)) rc = audrvUpdate(&a->driver);
    if (R_SUCCEEDED(rc)) rc = audrenStartAudioRenderer();
    if (R_SUCCEEDED(rc) && !audrvVoiceInit(&a->driver, 0, AudioPullRing::kChannels, PcmFormat_Int16, 48000))
        rc = MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (R_FAILED(rc)) {
        audren_ = a;
        CloseAudren();
        return false;
    }
    audrvVoiceSetVolume(&a->driver, 0, 1.0f);
    audrvMixSetVolume(&a->driver, AUDREN_FINAL_MIX_ID, 1.0f);
    audrvVoiceSetDestinationMix(&a->driver, 0, AUDREN_FINAL_MIX_ID);
    audrvVoiceSetMixFactor(&a->driver, 0, 1.0f, 0, 0);
    audrvVoiceSetMixFactor(&a->driver, 0, 0.0f, 0, 1);
    audrvVoiceSetMixFactor(&a->driver, 0, 0.0f, 1, 0);
    audrvVoiceSetMixFactor(&a->driver, 0, 1.0f, 1, 1);
    audrvVoiceStart(&a->driver, 0);
    for (int i = 0; i < Audren::kBuffers; ++i) {
        AudioDriverWaveBuf& b = a->buffers[i];
        b.data_raw = a->pool;
        b.size = a->pool_bytes;
        b.start_sample_offset = i * Audren::kFrames;
        b.end_sample_offset = (i + 1) * Audren::kFrames;
    }
    ring_ = &ring;
    audren_ = a;
    a->thread = std::thread([this, a] {
        ps2r::host::PinCurrentThreadInteractive(ps2r::host::ThreadRole::AudioCallback);
        std::vector<float> f(size_t(Audren::kFrames) * AudioPullRing::kChannels);
        while (!a->stop.load(std::memory_order_acquire)) {
            audrvUpdate(&a->driver);
            bool queued = false;
            for (int i = 0; i < Audren::kBuffers; ++i) {
                AudioDriverWaveBuf& b = a->buffers[i];
                if (b.state != AudioDriverWaveBufState_Free && b.state != AudioDriverWaveBufState_Done) continue;
                {
                    if (a->paused.load(std::memory_order_relaxed)) std::fill(f.begin(), f.end(), 0.0f);
                    else ring_->Pull(f.data(), Audren::kFrames);
                }

                int16_t* dst = static_cast<int16_t*>(a->pool) + size_t(i) * Audren::kFrames * AudioPullRing::kChannels;

                for (size_t k = 0; k < f.size(); ++k) {
                    const float v = std::floor(f[k] * 32768.0f + 0.5f);
                    dst[k] = int16_t(v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : int32_t(v));
                }
                armDCacheFlush(dst, size_t(Audren::kFrames) * AudioPullRing::kChannels * sizeof(int16_t));
                audrvVoiceAddWaveBuf(&a->driver, 0, &b);
                queued = true;
            }
            if (queued) audrvUpdate(&a->driver);
            audrenWaitFrame();
        }
    });
    got_freq_ = 48000;
    got_channels_ = AudioPullRing::kChannels;
    got_samples_ = Audren::kFrames;
    return true;
}

void SdlAudioSink::CloseAudren() {
    Audren* a = audren_;
    if (!a) return;
    a->stop.store(true, std::memory_order_release);
    if (a->thread.joinable()) a->thread.join();
    if (a->created) {
        audrvVoiceStop(&a->driver, 0);
        audrvClose(&a->driver);
    }
    if (a->renderer) audrenExit();
    std::free(a->pool);
    delete a;
    audren_ = nullptr;
    ring_ = nullptr;
}

bool SdlAudioSink::Open(const AudioSpec& spec, AudioPullRing& ring) {
    if (device_ != 0 || audren_) return true;
    if (OpenAudren(spec, ring)) return true;
    if (spec.channels != AudioPullRing::kChannels) {
        return false;
    }

    if (!audio_subsystem_) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            return false;
        }
        audio_subsystem_ = true;
    }

    ring_ = &ring;

    SDL_AudioSpec want{};
    want.freq     = spec.sample_rate;
    want.format   = AUDIO_F32SYS;
    want.channels = static_cast<Uint8>(spec.channels);
    want.samples  = kDeviceBufferSamples;
    want.callback = &SdlAudioSink::Callback;
    want.userdata = this;

    SDL_AudioSpec got{};

    const SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &want, &got, kAllowedChanges);
    if (dev == 0) {
        ring_ = nullptr;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        audio_subsystem_ = false;
        return false;
    }
    device_       = dev;
    got_freq_     = got.freq;
    got_channels_ = got.channels;
    got_samples_  = got.samples;
    paused_ = false;
    SDL_PauseAudioDevice(dev, 0);
    return true;
}

void SdlAudioSink::Close() {
    CloseAudren();
    if (device_ != 0) {

        SDL_CloseAudioDevice(device_);
        device_ = 0;
        ring_   = nullptr;
    }
    if (audio_subsystem_) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        audio_subsystem_ = false;
    }
    paused_ = false;
}

int SdlAudioSink::DeviceBufferFrames() const { return got_samples_; }

}
