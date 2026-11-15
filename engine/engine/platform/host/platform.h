#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <chrono>
#include <string>

#include "engine/platform/platform.h"

#define PS2R_FENV_ACCESS_ON _Pragma("GCC optimize (\"rounding-math\")")

namespace ps2r::host {
using ThreadRole = ps2r::platform::ThreadRole;
}

namespace ps2r::platform::host {

using ps2r::host::ThreadRole;

void PinCurrentThreadInteractive(const char* name, ThreadRole role = ThreadRole::Interactive, int worker_index = -1);

}

namespace ps2r::host {


void* AlignedAlloc(size_t alignment, size_t bytes);
void AlignedFree(void* p);

using SSize = long long;

enum FileOpenFlags : uint32_t {
    kOpenRead = 1u << 0,
    kOpenWrite = 1u << 1,
    kOpenCreate = 1u << 2,
    kOpenTruncate = 1u << 3,
    kOpenAppend = 1u << 4,
};
int FdOpen(const char* path, uint32_t flags);

SSize FdPread(int fd, void* buf, size_t n, uint64_t offset);

int64_t FdSize(int fd);
int FdClose(int fd);

int StrCaseCmp(const char* a, const char* b);


void SetCurrentThreadName(const char* name);

void PinCurrentThreadInteractive(ThreadRole role = ThreadRole::Interactive);

void PinCurrentThreadFixed(ThreadRole role, int worker_index = -1);

void RaiseTimerResolution();

void SleepForNs(uint64_t ns);

uint64_t MonotonicNowNs();

}
