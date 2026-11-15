#pragma once

#include "games/slus_20909_crash_twinsanity/hle/math/culling_math.h"

namespace title {

class CullingPlanes {
public:
    using Planes = std::array<CullingMath::Vector, 6>;
    static CullingMath::Vector Transform(
        const CullingMath::Vector& plane, const CullingMath::Matrix& place);
    static CullingMath::Frustum InChunk(
        const Planes& view, const Planes& far_view, const CullingMath::Matrix& place);

    static CullingMath::Frustum ThroughPortal(
        const Planes& view, const Planes& far_view,
        const CullingMath::Matrix& place, const Planes& portal);

private:
    static void Column(CullingMath::PlaneColumns& columns,
                       const CullingMath::Vector& plane, size_t column);
    static CullingMath::PlaneColumns Sides(
        const Planes& planes, const CullingMath::Matrix* place);
};

}
