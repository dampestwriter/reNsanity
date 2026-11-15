#pragma once

#include "engine/vu/hle/vu1_hle_contract.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/library_common.h"

class ZeroKick {
public:
    static bool RunA(const Vu1HleInput& in, Vu1HleOutput& out);
    static bool RunC(const Vu1HleInput& in, Vu1HleOutput& out);

    static bool RunCContForDraw(const Vu1HleInput& in, Vu1HleOutput& out);
    static bool RunAContForDraw(const Vu1HleInput& in, Vu1HleOutput& out);

private:
    template <bool full_state>
    static bool RunCContImpl(const Vu1HleInput& in, Vu1HleOutput& out);
    template <bool full_state>
    static bool RunAContImpl(const Vu1HleInput& in, Vu1HleOutput& out);

    static bool RunStub(const Vu1HleInput& in, Vu1HleOutput& out, int cls);
};
