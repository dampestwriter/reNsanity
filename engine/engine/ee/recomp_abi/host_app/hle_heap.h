#pragma once

#include <cstdint>

#include "engine/ee/cpu/cpu_state.h"

void HLE_002c5080(CpuContext& ctx);
void HLE_002c5820(CpuContext& ctx);
void HLE_002c5940(CpuContext& ctx);
void HLE_002c6ce0(CpuContext& ctx);
void HLE_002c6d8c(CpuContext& ctx);
void HLE_002c6e88(CpuContext& ctx);
void HLE_002c7070(CpuContext& ctx);
void HLE_002c71bc(CpuContext& ctx);
void HLE_002c7418(CpuContext& ctx);
void HLE_002c75e8(CpuContext& ctx);
void HLE_002ccba0(CpuContext& ctx);
void HLE_002cf930(CpuContext& ctx);

#include <string>
#include <vector>

void HLE_FUN_002aac80(CpuContext& ctx, uint32_t addr);
void HLE_FUN_002ad1a0(CpuContext& ctx, uint32_t addr);
void HLE_FUN_002ad8f0(CpuContext& ctx, uint32_t addr);
void HLE_FUN_002adb18(CpuContext& ctx, uint32_t addr);
void HLE_FUN_002adb40(CpuContext& ctx, uint32_t addr);
