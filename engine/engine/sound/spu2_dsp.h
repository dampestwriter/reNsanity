#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace ps2r::sound::spu2 {

inline int32_t Clamp16(int32_t v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }

inline int32_t ApplyVolume(int32_t sample, int32_t volume) { return (sample * volume) >> 15; }

extern const int16_t kGauss[512];

inline int32_t Interpolate(int32_t oldest, int32_t older, int32_t old, int32_t newest, uint32_t counter) {
    const uint32_t i = (counter >> 4) & 0xFF;
    int32_t out = (kGauss[0x0FF - i] * oldest) >> 15;
    out += (kGauss[0x1FF - i] * older) >> 15;
    out += (kGauss[0x100 + i] * old) >> 15;
    out += (kGauss[0x000 + i] * newest) >> 15;
    return out;
}

struct Rate {
    bool exponential = false, decrease = false;
    uint8_t shift = 0, step = 0;
};
struct RateCounter {
    uint32_t ticks = 0;

    bool Tick(const Rate& r, int32_t& level, int32_t lo, int32_t hi, bool negative = false);
};

class Adsr {
public:
    enum Phase : uint8_t { Off, Attack, Decay, Sustain, Release };

    void Set(uint16_t adsr1, uint16_t adsr2) { adsr1_ = adsr1; adsr2_ = adsr2; }
    void SetAdsr2(uint16_t adsr2) { adsr2_ = adsr2; }
    uint16_t Adsr1() const { return adsr1_; }
    uint16_t Adsr2() const { return adsr2_; }
    void KeyOn() { phase_ = Attack; level_ = 0; counter_ = {}; }
    void KeyOff() { if (phase_ != Off) { phase_ = Release; counter_ = {}; } }
    void Mute() { phase_ = Off; level_ = 0; counter_ = {}; }

    void Tick();
    Phase phase() const { return phase_; }
    int32_t level() const { return level_; }

private:
    uint16_t adsr1_ = 0, adsr2_ = 0;
    Phase phase_ = Off;
    int32_t level_ = 0;
    RateCounter counter_;
};

class Volume {
public:
    void Write(uint16_t reg);
    void Tick();
    int32_t level() const { return level_; }
    uint16_t reg() const { return reg_; }

private:
    uint16_t reg_ = 0;
    int32_t level_ = 0;
    RateCounter counter_;
};

constexpr int kEffectModes = 10;
extern const uint16_t kEffectPresets[kEffectModes][32];
extern const uint32_t kEffectAreaHalfwords[kEffectModes];

class Reverb {
public:
    Reverb();

    void SetMode(int mode, uint16_t delay, uint16_t feedback, bool clear);
    void SetEnabled(bool on) { enabled_ = on; }

    void SetHeadroom(int bits) { lim_ = int32_t(32768) << bits; }
    int mode() const { return mode_; }

    void Tick(int32_t in_left, int32_t in_right, int32_t& out_left, int32_t& out_right);


private:
    int32_t Limit(int64_t v) const { return int32_t(v < -int64_t(lim_) ? -lim_ : v > lim_ - 1 ? lim_ - 1 : v); }
    int32_t Down(int channel) const;
    void Up(int32_t& left, int32_t& right) const;
    uint32_t At(uint32_t offset) const;
    void PrepareAddresses();

    std::array<std::array<uint32_t, 14>, 2> offsets_{};
    uint32_t area_cursor_ = 0;

    int mode_ = 0;
    bool enabled_ = false;
    uint32_t cycle_ = 0;
    uint32_t pos_ = 0;
    std::array<uint16_t, 32> regs_{};
    int32_t lim_ = 32768;
    std::array<std::array<int32_t, 64>, 2> down_{}, up_{};
    std::vector<int32_t> area_;
};

}
