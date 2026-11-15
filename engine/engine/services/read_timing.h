#pragma once

#include <cstdint>

#include "engine/core/config/config.h"

class ReadTiming {
public:
    static constexpr double kTickMs = 16667.0 / 1000.0;
    static constexpr uint32_t kContiguousSectors = 16, kShortSeekSectors = 14764;
    static constexpr double kShortSeekMs = 42.0, kLongSeekMs = 112.0, kSectorsPerMs = 1.08;
    struct Position {
        double busy_ms = 0, last_end_ms = 0;
        uint32_t head_lba = 0;
    };

    explicit ReadTiming(const ps2r::CdvdConfig& cfg);

    uint64_t DueAtMs(uint64_t tick, double now_ms, uint32_t lba, uint32_t sectors);
    double LastEndMs() const { return position_.last_end_ms; }
    bool PollVisibility() const { return visibility_ == ps2r::CdvdDeliveryVisibility::Poll; }

private:
    double Charge(double now_ms, uint32_t lba, uint32_t sectors, double scale);
    ps2r::CdvdLatencyModel mode_;
    ps2r::CdvdDeliveryVisibility visibility_;
    double scale_, poll_scale_;
    Position position_;
};
