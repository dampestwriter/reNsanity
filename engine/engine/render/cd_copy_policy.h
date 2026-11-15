#pragma once

#include <cstdint>

#include "engine/render/backend.h"

namespace ps2r {
namespace render {

class CdCopyPolicy {
public:

    static bool Decide(CdCopyState& st, int rx0, int ry0, int rw, int rh, bool force_full,
                       int tw, int th, int out[4]);

    static void Invalidate(CdCopyState& st);
    static bool DirtyEmpty(const CdCopyState& st);
    static bool RectsIntersect(int ax0, int ay0, int ax1, int ay1, int bx0, int by0, int bx1,
                               int by1);
};

}
}
