#pragma once

#include <cstdint>

#include "engine/ee/timing/emu_domain.h"

class DerivedClocks {
public:

    static constexpr uint64_t kCountsPerTick[4] = {2457600ull, 153600ull, 9600ull, 262ull};
    static constexpr uint64_t kCop0CountsPerTick = 4915200ull;
    static constexpr uint32_t kFieldUs = 16667;
    static constexpr uint32_t kCdvdKbPerTick = 256;

    static constexpr uint64_t kRtcFixedEpochS = 1118836800ull;

    struct RtcTime {
        int year, month, day, hour, minute, second;
    };

    explicit DerivedClocks(const EmuDomain& domain) : domain_(domain) {}

    void SetRtcFixed(bool fixed) { rtc_fixed_ = fixed; }
    bool RtcFixed() const { return rtc_fixed_; }

    uint64_t TimerCounts(uint32_t clks) const;
    uint64_t T0Counts() const { return TimerCounts(2); }

    uint64_t VblankCount() const { return domain_.DomainTick(); }

    uint64_t FrameBoundaryCount() const { return domain_.BoundaryCount(); }
    uint32_t SyncVFieldParity() const { return static_cast<uint32_t>(domain_.DomainTick() & 1u); }

    uint64_t BudgetAnchorCounts() const { return domain_.LastBoundaryTick() * kCountsPerTick[2]; }

    uint64_t Cop0Count() const { return domain_.DomainTick() * kCop0CountsPerTick; }
    static RtcTime EpochToCivil(uint64_t seconds);

    uint64_t CdvdEpochTick() const { return domain_.DomainTick(); }

    static constexpr bool AudioRateMasterIsDevice() { return true; }

private:
    const EmuDomain& domain_;
    bool rtc_fixed_ = true;
};
