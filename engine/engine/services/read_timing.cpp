#include "engine/services/read_timing.h"

#include <algorithm>
#include <cmath>

ReadTiming::ReadTiming(const ps2r::CdvdConfig& cfg)
    : mode_(cfg.latency_model), visibility_(cfg.delivery_visibility),
      scale_(cfg.latency_scale), poll_scale_(cfg.poll_latency_scale) {}

uint64_t ReadTiming::DueAtMs(uint64_t tick, double now_ms, uint32_t lba, uint32_t sectors) {
    const uint64_t next = tick + 1;
    position_.last_end_ms = 0;
    if (PollVisibility()) {
        position_.last_end_ms = Charge(now_ms, lba, sectors,
                                       poll_scale_ > 0 ? poll_scale_ : 1.0);
        return std::max(next, static_cast<uint64_t>(std::ceil(position_.last_end_ms / kTickMs)));
    }
    if (mode_ != ps2r::CdvdLatencyModel::Modelled || scale_ <= 0) return next;
    const double end = Charge(now_ms, lba, sectors, scale_);
    return std::max(next, static_cast<uint64_t>(std::ceil(end / kTickMs)));
}
double ReadTiming::Charge(double now_ms, uint32_t lba, uint32_t sectors, double scale) {
    const auto delta = lba >= position_.head_lba ? lba - position_.head_lba : position_.head_lba - lba;
    const double seek = delta <= kContiguousSectors ? 0.0 : delta < kShortSeekSectors ? kShortSeekMs : kLongSeekMs;
    position_.busy_ms = std::max(position_.busy_ms, now_ms) +
                        (seek + static_cast<double>(sectors) / kSectorsPerMs) * scale;
    position_.head_lba = lba + sectors;
    return position_.busy_ms;
}
