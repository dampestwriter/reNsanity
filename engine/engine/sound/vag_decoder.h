#pragma once

#include <cstdint>

class VagDecoder {
public:
    static constexpr int kBlockBytes = 16;
    static constexpr int kBlockSamples = 28;
    static constexpr uint8_t kFlagLoopEnd = 0x01;
    static constexpr uint8_t kFlagLoopRepeat = 0x02;
    static constexpr uint8_t kFlagLoopStart = 0x04;
    static constexpr uint32_t kMaxShift = 12;
    static constexpr uint32_t kFilterCount = 5;

    struct History {
        int32_t h1 = 0;
        int32_t h2 = 0;
    };

    static uint8_t DecodeBlock(const uint8_t block[kBlockBytes], History& hist, int16_t out[kBlockSamples]);
};
