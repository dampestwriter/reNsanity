#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace ps2r {
namespace render {

struct DkUniformSnapshot {
    uint64_t generation = 0;
    uint32_t offset = 0;
    std::array<uint8_t, 512> bytes{};
};

struct DkUniformRing {
    static constexpr uint32_t kAlignment = 256, kMaxBinding = 512;

    static constexpr uint32_t kOperationReserve = 2 * kMaxBinding;
    static constexpr uint32_t kInvalid = ~0u;
    uint64_t generation = 0;
    uint32_t used = 0;

    struct InternEntry { DkUniformSnapshot snapshot; uint32_t size = 0, bound = 0; uint64_t hash = 0; };
    static constexpr uint32_t kInternEntries = 2048;
    std::unique_ptr<InternEntry[]> intern;
    bool intern_on = true;
    bool zero_padding = true;
    void EnableIntern() { intern = std::make_unique<InternEntry[]>(kInternEntries); }
    static uint64_t Hash(const uint8_t* bytes, uint32_t size) {
        uint64_t h = 0x9e3779b97f4a7c15ull;
        for (uint32_t off = 0; off < size; off += 8) {
            uint64_t word = 0;
            std::memcpy(&word, bytes + off, size - off < 8 ? size - off : 8);
            h ^= word + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        }
        return h;
    }

    void BeginAfterFence() {
        if (++generation == 0) std::abort();
        used = 0;
    }
    bool Fits(uint32_t bytes, uint32_t capacity) const {
        return used <= capacity && bytes <= capacity - used;
    }

    static bool SameChunks(const uint8_t* a, const uint8_t* b, uint32_t size, uint32_t chunks) {
        for (; chunks; chunks &= chunks - 1) {
            const uint32_t off = uint32_t(__builtin_ctz(chunks)) * 16u;
            if (off >= size) break;
            const uint32_t n = size - off < 16u ? size - off : 16u;
            if (std::memcmp(a + off, b + off, n) != 0) return false;
        }
        return true;
    }
    uint32_t ResolveLean(DkUniformSnapshot& last, const uint8_t* shadow, uint32_t size, uint32_t bound,
                         uint32_t dirty_chunks, uint8_t* cpu, uint32_t capacity, bool* written) {
        *written = false;
        if (!generation || !size || size > kMaxBinding || bound < size || bound > kMaxBinding || bound % kAlignment)
            return kInvalid;
        if (last.generation == generation && (!dirty_chunks || SameChunks(cpu + last.offset, shadow, size, dirty_chunks))) {
            return last.offset;
        }
        if (!Fits(bound, capacity)) return kInvalid;
        const uint32_t off = used;
        std::memcpy(cpu + off, shadow, size);
        if (zero_padding) std::memset(cpu + off + size, 0, bound - size);
        last.generation = generation;
        last.offset = off;
        used += bound;
        *written = true;
        return off;
    }

    uint32_t Resolve(DkUniformSnapshot& last, const uint8_t* shadow, uint32_t size, uint32_t bound,
                     bool dirty, uint8_t* cpu, uint32_t capacity, bool* written) {
        *written = false;
        if (!generation || !size || size > kMaxBinding || bound < size || bound > kMaxBinding || bound % kAlignment)
            return kInvalid;
        if (last.generation == generation && (!dirty || std::memcmp(last.bytes.data(), shadow, size) == 0))
            return last.offset;
        InternEntry* vacant = nullptr;
        uint64_t hash = 0;
        if (intern && intern_on) {
            hash = Hash(shadow, size);
            for (uint32_t probe = 0; probe < 4; ++probe) {
                auto& e = intern[(uint32_t(hash) + probe) & (kInternEntries - 1)];
                if (e.snapshot.generation != generation) { if (!vacant) vacant = &e; continue; }
                if (e.hash == hash && e.size == size && e.bound == bound &&
                    std::memcmp(e.snapshot.bytes.data(), shadow, size) == 0) {
                    last = e.snapshot;
                    return last.offset;
                }
            }
        }
        if (!Fits(bound, capacity)) return kInvalid;
        const uint32_t off = used;
        std::memcpy(cpu + off, shadow, size);
        if (zero_padding) std::memset(cpu + off + size, 0, bound - size);
        std::memcpy(last.bytes.data(), shadow, size);
        last.generation = generation;
        last.offset = off;
        used += bound;
        if (vacant) {
            vacant->hash = hash; vacant->size = size; vacant->bound = bound;
            vacant->snapshot = last;
        }
        *written = true;
        return off;
    }
};

}
}
