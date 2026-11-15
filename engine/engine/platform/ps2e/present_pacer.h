#pragma once

#include <cstdint>

namespace ps2r {
namespace ps2e {

class PresentPacer {
public:
    enum class Rate : uint8_t { Rate30 = 0, Rate60, Rate120, Rate144, Rate240, Uncapped };

    PresentPacer() = default;


    static uint32_t Hz(Rate rate);

    static bool PacedBySlots(Rate rate) { return rate != Rate::Rate30 && rate != Rate::Rate60; }

    static uint32_t SlotsPerTick(Rate rate, uint32_t game_hz = 60);

    void SetRate(Rate rate);

    void SetGameHz(uint32_t hz) { game_hz_ = hz ? hz : 60u; anchored_ = false; }
    Rate GetRate() const { return rate_; }

    bool ShowsPicture(uint64_t picture) const;

    void OnTickEdge(int64_t edge_ns);
    int64_t NextSlotNs(int64_t now_ns);

    void SlotServed(int64_t slot_ns, int64_t now_ns);

    int64_t SlotPeriodNs() const { return period_ns_; }

    static void SleepUntilNs(int64_t deadline_ns);

    static int64_t NowNs();

private:
    Rate rate_ = Rate::Rate60;
    uint32_t game_hz_ = 60;
    int64_t period_ns_ = 16666666;
    bool anchored_ = false;
    int64_t anchor_ns_ = 0;
    uint64_t k_ = 0;
    int64_t last_slot_ns_ = -(1LL << 62);
};

}
}
