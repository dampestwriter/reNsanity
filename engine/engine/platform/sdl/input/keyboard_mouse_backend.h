#pragma once

#include <cstdint>

#include "engine/platform/sdl/input/input_backend.h"

union SDL_Event;

namespace ps2r::platform::sdl {

class InputMapper;

class KeyboardMouseBackend : public InputBackend {
public:
    static constexpr int kNumScancodes = 512;

    explicit KeyboardMouseBackend(const InputMapper* mapper);

    void Poll(NormalizedInput& out) override;
    bool IsActive() const override { return true; }
    const char* Name() const override { return "kbm"; }

    void HandleEvent(const SDL_Event& e);
    void SetUiCaptured(bool captured);
    void ClearPending();

private:
    const InputMapper* mapper_;
    bool ui_captured_ = false;
    bool focused_ = true;
    uint32_t pending_buttons_ = 0, pending_mouse_ = 0;
    uint8_t inject_[kNumScancodes];
    uint8_t merged_[kNumScancodes];
};

}
