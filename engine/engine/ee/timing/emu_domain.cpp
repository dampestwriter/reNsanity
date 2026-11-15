#include "engine/ee/timing/emu_domain.h"

uint64_t EmuDomain::BoundaryAdvance(uint64_t quantum, uint64_t carried, uint64_t final_gap,
                                    int64_t lead_before, int64_t* lead_after) {
    int64_t allowed = static_cast<int64_t>(final_gap) + (kLeadSlackTicks - lead_before);
    if (allowed < 0) allowed = 0;
    uint64_t adv = quantum < static_cast<uint64_t>(allowed) ? quantum : static_cast<uint64_t>(allowed);
    if (adv < carried) adv = carried;
    *lead_after = lead_before + static_cast<int64_t>(adv) - static_cast<int64_t>(final_gap);
    return adv;
}

void EmuDomain::SetRegimePin(int n) {
    pin_ = (n >= RegimeDetector::kMinN && n <= RegimeDetector::kMaxN) ? n : -1;

    regime_ = static_cast<uint32_t>(pin_ > 0 ? pin_ : det_.n());
}

bool EmuDomain::InLoop(uint64_t raw_count) const {
    return have_boundary_ && raw_count - boundary_raw_ <= kIdleReleaseTicks;
}

void EmuDomain::OnRawTick(uint64_t raw_count) {
    if (InLoop(raw_count) && !bypass_) {
        const uint64_t cap = emu_base_ + regime_;
        if (emu_count_ >= cap) return;
    }
    ++emu_count_;
}

void EmuDomain::OnDeferredTick(uint64_t raw_count) {
    if (!pacer_release_) {
        OnRawTick(raw_count);
        return;
    }
    ++emu_count_;
}

void EmuDomain::NoteArrival(uint64_t raw_count) { arrival_raw_ = raw_count; }

void EmuDomain::Reseed(uint64_t raw_count, bool in_loop) {
    det_.Seed();
    regime_ = static_cast<uint32_t>(pin_ > 0 ? pin_ : det_.n());
    lead_ = 0;
    emu_base_ = emu_count_;
    boundary_raw_ = raw_count;
    have_boundary_ = in_loop;
}

void EmuDomain::OnBoundary(uint64_t raw_count) {
    ++boundaries_;
    const bool was_in_loop = InLoop(raw_count);
    if (!was_in_loop || bypass_) {

        Reseed(raw_count, true);
        return;
    }
    if (pacer_release_) {

        lead_ = 0;
        emu_base_ = emu_count_;
        boundary_raw_ = raw_count;
        return;
    }
    const uint64_t arrival_gap = arrival_raw_ > boundary_raw_ ? arrival_raw_ - boundary_raw_ : 0;
    const uint64_t final_gap = raw_count > boundary_raw_ ? raw_count - boundary_raw_ : 0;

    const uint32_t old_n = regime_;
    if (pin_ <= 0 && det_.OnBoundary(arrival_gap, final_gap))
        regime_ = static_cast<uint32_t>(det_.n());
    const uint64_t carried = emu_count_ > emu_base_ ? emu_count_ - emu_base_ : 0;
    int64_t lead_after = 0;
    const uint64_t adv = BoundaryAdvance(old_n, carried, final_gap, lead_, &lead_after);
    const uint64_t settled = emu_base_ + adv;
    if (settled > emu_count_) emu_count_ = settled;
    lead_ = lead_after;
    emu_base_ = emu_count_;
    boundary_raw_ = raw_count;
}

void EmuDomain::ReAnchor(uint64_t raw_count) {
    boundaries_ = 0;
    Reseed(raw_count, false);
}
