#include "engine/vu/vu_float.h"

#include <cmath>

alignas(16) const uint32_t VuFloat::kLaneMask[16][4] = {
    {0u, 0u, 0u, 0u},     {0u, 0u, 0u, ~0u},     {0u, 0u, ~0u, 0u},     {0u, 0u, ~0u, ~0u},
    {0u, ~0u, 0u, 0u},    {0u, ~0u, 0u, ~0u},    {0u, ~0u, ~0u, 0u},    {0u, ~0u, ~0u, ~0u},
    {~0u, 0u, 0u, 0u},    {~0u, 0u, 0u, ~0u},    {~0u, 0u, ~0u, 0u},    {~0u, 0u, ~0u, ~0u},
    {~0u, ~0u, 0u, 0u},   {~0u, ~0u, 0u, ~0u},   {~0u, ~0u, ~0u, 0u},   {~0u, ~0u, ~0u, ~0u},
};

uint16_t VuFloat::PackMacBits(const uint32_t bits[4], uint8_t mask) {
    uint16_t m = 0;
    if (mask & 8) m |= MacFlags1(bits[0], 3);
    if (mask & 4) m |= MacFlags1(bits[1], 2);
    if (mask & 2) m |= MacFlags1(bits[2], 1);
    if (mask & 1) m |= MacFlags1(bits[3], 0);
    return m;
}

