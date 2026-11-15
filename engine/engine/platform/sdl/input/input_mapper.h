#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "engine/platform/sdl/input/pad_button_map.h"

namespace ps2r::platform::sdl {

struct NormalizedInput;
struct PadSample;

struct Bindings {
    std::vector<std::pair<int, uint32_t>> key_buttons;
    std::vector<std::pair<int, uint32_t>> mouse_buttons;
    int   key_left_up = 0, key_left_down = 0, key_left_left = 0, key_left_right = 0;
    bool  mouse_to_right_stick = true;
    float mouse_sensitivity    = 0.35f;
    float mouse_decay          = 12.0f;
    bool  gamepad_enabled      = true;
    float gamepad_deadzone     = 0.12f;
    float gamepad_sensitivity  = 1.33f;
    PadButtonMap pad_map;
};

uint8_t StickToByte(float v);

int ScancodeFromName(const std::string& name);

int ParseMouseName(const std::string& name);

class InputMapper {
public:
    InputMapper();

    bool LoadConfig(const std::string& path);

    bool LoadConfigText(const std::string& text, const char* origin);

    uint32_t ButtonsFromKeys(const uint8_t* keystate, int numkeys) const;

    void KeyStickFromKeys(const uint8_t* keystate, int numkeys, float& lx, float& ly) const;

    uint32_t LogicalForScancode(int scancode) const;

    PadSample Map(const NormalizedInput& in, float dt_seconds);

    void ResetMouseAccum();
    const Bindings& Config() const { return b_; }

private:
    Bindings b_;
    float accum_x_ = 0.f, accum_y_ = 0.f;
};

}
