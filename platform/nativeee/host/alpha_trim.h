#pragma once

#include <cstdint>
#include <vector>

#include "engine/platform/ps2e/ps2e_draw.h"
#include "gs_texture.h"

namespace te::alphatrim {

bool Applies(const ps2e_draw_state& state);

size_t FailingTriangles(const std::vector<ps2e_draw_mesh_vertex>& triangles, const te::gfx::DecodedTexture& texture,
                        const ps2e_draw_state& state, std::vector<uint8_t>& failing);
}
