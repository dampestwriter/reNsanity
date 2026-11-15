#pragma once

#include <memory>

#include "engine/render/backend.h"

namespace ps2r {
namespace platform {
class Window;
}

namespace ps2e {

std::unique_ptr<render::Backend> CreateDrawBackend(platform::Window& window);

void DrawBackendSwapInterval(render::Backend& device, int interval);

}
}
