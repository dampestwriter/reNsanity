#pragma once

#include <cstdint>

class TickSource {
public:
    enum class Policy { Deterministic, Play };
    enum class Mode { Realtime, Freerun, Turbo, Warp };

    static constexpr uint32_t kBasePeriodUs = 16667;
    static constexpr uint32_t kTurboKMin = 2;
    static constexpr uint32_t kTurboKMax = 64;
    static uint32_t PeriodUsForK(uint32_t k);

    void SetPolicy(Policy p) { policy_ = p; }
    Policy policy() const { return policy_; }
    void SetMode(Mode m, uint32_t k = 1);
    Mode mode() const { return mode_; }
    uint32_t K() const { return k_; }

    uint64_t Tick();
    uint64_t RawCount() const { return raw_count_; }

    uint32_t PeriodUs() const;

    bool Bypass() const { return mode_ == Mode::Freerun || mode_ == Mode::Warp; }

    void ResetForRestore(uint64_t raw_count) { raw_count_ = raw_count; }

private:
    Policy policy_ = Policy::Deterministic;
    Mode mode_ = Mode::Realtime;
    uint32_t k_ = 1;
    uint64_t raw_count_ = 0;
};
