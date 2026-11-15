#include "engine/platform/ps2e/present_pacer.h"

#include <cctype>
#include <cstring>

#include "engine/platform/host/platform.h"

namespace ps2r {
namespace ps2e {

namespace {
const uint32_t kHz[] = {30, 60, 120, 144, 240, 0};
constexpr int64_t kCoarseMarginNs = 1000000;
constexpr int64_t kFineStepNs = 100000;
}

uint32_t PresentPacer::Hz(Rate rate) { return kHz[static_cast<unsigned>(rate) < 6 ? static_cast<unsigned>(rate) : 1]; }

uint32_t PresentPacer::SlotsPerTick(Rate rate, uint32_t game_hz) {
    const uint32_t hz = Hz(rate);
    if (!game_hz || !hz || hz % game_hz) return 0;
    return hz / game_hz;
}

void PresentPacer::SetRate(Rate rate) {
    rate_ = rate;
    const uint32_t hz = Hz(rate);
    period_ns_ = hz ? static_cast<int64_t>(1000000000ull / hz) : 0;
    anchored_ = false;
    k_ = 0;
}

bool PresentPacer::ShowsPicture(uint64_t picture) const {
    if (rate_ == Rate::Rate30) return (picture & 1u) == 0;
    return true;
}

void PresentPacer::OnTickEdge(int64_t edge_ns) {
    if (!PacedBySlots(rate_) || rate_ == Rate::Uncapped) return;
    const uint32_t m = SlotsPerTick(rate_, game_hz_);
    if (m == 0 && anchored_) return;
    anchor_ns_ = edge_ns;

    k_ = 0;
    if (anchored_ && period_ns_ > 0) {
        while (anchor_ns_ + static_cast<int64_t>(k_) * period_ns_ < last_slot_ns_ + period_ns_ / 2) ++k_;
    }
    anchored_ = true;
}

int64_t PresentPacer::NextSlotNs(int64_t now_ns) {
    if (rate_ == Rate::Uncapped || !PacedBySlots(rate_)) return now_ns;
    if (!anchored_) {
        anchor_ns_ = now_ns;
        k_ = 0;
        anchored_ = true;
    }
    int64_t slot = anchor_ns_ + static_cast<int64_t>(k_) * period_ns_;
    if (slot + period_ns_ < now_ns) {

        anchor_ns_ = now_ns;
        k_ = 0;
        slot = now_ns;
    }
    return slot;
}

void PresentPacer::SlotServed(int64_t slot_ns, int64_t) {
    k_++;
    last_slot_ns_ = slot_ns;
}

int64_t PresentPacer::NowNs() { return static_cast<int64_t>(ps2r::host::MonotonicNowNs()); }

void PresentPacer::SleepUntilNs(int64_t deadline_ns) {
    for (;;) {
        const int64_t now = NowNs();
        if (now >= deadline_ns) return;
        const int64_t remaining = deadline_ns - now;
        if (remaining > kCoarseMarginNs) ps2r::host::SleepForNs(static_cast<uint64_t>(remaining - kCoarseMarginNs));
        else ps2r::host::SleepForNs(static_cast<uint64_t>(remaining < kFineStepNs ? remaining : kFineStepNs));
    }
}

}
}
