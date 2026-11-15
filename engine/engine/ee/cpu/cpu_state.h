#pragma once

#include "engine/core/types.h"

#include <cstddef>
#include <cstring>

union alignas(16) GPR_reg {
    u128 UQ;
    s128 SQ;
    u64 UD[2];
    s64 SD[2];
    u32 UL[4];
    s32 SL[4];
    u16 US[8];
    s16 SS[8];
    u8 UC[16];
    s8 SC[16];
};

union alignas(16) VR_reg {
    struct {
        float x, y, z, w;
    };
    float F[4];
    u32 I[4];
    s128 Q;
};

union GPRregs {
    GPR_reg r[32];
};

union CP0regs {
    struct {
        u32 Index, Random, EntryLo0, EntryLo1, Context, PageMask, Wired, Reserved0, BadVAddr, Count, EntryHi,
            Compare, Status, Cause, EPC, PRid, Config, LLAddr, WatchLO, WatchHI, XContext, Reserved1, Reserved2,
            Debug, DEPC, PerfCnt, ErrCtl, CacheErr, TagLo, TagHi, ErrorEPC, DESAVE;
    } n;
    u32 r[32];
};

union FPRreg {
    float f;
    u32 UL;
    s32 SL;
};

struct FPUregs {
    FPRreg fpr[32];
    u32 fprc[32];
    FPRreg ACC;
    u32 ACCflag;
};

struct VUregs {
    VR_reg VF[32];
    u16 VI[16];
    VR_reg ACC;
    float Q;
    float P;
    u32 I;
    u32 R;
    u16 status_flag;
    u16 mac_flag;
    u32 clip_flag;
    u32 TPC;
    u32 CMSAR0;
    u32 FBRST;
    u32 VPU_STAT;
    u32 CMSAR1;

    s32 ReadControlReg(int reg) const {
        if (reg == 0) return 0;
        if (reg >= 1 && reg <= 15) return static_cast<s32>(static_cast<u32>(VI[reg]));
        switch (reg) {
            case 16: return static_cast<s32>(status_flag);
            case 17: return static_cast<s32>(mac_flag);
            case 18: return static_cast<s32>(clip_flag);
            case 20: return static_cast<s32>(R);
            case 21: return static_cast<s32>(I);
            case 22: {
                u32 bits;
                std::memcpy(&bits, &Q, sizeof(bits));
                return static_cast<s32>(bits);
            }
            case 26: return static_cast<s32>(TPC);
            case 27: return static_cast<s32>(CMSAR0);
            case 28: return static_cast<s32>(FBRST);
            case 29: return static_cast<s32>(VPU_STAT);
            case 31: return static_cast<s32>(CMSAR1);
            default: return 0;
        }
    }

    void WriteControlReg(int reg, u32 value) {
        if (reg >= 1 && reg <= 15) {
            VI[reg] = static_cast<u16>(value);
            return;
        }
        switch (reg) {
            case 16: status_flag = static_cast<u16>((status_flag & 0x03Fu) | (value & 0xFC0u)); break;
            case 17: mac_flag = static_cast<u16>(value); break;
            case 18: clip_flag = value & 0xFFFFFFu; break;
            case 20: R = value & 0x7FFFFFu; break;
            case 21: I = value; break;
            case 22: std::memcpy(&Q, &value, sizeof(Q)); break;
            case 26: TPC = value & 0xFFFFu; break;
            case 27: CMSAR0 = value & 0xFFFFu; break;
            case 28: FBRST = value & 0x0C0Cu; break;
            case 31: CMSAR1 = value & 0xFFFFu; break;
            default: break;
        }
    }

    void Reset() {
        std::memset(this, 0, sizeof(VUregs));
        VF[0].w = 1.0f;
        Q = 1.0f;
    }
};

struct CPURegs {
    GPRregs GPR;
    GPR_reg HI;
    GPR_reg LO;
    GPR_reg HI1;
    GPR_reg LO1;
    CP0regs CP0;
    u32 sa;
    u32 pc;
};

struct EmotionEngineState {
    CPURegs cpuRegs;
    FPUregs fpuRegs;
    VUregs vuRegs;
};

typedef EmotionEngineState CpuContext;

static_assert(sizeof(GPR_reg) == 16, "GPR_reg is one quadword");
static_assert(sizeof(VR_reg) == 16, "VR_reg is one quadword");
static_assert(sizeof(CPURegs) == 720, "CPURegs: 32x16 GPR + 4x16 HI/LO + 128 CP0 + sa + pc, 16-aligned");
static_assert(sizeof(FPUregs) == 264, "FPUregs: 128 fpr + 128 fprc + ACC + ACCflag");
static_assert(sizeof(VUregs) == 608, "VUregs: 512 VF + 32 VI + 16 ACC + 44 scalars, 16-aligned");
static_assert(offsetof(EmotionEngineState, fpuRegs) == 720, "fpuRegs offset");
static_assert(offsetof(EmotionEngineState, vuRegs) == 992, "vuRegs offset");
static_assert(sizeof(EmotionEngineState) == 1600, "CpuContext size == anchor THRD ctx_size");
