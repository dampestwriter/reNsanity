#include "engine/ee/timing/boundary_protocol.h"

#include <chrono>

BoundaryProtocol::BoundaryProtocol(TickSource& source, TickSync& ticks) : source_(source), ticks_(ticks) {}

bool BoundaryProtocol::ParkForPacerTick() {
    const uint32_t period_us = source_.PeriodUs() ? source_.PeriodUs() : TickSource::kBasePeriodUs;
    if (ticks_.Pending() != 0) return false;
    std::unique_lock<std::mutex> lk(mu_);
    const uint64_t seen = tick_generation_;
    cv_.wait_for(lk, std::chrono::microseconds(period_us) * kBoundaryParkPeriods,
                 [&] { return tick_generation_ != seen || ticks_.Pending() != 0; });
    return true;
}

void BoundaryProtocol::NotifyTick() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        ++tick_generation_;
    }
    cv_.notify_all();
}
