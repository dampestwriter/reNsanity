#pragma once

#include <cstdint>

struct Vu1FamilySignature {
    uint32_t variant_base;
    uint32_t dispatcher_op0;
    uint16_t exit_stubs[2];
    uint32_t family_exit_tpc;
};
