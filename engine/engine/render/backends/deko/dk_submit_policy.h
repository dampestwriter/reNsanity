#pragma once

#include <cstdint>

namespace ps2r::render {

constexpr uint32_t kSubmitMaxDraws = 512;
constexpr uint32_t kSubmitMinDraws = 256;
constexpr uint32_t kSubmitVertices = 32768;
constexpr uint32_t kSubmitStreamBytes = 512 * 1024;
constexpr uint32_t kSubmitPushBytes = 32 * 1024;

inline bool SubmitDue(uint64_t draws, uint64_t vertices, uint64_t stream, uint64_t push) {
    return draws >= kSubmitMaxDraws || (draws >= kSubmitMinDraws &&
           (vertices >= kSubmitVertices || stream >= kSubmitStreamBytes || push >= kSubmitPushBytes));
}

}
