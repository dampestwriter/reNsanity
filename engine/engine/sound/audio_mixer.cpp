#include "engine/sound/audio_mixer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "engine/sound/pcm_source.h"
#include "engine/sound/voice_source.h"

namespace ps2r::sound {
namespace {
bool Gain(float value) { return std::isfinite(value) && value >= 0 && value <= 1; }
inline int32_t Sat16(int32_t v) { return spu2::Clamp16(v); }
}

namespace {
constexpr int kHeadroomShift = 2;

constexpr int kReverbHeadroom = 2;
}
namespace {
constexpr float kDuckThreshold = 0.9f;
constexpr float kDuckFloor = 0.35f;
constexpr float kDuckRelease = 0.04f;
}

bool AudioMixer::SetGroup(uint32_t group, float left, float right) {
    if (group >= kGroups || !Gain(left) || !Gain(right)) return false;
    state_.left[group] = left; state_.right[group] = right;
    return true;
}

bool AudioMixer::Validate(const State& state) {
    if (!Gain(state.master)) return false;
    for (uint32_t group = 0; group < kGroups; ++group)
        if (!Gain(state.left[group]) || !Gain(state.right[group])) return false;
    return true;
}

bool AudioMixer::Restore(const State& state) {
    if (!Validate(state)) return false;

    state_ = state;
    limiter_.Reset();
    return true;
}

void AudioMixer::ProgramEffects() {
    const EffectSettings* fx = voices_.Effects();
    for (uint32_t core = 0; core < kGroups; ++core) {
        if (!fx) {
            reverb_[core].SetEnabled(false);
            continue;
        }
        const auto& c = fx->cores[core];
        if (c.generation != reverb_generation_[core]) {
            reverb_[core].SetMode(c.mode, c.delay, c.feedback, true);
            reverb_generation_[core] = c.generation;
        }
        reverb_[core].SetEnabled(c.enabled);
    }
}

void AudioMixer::MixInto(float* stereo, int frames) {
    if (frames <= 0) return;
    voices_.BeginMixTick();
    ProgramEffects();
    const EffectSettings* fx = voices_.Effects();
    const auto gain = state_;

    const auto movie_gain = movie_ ? movie_->Gains() : std::array<float, 2>{0, 0};
    constexpr int chunk = 128;
    int16_t pcm[chunk * 2];

    int32_t mvol[kGroups][2];
    for (uint32_t core = 0; core < kGroups; ++core) {
        mvol[core][0] = std::min<int32_t>(int32_t(std::lround(double(gain.left[core]) * 65534.0)), 0x7FFF);
        mvol[core][1] = std::min<int32_t>(int32_t(std::lround(double(gain.right[core]) * 65534.0)), 0x7FFF);
    }
    const int32_t bvol[2] = {int32_t(movie_gain[0] * 32767.0f), int32_t(movie_gain[1] * 32767.0f)};

    for (auto& unit : reverb_) unit.SetHeadroom(kReverbHeadroom);
    int32_t dry[kGroups][2][chunk], wet[kGroups][2][chunk];
    static thread_local float duck_l[kGroups * kChannelsPerGroup][chunk], duck_r[kGroups * kChannelsPerGroup][chunk];
    static thread_local bool duck_own[kGroups * kChannelsPerGroup][chunk];
    alignas(16) float channel_left[chunk], channel_right[chunk];
    bool owned[chunk];
    for (int base = 0; base < frames; base += chunk) {
        const int n = std::min(chunk, frames - base);

        const int movie_frames = movie_ ? movie_->Pop(pcm, n) : 0;
        std::fill_n(&dry[0][0][0], kGroups * 2 * chunk, 0);
        std::fill_n(&wet[0][0][0], kGroups * 2 * chunk, 0);

        auto add = [&](uint32_t channel, int frame, float l_out, float r_out) {
            const uint32_t core = channel / kChannelsPerGroup;
            int32_t l = int32_t(l_out * 32768.0f), r = int32_t(r_out * 32768.0f);
            l >>= kHeadroomShift;
            r >>= kHeadroomShift;
            dry[core][0][frame] += l;
            dry[core][1][frame] += r;
            if (fx && fx->send[channel]) {
                wet[core][0][frame] += l;
                wet[core][1][frame] += r;
            }
        };
        int stepped = voices_.StepChannel(0, n, channel_left, channel_right, owned);
        if (stepped >= 0) {

            struct Stepped { int count; bool sfx; };
            Stepped info[kGroups * kChannelsPerGroup];
            for (uint32_t channel = 0;;) {
                info[channel] = {stepped, stepped > 0 && voices_.ChannelGroup(channel) == 1};
                if (stepped) {
                    std::memcpy(duck_l[channel], channel_left, sizeof(float) * size_t(n));
                    std::memcpy(duck_r[channel], channel_right, sizeof(float) * size_t(n));
                    std::memcpy(duck_own[channel], owned, size_t(n));
                }
                if (++channel == kGroups * kChannelsPerGroup) break;
                stepped = voices_.StepChannel(channel, n, channel_left, channel_right, owned);
            }

            float others[chunk][2] = {}, effects[chunk][2] = {};
            float target = 1.0f;
            float path[kGroups][2];
            for (uint32_t core = 0; core < kGroups; ++core) {
                path[core][0] = gain.master;
                path[core][1] = gain.master;
                for (uint32_t next = core; next < kGroups; ++next) {
                    path[core][0] *= gain.left[next];
                    path[core][1] *= gain.right[next];
                }
            }
            for (uint32_t channel = 0; channel < kGroups * kChannelsPerGroup; ++channel) {
                if (!info[channel].count) continue;
                auto& sum = info[channel].sfx ? effects : others;
                const float* scale = path[channel / kChannelsPerGroup];
                for (int frame = 0; frame < n; ++frame)
                    if (duck_own[channel][frame]) {
                        sum[frame][0] += duck_l[channel][frame] * scale[0];
                        sum[frame][1] += duck_r[channel][frame] * scale[1];
                    }
            }
            for (int frame = 0; frame < n; ++frame) {
                const float other = std::max(std::fabs(others[frame][0]), std::fabs(others[frame][1]));
                const float sfx = std::max(std::fabs(effects[frame][0]), std::fabs(effects[frame][1]));
                if (sfx > 1e-6f && other + sfx > kDuckThreshold)
                    target = std::min(target, std::max(kDuckFloor, (kDuckThreshold - other) / sfx));
            }
            float g0 = sfx_gain_, g1;
            if (target < sfx_gain_) g1 = target;
            else if (sfx_gain_ < 1.0f) g1 = 1.0f - sfx_gain_ < 1e-3f ? 1.0f : sfx_gain_ + (std::min(target, 1.0f) - sfx_gain_) * kDuckRelease;
            else g1 = 1.0f;
            const int ramp = std::max(1, n / 2);
            for (uint32_t channel = 0; channel < kGroups * kChannelsPerGroup; ++channel) {
                if (!info[channel].count) continue;
                for (int frame = 0; frame < n; ++frame) {
                    if (!duck_own[channel][frame]) continue;
                    float l = duck_l[channel][frame], r = duck_r[channel][frame];
                    if (info[channel].sfx && (g0 != 1.0f || g1 != 1.0f)) {
                        const float g = frame >= ramp ? g1 : g0 + (g1 - g0) * float(frame + 1) / float(ramp);
                        l *= g;
                        r *= g;
                    }
                    add(channel, frame, l, r);
                }
            }
            sfx_gain_ = g1;
        } else {
            for (int frame = 0; frame < n; ++frame)
                for (uint32_t channel = 0; channel < kGroups * kChannelsPerGroup; ++channel) {
                    float l = 0, r = 0;
                    if (!voices_.StepVoice(channel, l, r)) continue;
                    add(channel, frame, l, r);
                }
        }
        auto chain_frame = [&](int frame) {

            int32_t movie_left = 0, movie_right = 0;
            if (frame < movie_frames) {
                movie_left = spu2::ApplyVolume(pcm[frame * 2], bvol[0]);
                movie_right = spu2::ApplyVolume(pcm[frame * 2 + 1], bvol[1]);
                movie_left >>= kHeadroomShift;
                movie_right >>= kHeadroomShift;
            }
            int32_t ext_left = 0, ext_right = 0, out_left = 0, out_right = 0;
            for (uint32_t core = 0; core < kGroups; ++core) {
                const int32_t in_left = core == 0 ? movie_left : spu2::ApplyVolume(ext_left, 0x7FFF);
                const int32_t in_right = core == 0 ? movie_right : spu2::ApplyVolume(ext_right, 0x7FFF);
                int32_t dl = Sat16(dry[core][0][frame]) + in_left, dr = Sat16(dry[core][1][frame]) + in_right;
                const auto* c = fx ? &fx->cores[core] : nullptr;
                if (c && (c->mode != 0 || c->volume_left || c->volume_right)) {
                    int32_t wl = Sat16(wet[core][0][frame]), wr = Sat16(wet[core][1][frame]);
                    if (core == 0) {
                        wl += in_left;
                        wr += in_right;
                    }
                    int32_t rl = 0, rr = 0;
                    reverb_[core].Tick(wl, wr, rl, rr);
                    const int32_t evl = int32_t((int64_t(rl) * int16_t(c->volume_left)) >> 15), evr = int32_t((int64_t(rr) * int16_t(c->volume_right)) >> 15);
                    dl += evl;
                    dr += evr;
                }
                const int32_t ol = spu2::ApplyVolume(Sat16(dl), mvol[core][0]);
                const int32_t orr = spu2::ApplyVolume(Sat16(dr), mvol[core][1]);
                if (core == 0) {
                    ext_left = ol;
                    ext_right = orr;
                } else {
                    out_left = Sat16(ol);
                    out_right = Sat16(orr);
                }
            }
            float left = float(out_left) / 32768.0f, right = float(out_right) / 32768.0f;
            left *= float(1 << kHeadroomShift);
            right *= float(1 << kHeadroomShift);
            stereo[(base + frame) * 2] = left * gain.master;
            stereo[(base + frame) * 2 + 1] = right * gain.master;
        };
        for (int frame = 0; frame < n; ++frame) chain_frame(frame);
    }
    limiter_.Process(stereo, frames);
    for (int i = 0; i < frames * 2; ++i) stereo[i] = std::clamp(stereo[i], -1.0f, 1.0f);
}
}
