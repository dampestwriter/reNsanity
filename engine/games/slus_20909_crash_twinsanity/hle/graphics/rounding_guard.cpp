#include "games/slus_20909_crash_twinsanity/hle/graphics/rounding_guard.h"

#include <cfenv>

namespace title {

RoundingGuard::RoundingGuard() noexcept : previous_(std::fegetround()) {
    if (previous_ != FE_TOWARDZERO) std::fesetround(FE_TOWARDZERO);
}

RoundingGuard::~RoundingGuard() noexcept {
    if (previous_ != FE_TOWARDZERO) std::fesetround(previous_);
}

}
