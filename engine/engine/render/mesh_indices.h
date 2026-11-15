#pragma once
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include "engine/render/backend.h"

namespace ps2r { namespace render {
inline std::vector<uint32_t> MeshIndices(const MeshVertex* v, const SkinBinding* b, size_t count) {
    if (count > std::numeric_limits<uint32_t>::max()) throw std::overflow_error("mesh index count");
    std::unordered_map<std::string, uint32_t> first;
    first.reserve(count);
    std::vector<uint32_t> indices(count);
    std::string key(sizeof(MeshVertex) + (b ? sizeof(SkinBinding) : 0), '\0');
    for (size_t i = 0; i < count; ++i) {
        std::memcpy(&key[0], v + i, sizeof(MeshVertex));
        if (b) std::memcpy(&key[sizeof(MeshVertex)], b + i, sizeof(SkinBinding));
        indices[i] = first.emplace(key, uint32_t(i)).first->second;
    }
    return indices;
}
} }
