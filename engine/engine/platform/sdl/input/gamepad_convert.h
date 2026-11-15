#pragma once

#include <cstdint>

#include "engine/platform/sdl/input/pad_button_map.h"

namespace ps2r::platform::sdl {

struct NormalizedInput;

struct GamepadRaw {
    static constexpr int kAxes = 6;
    static constexpr int kButtons = 21;
    int16_t axis[kAxes] = {};
    uint8_t button[kButtons] = {};
};

class GamepadConvert {
public:

    static void Convert(const GamepadRaw& raw, float deadzone, NormalizedInput& out,
                        float sensitivity = 1.33f, const PadButtonMap& map = PadButtonMap());

    static void Stick(int16_t rawx, int16_t rawy, float deadzone, float sensitivity,
                      float& outx, float& outy);
};

}
