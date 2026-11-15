#pragma once

#include <cstdint>

#include "engine/platform/platform.h"

namespace ps2r::platform::sdl {

struct PadSample {
    uint16_t buttons = 0;
    uint8_t  lx = 0x80, ly = 0x80;
    uint8_t  rx = 0x80, ry = 0x80;

};

platform::InputState ToInputState(const PadSample& s);
PadSample            FromInputState(const platform::InputState& w);

}
