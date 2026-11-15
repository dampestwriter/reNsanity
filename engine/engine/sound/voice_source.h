#pragma once

#include <array>
#include <cstdint>

namespace ps2r::sound {

struct EffectSettings {
    EffectSettings() { send.fill(true); }
    struct Core {
        int32_t mode = 0;
        uint16_t delay = 0, feedback = 0;
        uint16_t volume_left = 0, volume_right = 0;
        bool enabled = false;
        uint32_t generation = 0;
    };
    std::array<Core, 2> cores{};
    std::array<bool, 48> send{};
    std::array<uint32_t, 2> shadow{};
};

class VoiceSource {
public:
    virtual ~VoiceSource() = default;
    virtual void BeginMixTick() = 0;
    virtual bool StepVoice(uint32_t channel, float& left, float& right) = 0;

    virtual int StepChannel(uint32_t, int, float*, float*, bool*) { return -1; }

    virtual uint32_t ChannelGroup(uint32_t) const { return 0; }

    virtual const EffectSettings* Effects() const { return nullptr; }
};

}
