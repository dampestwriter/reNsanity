#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "engine/gs/hle/targets/vram_layout.h"

namespace ps2r {
namespace gs {

class ClutHealCache {
public:
    static constexpr int kGoodAlphaMin = 100;
    static constexpr int kGoodEntriesMin = 200;

    bool Heal(uint32_t cbp, uint32_t* palette, const VramLayout& layout);
    bool HasCached(uint32_t cbp) const { return cache_.count(cbp) != 0; }
    void Clear() { cache_.clear(); }

private:
    std::unordered_map<uint32_t, std::vector<uint32_t>> cache_;
};

}
}
