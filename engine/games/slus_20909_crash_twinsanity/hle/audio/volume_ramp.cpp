#include "games/slus_20909_crash_twinsanity/hle/audio/volume_ramp.h"

#include <cstdlib>

namespace title {
namespace {

uint16_t Rate(uint32_t distance) {
    const uint32_t n = distance / 2;
    uint32_t k = 4;
    if (n & 0x2000) k = 0;
    else if (n & 0x1000) k = 1;
    else if (n & 0x0800) k = 2;
    else if (n & 0x0400) k = 3;
    return uint16_t(0x23 + 4 * k);
}
uint32_t Current(const ps2r::sound::spu2::Volume& v) { return uint16_t(int16_t(v.level())); }
}
void VolumeRamp::Target(uint16_t volume, int32_t current_level) {
    state_.target = volume;
    state_.phase = volume >= 0x4000 ? 1 : 0;
    state_.direction = uint32_t(uint16_t(int16_t(current_level))) / 2 < volume ? 1 : -1;
    state_.active = true;
}
void VolumeRamp::Control(ps2r::sound::spu2::Volume& volume) {
    if (!state_.active) return;
    uint32_t c = Current(volume);
    if (state_.phase == 1) {
        state_.phase = 2;
        if (c <= 0x7fff) {
            volume.Write(0xa000);
            state_.direction = int8_t(-state_.direction);
            return;
        }
    } else if (state_.phase == 0 && c > 0x7fff) {
        volume.Write(0xb000);
        return;
    }
    if (state_.phase == 2 && c == 0) c = 0xffff;
    const uint32_t target2 = uint32_t(state_.target) * 2;
    uint32_t distance = c > target2 ? c - target2 : target2 - c;
    if (target2 < c && state_.direction == 1) distance = 0;
    if (c < target2 && state_.direction == -1) distance = 0;
    if (distance <= 0x400) {
        volume.Write(state_.target);
        state_.active = false;
        return;
    }
    const uint16_t rate = Rate(distance);
    if (state_.phase == 0) volume.Write(uint16_t(rate | (c < target2 ? 0x8000 : 0xa000)));
    else volume.Write(uint16_t(rate | (target2 < c ? 0x9000 : 0xb000)));
}
void VolumeRamp::Reset() { state_ = {}; }
}
