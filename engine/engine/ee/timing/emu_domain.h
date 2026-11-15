#pragma once

#include <cstdint>

#include "engine/ee/timing/regime_detector.h"

class EmuDomain {
public:
    static constexpr int64_t kLeadSlackTicks = 1;
    static constexpr uint64_t kIdleReleaseTicks = 6;

    static uint64_t BoundaryAdvance(uint64_t quantum, uint64_t carried, uint64_t final_gap,
                                    int64_t lead_before, int64_t* lead_after);

    void SetRegimePin(int n);
    int RegimePin() const { return pin_; }

    void SetBypass(bool on) { bypass_ = on; }
    bool Bypass() const { return bypass_; }

    void OnRawTick(uint64_t raw_count);

    void OnDeferredTick(uint64_t raw_count);
    void SetPacerRelease(bool on) { pacer_release_ = on; }
    bool PacerRelease() const { return pacer_release_; }

    void NoteArrival(uint64_t raw_count);

    void OnBoundary(uint64_t raw_count);

    void ReAnchor(uint64_t raw_count);

    uint64_t DomainTick() const { return emu_count_; }
    uint32_t Regime() const { return regime_; }
    int64_t Lead() const { return lead_; }
    uint64_t BoundaryCount() const { return boundaries_; }
    uint64_t LastBoundaryTick() const { return emu_base_; }
    bool InLoop(uint64_t raw_count) const;
    const RegimeDetector& Detector() const { return det_; }

private:
    void Reseed(uint64_t raw_count, bool in_loop);

    uint64_t emu_count_ = 0;
    uint64_t emu_base_ = 0;
    uint64_t boundary_raw_ = 0;
    uint64_t arrival_raw_ = 0;
    uint64_t boundaries_ = 0;
    bool have_boundary_ = false;
    int64_t lead_ = 0;
    uint32_t regime_ = 1;
    int pin_ = -1;
    bool bypass_ = false;
    bool pacer_release_ = false;
    RegimeDetector det_;
};
