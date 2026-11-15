#include "engine/platform/sdl/window.h"

#include <SDL.h>
#include <switch.h>

#include <cstdio>

namespace ps2r {
namespace platform {

Window::~Window() { Destroy(); }

bool Window::Create() {
    Destroy();
    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
            std::fprintf(stderr, "[sdl] SDL_InitSubSystem(VIDEO) failed: %s\n", SDL_GetError());
            return false;
        }
        video_inited_ = true;
    }
    SDL_DisplayMode screen{};
    const bool have_screen = SDL_GetCurrentDisplayMode(0, &screen) == 0 && screen.w > 0 && screen.h > 0;
    width_ = have_screen ? screen.w : 1280;
    height_ = have_screen ? screen.h : 720;
    return true;
}

void Window::Destroy() {
    if (video_inited_) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        video_inited_ = false;
    }
    width_ = height_ = 0;
}

void Window::DrawableSize(int* w, int* h) const {
    const bool docked = appletGetOperationMode() == AppletOperationMode_Console;
    if (w) *w = docked ? 1920 : 1280;
    if (h) *h = docked ? 1080 : 720;
}

}
}
