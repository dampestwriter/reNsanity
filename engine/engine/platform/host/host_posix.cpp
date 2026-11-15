#include <array>
#include "engine/platform/host/platform.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fcntl.h>
#include <pthread.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <malloc.h>
#include <switch.h>

#include <atomic>
#include <cstdio>
#include <mutex>

namespace ps2r::host {

void* AlignedAlloc(size_t alignment, size_t bytes) {
    void* p = nullptr;
    if (alignment < sizeof(void*)) alignment = sizeof(void*);
    p = memalign(alignment, bytes ? bytes : 1);
    return p;
}
void AlignedFree(void* p) { std::free(p); }

int FdOpen(const char* path, uint32_t flags) {
    int o = 0;
    const bool rd = flags & kOpenRead, wr = flags & kOpenWrite;
    o |= (rd && wr) ? O_RDWR : (wr ? O_WRONLY : O_RDONLY);
    if (flags & kOpenCreate) o |= O_CREAT;
    if (flags & kOpenTruncate) o |= O_TRUNC;
    if (flags & kOpenAppend) o |= O_APPEND;
    return ::open(path, o, 0644);
}

namespace {
std::mutex g_pread_mutex;
}

SSize FdPread(int fd, void* buf, size_t n, uint64_t offset) {

    constexpr size_t kChunk = 256u << 10;
    if (n > kChunk) n = kChunk;
    std::unique_lock<std::mutex> lock(g_pread_mutex);
    const off_t at = ::lseek(fd, 0, SEEK_CUR);
    if (at < 0 || ::lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0) return -1;
    const SSize got = ::read(fd, buf, n);
    ::lseek(fd, at, SEEK_SET);
    lock.unlock();
    return got;
}
int64_t FdSize(int fd) {
    struct stat st{};
    return ::fstat(fd, &st) == 0 ? static_cast<int64_t>(st.st_size) : -1;
}
int FdClose(int fd) { return ::close(fd); }

int StrCaseCmp(const char* a, const char* b) { return ::strcasecmp(a, b); }

namespace {
int BuilderCore(int worker_index) {
    static constexpr int cores[3] = {2, 1, 2};
    return worker_index >= 0 && worker_index < 3 ? cores[worker_index] : worker_index;
}
}

void SetCurrentThreadName(const char* name) {
    if (!name || !*name) return;
    (void)name;
}

extern "C" void te_switch_thread_named(const char* role);

void PinCurrentThreadFixed(ThreadRole role, int worker_index) {
    int core = -1, priority = 0x3B;
    const char* name = "";
    (void)priority;
    switch (role) {
        case ThreadRole::Game: core = 0; priority = 0x2C; name = "game"; break;
        case ThreadRole::Graphics: core = 1; priority = 0x2C; name = "render"; break;
        case ThreadRole::Worker: core = BuilderCore(worker_index); name = "builder"; break;
        case ThreadRole::Pacer: core = 2; priority = 0x28; name = "pacer"; break;
        case ThreadRole::AudioCallback: core = 2; priority = 0x29; name = "audio-pull"; break;
        case ThreadRole::AudioMixer: core = 2; priority = 0x2A; name = "audio-mix"; break;
        default: return;
    }
    const auto fail = [&] {
        std::fprintf(stderr, "[fixed.plan] FAILED role=%s worker=%d core=%d; plan invalid\n", name, worker_index, core);
        std::abort();
    };
    if (core < 0 || core > 2) { fail(); return; }
    const Handle self = threadGetCurHandle();
    u64 cores = 0, priorities = 0, got_mask = 0;
    s32 got_core = -1, got_priority = -1;
    if (R_FAILED(svcGetInfo(&cores, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) ||
        R_FAILED(svcGetInfo(&priorities, InfoType_PriorityMask, CUR_PROCESS_HANDLE, 0)) ||
        !(cores & (1ull << core)) || !(priorities & (1ull << priority))) { fail(); return; }
    const u64 want_mask = 1ull << core;
    if (R_FAILED(svcSetThreadCoreMask(self, core, want_mask)) ||
        R_FAILED(svcSetThreadPriority(self, priority)) ||
        R_FAILED(svcGetThreadCoreMask(&got_core, &got_mask, self)) ||
        R_FAILED(svcGetThreadPriority(&got_priority, self)) ||
        got_core != core || got_mask != want_mask || got_priority != priority) { fail(); return; }

    static constexpr const char* builder_names[] = {"builder-0", "builder-1", "builder-2"};
    te_switch_thread_named(role == ThreadRole::Worker ? builder_names[worker_index] : name);
}
void PinCurrentThreadInteractive(ThreadRole role) { PinCurrentThreadFixed(role); }

uint64_t MonotonicNowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void RaiseTimerResolution() {

}

void SleepForNs(uint64_t ns) {
    timespec req{};
    req.tv_sec = static_cast<time_t>(ns / 1000000000ull);
    req.tv_nsec = static_cast<long>(ns % 1000000000ull);

    while (nanosleep(&req, &req) == -1 && errno == EINTR) {
    }
}

}
