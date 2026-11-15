#pragma once

#include <cstdint>

class RegimeDetector {
public:
    static constexpr int kMinN = 1;
    static constexpr int kMaxN = 5;
    static constexpr uint32_t kFullWin = 120;
    static constexpr uint32_t kFastWin = 15;
    static constexpr double kUpFrac = 0.30;
    static constexpr double kDownFrac = 0.90;
    static constexpr double kUpFracFast = 0.50;
    static constexpr double kDownFracFast = 0.95;

    void Seed();

    bool OnBoundary(uint64_t arrival_gap, uint64_t final_gap);

    int n() const { return n_; }
    uint32_t samples() const { return samples_; }
    bool fast_window() const { return fast_; }

private:
    int n_ = 1;
    uint32_t samples_ = 0;
    uint32_t over_ = 0;
    uint32_t under_ = 0;
    bool fast_ = true;
};
