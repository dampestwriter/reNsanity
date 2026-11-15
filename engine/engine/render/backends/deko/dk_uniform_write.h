#pragma once

#include <cstdint>
#include <cstring>

namespace ps2r::render {

inline uint32_t DkUniformChunks(uint32_t off, uint32_t n) {
    const uint32_t first = off >> 4, last = (off + n - 1) >> 4;
    return (~0u << first) & (~0u >> (31u - last));
}

template<uint32_t N, class Program>
inline void DkSetUniform(Program& p, uint32_t off, const void* v) {
    static_assert(N > 0 && N <= 512, "uniform write size");
    uint8_t* dst = p.shadow.data() + off;
    if (std::memcmp(dst, v, N) == 0) return;
    std::memcpy(dst, v, N);
    const uint32_t mask = DkUniformChunks(off, N);
    p.dirty |= mask;
    p.key_dirty |= mask;
}
}
