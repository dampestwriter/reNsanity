#pragma once
#include <array>
#include <cstdint>

namespace ps2r::render {
struct DkDrawPacketKey {
    uint64_t addr = 0;
    uint32_t size = 0, first = 0, count = 0, layout = 0, changes = 0;
    bool operator==(const DkDrawPacketKey& b) const {
        return addr == b.addr && size == b.size && first == b.first && count == b.count &&
               layout == b.layout && changes == b.changes;
    }
};
struct DkDrawPacketCache {

    static constexpr uint32_t kWords = 96, kEntries = 2048;
    struct Entry { DkDrawPacketKey key; uint32_t count = 0; std::array<uint32_t, kWords> words{}; };
    std::array<Entry, kEntries> entries{};
    Entry& Lookup(const DkDrawPacketKey& k) {
        uint64_t h = (k.addr >> 4) ^ (uint64_t(k.size) << 17) ^ (uint64_t(k.first) << 32) ^
                     (uint64_t(k.count) * 0x9e3779b97f4a7c15ull) ^ (k.layout * 17 + k.changes);
        h ^= h >> 33; h *= 0xff51afd7ed558ccdull; h ^= h >> 33;
        return entries[h & (kEntries - 1)];
    }
};
}
