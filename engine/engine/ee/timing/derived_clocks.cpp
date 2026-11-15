#include "engine/ee/timing/derived_clocks.h"

uint64_t DerivedClocks::TimerCounts(uint32_t clks) const {
    return domain_.DomainTick() * kCountsPerTick[clks & 3u];
}

DerivedClocks::RtcTime DerivedClocks::EpochToCivil(uint64_t seconds) {
    const uint64_t days = seconds / 86400u;
    const uint32_t sod = static_cast<uint32_t>(seconds % 86400u);

    const int64_t z = static_cast<int64_t>(days) + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const uint32_t doe = static_cast<uint32_t>(z - era * 146097);
    const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t y = static_cast<int64_t>(yoe) + era * 400;
    const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const uint32_t mp = (5 * doy + 2) / 153;
    const uint32_t d = doy - (153 * mp + 2) / 5 + 1;
    const uint32_t m = mp < 10 ? mp + 3 : mp - 9;
    RtcTime t;
    t.year = static_cast<int>(y + (m <= 2 ? 1 : 0));
    t.month = static_cast<int>(m);
    t.day = static_cast<int>(d);
    t.hour = static_cast<int>(sod / 3600u);
    t.minute = static_cast<int>((sod / 60u) % 60u);
    t.second = static_cast<int>(sod % 60u);
    return t;
}

