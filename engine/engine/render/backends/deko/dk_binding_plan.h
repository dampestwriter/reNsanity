#pragma once

#include "engine/render/gs_specialize.h"

namespace ps2r {
namespace render {

inline int DkPs2VsVariant(int native, int uv, int lit) {
    if (native == 0 && uv == 0) return 0;
    if (native == 2 && (uv == 2 || uv == 3)) return uv == 2 ? 1 : 2;
    if (native == 1 && (uv == 2 || uv == 3) && (lit == 0 || lit == 1))
        return 3 + 2 * lit + (uv == 3 ? 1 : 0);
    if (native == 3 && (uv == 2 || uv == 3) && lit == 1) return 7 + (uv == 3 ? 1 : 0);
    return -1;
}

inline uint32_t DkPs2SamplerMask(const GsSpecialize::Key& key) {
    using F = GsSpecialize;
    const auto v = [&](int f) { return F::Value(key, f); };
    if (v(F::kDate) != 0 || v(F::kFbmsk16En) != 0 || v(F::kZsrcEn) != 0 ||
        (v(F::kBlendEnabled) != 0 && v(F::kSwBlend) != 0)) return 0x1f;
    if (v(F::kUseTexture) == 0) return 0;
    if (v(F::kUseTexture) != 1) return 0x1f;
    if (v(F::kIdxFmt) == 0) return 1u;
    if (v(F::kIdxFmt) == 8) return (1u << 0) | (1u << 2);
    return 0x1f;
}

template<class Emit>
inline void DkBindChangedTextures(uint32_t live, uint32_t* applied, const uint32_t* want, Emit emit) {
    for (unsigned i = 0; i < 5;) {
        if (!(live & (1u << i))) { ++i; continue; }
        unsigned first = 5, last = 0;
        do {
            if (applied[i] != want[i]) { if (first == 5) first = i; last = i; }
            ++i;
        } while (i < 5 && (live & (1u << i)));
        if (first == 5) continue;
        emit(first, last - first + 1);
        for (unsigned j = first; j <= last; ++j) applied[j] = want[j];
    }
}

}
}
