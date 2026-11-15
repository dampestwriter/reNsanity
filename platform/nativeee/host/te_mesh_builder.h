#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/graphics/mesh_geometry.h"

namespace te::gfx {

using MeshPrepared = title::MeshGeometry::Prepared;
using MeshSink = title::MeshGeometry::Sink;
inline title::MeshGeometry::Result MeshPrepare(const title::MeshGeometry::Mesh& mesh, MeshPrepared& out) {
    return title::MeshGeometry::Prepare(mesh, out);
}
inline title::MeshGeometry::Result MeshBuild(const MeshPrepared& prepared, const title::MeshGeometry::View& view,
                                              const ps2r::gs::DrawIr& state, const std::array<uint8_t, 4>* colours,
                                              MeshSink sink, void* user) {
    return title::MeshGeometry::BuildPrepared(prepared, view, state, colours, sink, user);
}

using MeshOrderSink = title::MeshGeometry::OrderSink;
inline title::MeshGeometry::Result MeshBuild(const MeshPrepared& prepared, const title::MeshGeometry::View& view,
                                              const ps2r::gs::DrawIr& state, const std::array<uint8_t, 4>* colours,
                                              MeshSink sink, MeshOrderSink body, void* user) {
    return title::MeshGeometry::BuildPrepared(prepared, view, state, colours, sink, body, user);
}

}
