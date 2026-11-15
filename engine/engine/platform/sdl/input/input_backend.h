#pragma once

#include <cstdint>

namespace ps2r::platform::sdl {

struct NormalizedInput {
    float    lx = 0.f, ly = 0.f;
    float    rx = 0.f, ry = 0.f;
    uint32_t buttons = 0;
    float    mouse_dx = 0.f;
    float    mouse_dy = 0.f;
    bool     mouse_buttons[3] = {};
};

enum LogicalButton : uint32_t {
    kBtnSelect   = 1u << 0,  kBtnL3     = 1u << 1,
    kBtnR3       = 1u << 2,  kBtnStart  = 1u << 3,
    kBtnUp       = 1u << 4,  kBtnRight  = 1u << 5,
    kBtnDown     = 1u << 6,  kBtnLeft   = 1u << 7,
    kBtnL2       = 1u << 8,  kBtnR2     = 1u << 9,
    kBtnL1       = 1u << 10, kBtnR1     = 1u << 11,
    kBtnTriangle = 1u << 12, kBtnCircle = 1u << 13,
    kBtnCross    = 1u << 14, kBtnSquare = 1u << 15,
};

class InputBackend {
public:
    virtual ~InputBackend() = default;

    virtual void Poll(NormalizedInput& out) = 0;

    virtual bool IsActive() const = 0;
    virtual const char* Name() const = 0;
};

}
