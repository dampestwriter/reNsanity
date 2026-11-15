#pragma once

#include <cstdint>

#include "engine/sound/spu2_dsp.h"

namespace title {

class VolumeRamp {
public:
    struct State {
        uint16_t target = 0;
        int8_t direction = 0;
        uint8_t phase = 0;
        bool active = false;
    };

    void Target(uint16_t volume, int32_t current_level);

    void Control(ps2r::sound::spu2::Volume& volume);
    void Reset();

private:
    State state_;
};

}
