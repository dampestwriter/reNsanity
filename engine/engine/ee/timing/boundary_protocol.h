#pragma once

#include <condition_variable>
#include <cstdint>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "engine/ee/timing/tick_source.h"
#include "engine/ee/timing/tick_sync.h"

class BoundaryProtocol {
public:
    static constexpr uint32_t kRunaheadDefault = 2;
    static constexpr uint32_t kRunaheadMin = 1;
    static constexpr uint32_t kRunaheadMax = 4;

    static bool ShouldPark(uint32_t declared, uint32_t processed, uint32_t lag) { return declared > processed + lag; }

    static uint64_t Depth(uint32_t declared, uint32_t processed) {
        return declared > processed ? static_cast<uint64_t>(declared - processed) : 0u;
    }

    BoundaryProtocol(TickSource& source, TickSync& ticks);
    uint32_t Runahead() const { return runahead_; }
    bool ShouldPark(uint32_t declared, uint32_t processed) const { return ShouldPark(declared, processed, runahead_); }


    void NotifyTick();

    bool ParkForPacerTick();
    static constexpr uint32_t kBoundaryParkPeriods = 2;



private:
    TickSource& source_;
    TickSync& ticks_;
    uint32_t runahead_ = kRunaheadDefault;
    std::mutex mu_;
    std::condition_variable cv_;
    uint64_t tick_generation_ = 0;
};
