#pragma once

#include <cstdint>
#include <string>

namespace ps2r::platform::sdl {

struct PadControl {
    int button = -1;
    int axis = -1;
};

class PadButtonMap {
public:
    static constexpr int kButtons = 16;

    PadButtonMap();

    const PadControl& Control(int bit_index) const { return control_[bit_index & 15]; }
    void Set(int bit_index, const PadControl& c) { control_[bit_index & 15] = c; }

    uint32_t Pressed(const uint8_t* buttons, int num_buttons, int16_t trigger_left, int16_t trigger_right) const;

    static bool ControlFromName(const std::string& name, PadControl& out);
    static std::string ControlName(const PadControl& c);
    static bool Bound(const PadControl& c) { return c.button >= 0 || c.axis >= 0; }


private:
    PadControl control_[kButtons];
};

}
