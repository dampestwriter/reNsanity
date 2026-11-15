#pragma once

#include <string>

namespace ps2r::platform::sdl {

struct InputSettings {
    std::string bindings_path;
    std::string device_config_path;
    std::string gamecontrollerdb_path;
    bool  kbm_enabled         = true;
    bool  gamepad_enabled     = true;
    float gamepad_deadzone    = 0.12f;
    float gamepad_sensitivity = 1.33f;
};

}
