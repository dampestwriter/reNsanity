#pragma once

#include <cstdint>

#include "engine/ee/cpu/cpu_state.h"

class Vu0Macro {
public:
    void Execute(CpuContext& ctx, uint32_t start_addr);
    void ResumeIfWaiting(CpuContext& ctx);
};

class Vu1Window {
public:
    void Execute(CpuContext& ctx, uint32_t start_addr);
};

extern Vu0Macro g_vu0;
extern Vu1Window g_vu1;
