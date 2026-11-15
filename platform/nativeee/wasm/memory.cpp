#include "platform/memory.h"
#include "debug.h"
#include "retail/libc.h"
#include <kernel.h>

extern "C" unsigned char __heap_base[];

namespace
{
constexpr u32 MemorySize = 0x2000000;

constexpr u32 HeapReserve = 0x2800;

constexpr u32 FixedPoolStart = 0x3F6480;

u32 PoolStart()
{
    u32 now = reinterpret_cast<u32>(RetailLibc::Sbrk(0));
    return now < FixedPoolStart ? FixedPoolStart : now;
}
}

extern "C" void* EndOfHeap(void)
{
    return reinterpret_cast<void*>(MemorySize);
}

u32 Platform::Memory::PoolSpace()
{
    return MemorySize - (PoolStart() + HeapReserve);
}

void* Platform::Memory::AllocatePool(u32 size)
{
    static bool moved;
    if (!moved)
    {
        moved = true;
        u32 now = reinterpret_cast<u32>(RetailLibc::Sbrk(0));
        if (now < FixedPoolStart)
        {
            RetailLibc::Sbrk(static_cast<s32>(FixedPoolStart - now));
        }
    }

    return RetailLibc::Malloc(size);
}

void Platform::Memory::Synchronise()
{
}

void Platform::Memory::BeforeDeviceWrite(void*, u32)
{
}

void Platform::Memory::WriteBackCache()
{
}
