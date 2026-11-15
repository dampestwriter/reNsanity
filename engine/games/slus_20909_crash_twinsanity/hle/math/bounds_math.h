#pragma once

#include "games/slus_20909_crash_twinsanity/hle/math/math_library.h"

namespace title {

class BoundsMath {
public:
    using Vector = MathLibrary::Vector;
    struct Box { Vector minimum, maximum; };
};

}
