#include <cstdio>

#include "te_host.h"
#include "te_input.h"
#include "te_services.h"

#include <SDL.h>

#include "engine/platform/sdl/input/input_system.h"
#include "engine/platform/sdl/input/keyboard_mouse_backend.h"

namespace te::input {

namespace {
ps2r::platform::sdl::InputSystem* g_input = nullptr;
bool g_installed = false;
}

bool Install(Pads& pads, const Settings& settings) {
    if (g_installed) return true;
    if (!settings.sdl_input) return false;
    if (!SDL_WasInit(SDL_INIT_EVENTS)) {
        if (SDL_InitSubSystem(SDL_INIT_EVENTS) != 0) return false;
    }
    g_input = new ps2r::platform::sdl::InputSystem;
    ps2r::platform::sdl::InputSettings in;
    in.kbm_enabled = settings.kbm;
    in.gamepad_enabled = true;
    g_input->Init(in);
    const float dt = 1.0f / 60.0f;
    pads.SetSampler([dt](unsigned port, unsigned slot) -> Pads::Sample {
        Pads::Sample s;
        if (port != 0 || slot != 0) return s;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {

            if (e.type == SDL_QUIT || (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE)) {
                        te::RequestExit(0);
            }
            g_input->HandleEvent(e);
        }
        const ps2r::platform::sdl::PadSample p = g_input->Frame(dt);
        s.connected = g_input->Connected() || te::GetSettings().pad_connected;
        s.buttons = p.buttons;
        s.lx = p.lx; s.ly = p.ly; s.rx = p.rx; s.ry = p.ry;
        return s;
    });
    g_installed = true;
    return true;
}

}
