#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "engine/platform/sdl/input/input_mapper.h"
#include "engine/platform/sdl/input/input_settings.h"
#include "engine/platform/sdl/input/pad_sample.h"

union SDL_Event;

namespace ps2r::platform::sdl {

class KeyboardMouseBackend;
class GamepadBackend;

class InputSystem {
public:
    InputSystem();
    InputSystem(const InputSystem&) = delete;
    InputSystem& operator=(const InputSystem&) = delete;

    void Init(const InputSettings& settings);

    void HandleEvent(const SDL_Event& e);

    PadSample Frame(float dt_seconds);

    const PadSample& LastSample() const { return last_; }

    bool Connected() const;
    bool UiCaptured() const { return ui_capture_; }
    void SetKbmEnabled(bool enabled);
    bool KbmEnabled() const { return kbm_enabled_; }

    bool LoadDeviceConfig(const std::string& path);

    InputMapper&          Mapper() { return mapper_; }
    const InputMapper&    Mapper() const { return mapper_; }
    GamepadBackend*       Gamepad() { return gamepad_.get(); }
    KeyboardMouseBackend* Kbm() { return kbm_.get(); }

    enum class Source { None, Kbm, Gamepad };
    Source ActiveSource() const { return active_; }
    std::string Sources() const;

private:
    void NoteSource(Source source);
    Source active_ = Source::None;
    InputSettings settings_;
    InputMapper   mapper_;
    std::unique_ptr<KeyboardMouseBackend> kbm_;
    std::unique_ptr<GamepadBackend>       gamepad_;
    PadSample last_;
    bool ui_capture_ = false;
    bool kbm_enabled_ = true;
    bool initialized_ = false;
};

}
