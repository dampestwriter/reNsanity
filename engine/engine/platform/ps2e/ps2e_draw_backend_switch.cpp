#include "engine/platform/ps2e/ps2e_draw_backend.h"

#include <cstdio>
#include <memory>

#include "engine/platform/sdl/window.h"
#include "engine/render/backends/deko/dk_device.h"

namespace ps2r {
namespace ps2e {

void DrawBackendSwapInterval(render::Backend& device, int interval) {
    static_cast<render::DekoDevice&>(device).SetSwapInterval(interval);
}

std::unique_ptr<render::Backend> CreateDrawBackend(platform::Window& window) {
    auto device = std::make_unique<render::DekoDevice>();
    if (!device->Init(window.Width(), window.Height())) {
        std::fprintf(stderr, "[ps2e_draw] deko3d failed to start\n");
        return nullptr;
    }
    device->SetSwapInterval(0);
    return device;
}

}
}
