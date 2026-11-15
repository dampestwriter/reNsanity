#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "engine/platform/sdl/input/input_backend.h"
#include "engine/platform/sdl/input/pad_button_map.h"

union SDL_Event;

namespace ps2r::platform::sdl {

struct PadInfo {
    int         instance_id = -1;
    std::string name;
    std::string guid;
    bool        generic_mapping = false;
};

class GamepadBackend : public InputBackend {
public:
    static constexpr int kSelectionAuto = -1;
    static constexpr int kSelectionNone = -2;
    static constexpr float kMinDeadzone = 0.f, kMaxDeadzone = 0.9f;
    static constexpr float kMinSensitivity = 0.5f, kMaxSensitivity = 2.0f;

    GamepadBackend(float deadzone, float sensitivity, bool enabled, const std::string& gamecontrollerdb_path);
    ~GamepadBackend() override;
    GamepadBackend(const GamepadBackend&) = delete;
    GamepadBackend& operator=(const GamepadBackend&) = delete;

    void Poll(NormalizedInput& out) override;
    bool IsActive() const override;
    const char* Name() const override { return "gamepad"; }

    void HandleEvent(const SDL_Event& e);
    void SetUiCaptured(bool captured);
    void ClearPending();

    const std::vector<PadInfo>& Snapshot() const { return snapshot_; }

    void SetSelection(int selection);
    int  Selection() const { return selection_; }

    void SetPreferredGuid(const std::string& guid) { preferred_guid_ = guid; }
    const std::string& PreferredGuid() const { return preferred_guid_; }
    void  SetDeadzone(float dz);
    float Deadzone() const { return deadzone_; }
    void  SetSensitivity(float s);
    float Sensitivity() const { return sensitivity_; }
    bool Enabled() const { return enabled_; }

    void SetButtonMap(const PadButtonMap& map) { map_ = map; }
    const PadButtonMap& ButtonMap() const { return map_; }

private:
    struct Pad {
        void* controller = nullptr;
        int   instance_id = -1;
        bool  generic_mapping = false;
        uint32_t pending_buttons = 0;
    };

    void TryOpenDeviceIndex(int device_index);
    void CloseInstance(int instance_id);
    void PublishSnapshot();

    std::vector<Pad>     pads_;
    std::vector<PadInfo> snapshot_;
    std::string          preferred_guid_;
    int   selection_ = kSelectionAuto;
    float deadzone_ = 0.12f;
    float sensitivity_ = 1.33f;
    PadButtonMap map_;
    bool  enabled_ = true;
    bool  subsystem_ok_ = false;
    bool  ui_captured_ = false;
};

}
