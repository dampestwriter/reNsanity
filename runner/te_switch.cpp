#include "te_switch.h"

#include <switch.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

void TeMallocCacheEnable(bool on);
void TeMallocCacheThreadExit();
extern "C" void te_graphics_release_device();

namespace {
constexpr size_t kMinThreadStack = 1024u * 1024u;

constexpr int kThreadSlots = 32;
struct ThreadSlot {
    std::atomic<Handle> handle{INVALID_HANDLE};
    std::atomic<u64> entry{0};
    std::atomic<const char*> name{nullptr};
    u64 ticks = 0;
};
ThreadSlot g_thread_slots[kThreadSlots];
std::atomic<int> g_thread_count{0};
bool g_threads_on = false;

int ThreadRegister(Handle h, u64 entry) {
    const int n = std::min(g_thread_count.load(), kThreadSlots);
    for (int k = 0; k < n; ++k) {
        Handle free_handle = INVALID_HANDLE;
        if (g_thread_slots[k].entry.load() == 0 || g_thread_slots[k].handle.load() != INVALID_HANDLE) continue;
        if (g_thread_slots[k].handle.compare_exchange_strong(free_handle, h)) {
            g_thread_slots[k].entry.store(entry);
            return k;
        }
    }
    const int i = g_thread_count.fetch_add(1);
    if (i >= kThreadSlots) return -1;
    g_thread_slots[i].entry.store(entry);
    g_thread_slots[i].handle.store(h);
    return i;
}

}
extern "C" char* fake_heap_start;
extern "C" char* fake_heap_end;
extern "C" char __malloc_av_[];
extern "C" void __malloc_lock(struct _reent*);
extern "C" void __malloc_unlock(struct _reent*);

extern "C" u64 __nx_heap_size __attribute__((weak));
extern "C" void __libnx_initheap(void) {
    void* addr = nullptr;
    u64 size = 0;
    if (envHasHeapOverride()) {
        addr = envGetHeapOverrideAddr();
        size = envGetHeapOverrideSize();
    } else {
        u64 available = 0, used = 0;
        svcGetInfo(&available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
        if (&__nx_heap_size && __nx_heap_size) size = __nx_heap_size;
        else if (available > used + 0x200000) size = (available - used - 0x200000) & ~u64(0x1FFFFF);
        if (!size) size = 0x2000000ull * 16;
        if (R_FAILED(svcSetHeapSize(&addr, size))) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
    }
    const u64 lo = reinterpret_cast<u64>(addr), hi = lo + size;
    u64 best_lo = lo, best_hi = lo, run_lo = lo;
    bool complete = true, holes = false;
    for (u64 a = lo; a < hi;) {
        MemoryInfo mi{};
        u32 page = 0;
        if (R_FAILED(svcQueryMemory(&mi, &page, a)) || !mi.size) { complete = false; break; }
        const u64 end = std::min(hi, mi.addr + mi.size);
        if (mi.perm != Perm_Rw || mi.attr) {
            holes = true;
            if (a - run_lo > best_hi - best_lo) { best_lo = run_lo; best_hi = a; }
            run_lo = end;
        }
        a = end;
    }
    if (!complete && !holes) { best_lo = lo; best_hi = hi; }
    else if (hi - run_lo > best_hi - best_lo) { best_lo = run_lo; best_hi = hi; }
    extern char* fake_heap_start;
    extern char* fake_heap_end;
    fake_heap_start = reinterpret_cast<char*>(best_lo);
    fake_heap_end = reinterpret_cast<char*>(best_hi);
}
extern "C" Result __real_threadClose(Thread* t);
extern "C" Result __wrap_threadClose(Thread* t) {
    return __real_threadClose(t);
}
namespace {
struct ThreadStart {
    void* (*func)(void*);
    void* arg;
};

void* WrappedThread(void* p) {
    const ThreadStart start = *static_cast<ThreadStart*>(p);
    delete static_cast<ThreadStart*>(p);
    const int slot = g_threads_on ? ThreadRegister(threadGetCurHandle(), reinterpret_cast<u64>(start.func)) : -1;
    void* result = start.func(start.arg);
    if (slot >= 0) {
        g_thread_slots[slot].name.store(nullptr);
        g_thread_slots[slot].handle.store(INVALID_HANDLE);
    }
    TeMallocCacheThreadExit();
    return result;
}
}
extern "C" int __real___syscall_thread_create(struct __pthread_t** thread, void* (*func)(void*), void* arg,
                                              void* stack_addr, size_t stack_size);
extern "C" int __wrap___syscall_thread_create(struct __pthread_t** thread, void* (*func)(void*), void* arg,
                                              void* stack_addr, size_t stack_size) {
    if (!stack_addr && stack_size < kMinThreadStack) stack_size = kMinThreadStack;
    auto* start = new ThreadStart{func, arg};
    const int rc = __real___syscall_thread_create(thread, &WrappedThread, start, stack_addr, stack_size);
    if (rc != 0) delete start;
    return rc;
}

extern "C" void te_switch_thread_named(const char* role) {
    const Handle self = threadGetCurHandle();
    const int n = std::min(g_thread_count.load(), kThreadSlots);
    for (int i = 0; i < n; ++i)
        if (g_thread_slots[i].handle.load() == self) g_thread_slots[i].name.store(role);
}

namespace te::nx {
namespace {
int g_log_fd_for_crash = -1;
const char* const kLogPath = "sdmc:/switch/reNsanity/reNsanity.log";
}
}

extern "C" void _start();
extern "C" char __code_start[], __rodata_start[];
alignas(16) u8 __nx_exception_stack[0x4000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
namespace {

struct CrashLine {
    char text[256];
    int n = 0;
    CrashLine& Put(const char* s) {
        while (*s && n < int(sizeof text) - 1) text[n++] = *s++;
        return *this;
    }
    CrashLine& Hex(u64 v) {
        Put("0x");
        int digits = 1;
        while (digits < 16 && (v >> (4 * digits))) ++digits;
        for (int d = digits - 1; d >= 0 && n < int(sizeof text) - 1; --d) text[n++] = "0123456789abcdef"[(v >> (4 * d)) & 15];
        return *this;
    }
    void Out() {
        text[n++] = '\n';
        if (te::nx::g_log_fd_for_crash < 0) te::nx::g_log_fd_for_crash = ::open(te::nx::kLogPath, O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (te::nx::g_log_fd_for_crash >= 0) (void)!::write(te::nx::g_log_fd_for_crash, text, size_t(n));
        n = 0;
    }
};
}

extern "C" void __libnx_exception_handler(ThreadExceptionDump* ctx) {
    const u64 base = reinterpret_cast<u64>(&_start);
    CrashLine line;
    line.Put("\n[reNsanity] switch: CRASH exception ").Hex(ctx->error_desc).Put(", pc ").Hex(ctx->pc.x).Put(" (+")
        .Hex(ctx->pc.x - base).Put("), lr ").Hex(ctx->lr.x).Put(" (+").Hex(ctx->lr.x - base).Put("), sp ").Hex(ctx->sp.x)
        .Put(", far ").Hex(ctx->far.x).Out();
    for (int i = 0; i < 29; i += 4) {
        line.Put("[reNsanity] switch: ");
        for (int k = i; k < i + 4 && k < 29; ++k) line.Put(" x").Hex(u64(k)).Put(" ").Hex(ctx->cpu_gprs[k].x);
        line.Out();
    }

    u64 fp = ctx->fp.x;
    const u64 lo = ctx->sp.x;
    u64 hi = 0;
    MemoryInfo info{};
    u32 page = 0;
    if (R_SUCCEEDED(svcQueryMemory(&info, &page, lo)) && (info.perm & Perm_R))
        hi = std::min<u64>(info.addr + info.size, lo + (1u << 20));
    for (int depth = 0; depth < 24 && fp >= lo && fp + 16 <= hi && !(fp & 7); ++depth) {
        const u64* record = reinterpret_cast<const u64*>(fp);
        const u64 ret = record[1];
        line.Put("[reNsanity] switch:  frame ").Hex(u64(depth)).Put(": ").Hex(ret).Put(" (+").Hex(ret - base).Put(")").Out();
        if (record[0] <= fp) break;
        fp = record[0];
    }
}


namespace te::nx {

namespace {

constexpr const char* kDataDir = "sdmc:/switch/reNsanity";

std::vector<std::string> g_args;
std::vector<char*> g_argv;

struct Clock {
    PcvModuleId id;
    PcvModule legacy;
    u32 found = 0;
};
Clock g_clocks[] = {{PcvModuleId_CpuBus, PcvModule_CpuBus}, {PcvModuleId_GPU, PcvModule_GPU}};

u32 ClockRate(const Clock& c, u32 hz, u32* found) {
    u32 now = 0;
    if (hosversionAtLeast(8, 0, 0)) {
        ClkrstSession s;
        if (R_FAILED(clkrstOpenSession(&s, c.id, 3))) return 0;
        if (found && R_FAILED(clkrstGetClockRate(&s, found))) {
            clkrstCloseSession(&s);
            return 0;
        }
        if (hz) clkrstSetClockRate(&s, hz);
        clkrstGetClockRate(&s, &now);
        clkrstCloseSession(&s);
    } else {
        if (found && R_FAILED(pcvGetClockRate(c.legacy, found))) return 0;
        if (hz) pcvSetClockRate(c.legacy, hz);
        pcvGetClockRate(c.legacy, &now);
    }
    return now;
}

void RestoreClocks() {
    for (const Clock& c : g_clocks)
        if (c.found) ClockRate(c, c.found, nullptr);
    if (hosversionAtLeast(8, 0, 0)) clkrstExit();
    else pcvExit();
}

void RaiseClocks() {
    if (R_FAILED(hosversionAtLeast(8, 0, 0) ? clkrstInitialize() : pcvInitialize())) return;
    constexpr u32 kGpuMhz = 384;
    Clock& c = g_clocks[1];
    ClockRate(c, kGpuMhz * 1000000u, &c.found);
    std::atexit(RestoreClocks);
}

Thread g_balance;
std::atomic<bool> g_balance_stop{false};

void BalanceMain(void*) {
    double thread_avg[kThreadSlots]{}, moved_at[kThreadSlots]{}, core_avg[3]{}, last_move = -1e9;
    Handle avg_handle[kThreadSlots]{};
    bool core_avg_on = false;
    u64 idle_was[4] = {};
    bool core_ok[4] = {};
    u64 was = armGetSystemTick();
    for (int c = 0; c < 4; ++c)
        core_ok[c] = R_SUCCEEDED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, c, 1u << c)) &&
                     R_SUCCEEDED(svcGetInfo(&idle_was[c], InfoType_IdleTickCount, INVALID_HANDLE, u64(c)));
    double t = 0;
    while (!g_balance_stop.load()) {
        for (int i = 0; i < 10 && !g_balance_stop.load(); ++i) svcSleepThread(100000000);
        const u64 now = armGetSystemTick();
        const double wall_ticks = double(now - was);
        t += double(armTicksToNs(now - was)) / 1e9;
        was = now;
        float cores[4];
        for (float& c : cores) c = std::nanf("");
        for (int c = 0; c < 4; ++c) {
            u64 idle = 0;
            if (!core_ok[c] || R_FAILED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, c, 1u << c)) ||
                R_FAILED(svcGetInfo(&idle, InfoType_IdleTickCount, INVALID_HANDLE, u64(c))))
                continue;
            cores[c] = float(100.0 * (1.0 - double(idle - idle_was[c]) / wall_ticks));
            idle_was[c] = idle;
        }
        const int n = std::min(g_thread_count.load(), kThreadSlots);
        for (int i = 0; i < n; ++i) {
            ThreadSlot& sl = g_thread_slots[i];
            const Handle h = sl.handle.load();
            u64 ticks = 0;
            if (h == INVALID_HANDLE || R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, h, u64(-1)))) continue;
            const double share = sl.ticks ? 100.0 * double(ticks - sl.ticks) / wall_ticks : 0.0;
            sl.ticks = ticks;
            if (avg_handle[i] != h) { avg_handle[i] = h; thread_avg[i] = share; moved_at[i] = -1e9; }
            else thread_avg[i] = 0.5 * (thread_avg[i] + share);
        }
        if (std::isfinite(cores[0]) && std::isfinite(cores[1]) && std::isfinite(cores[2])) {
            for (int c = 0; c < 3; ++c) core_avg[c] = core_avg_on ? 0.5 * (core_avg[c] + cores[c]) : cores[c];
            core_avg_on = true;
            int hi = 0, lo = 0;
            for (int c = 1; c < 3; ++c) {
                if (core_avg[c] > core_avg[hi]) hi = c;
                if (core_avg[c] < core_avg[lo]) lo = c;
            }
            if (t - last_move >= 2.0 && core_avg[hi] - core_avg[lo] > 6.0) {
                int best = -1;
                double best_after = core_avg[hi] - 1.0;
                for (int i = 0; i < n; ++i) {
                    const Handle h = g_thread_slots[i].handle.load();
                    const char* name = g_thread_slots[i].name.load();
                    if (h == INVALID_HANDLE || avg_handle[i] != h || !name || t - moved_at[i] < 5.0) continue;
                    const bool record = !std::strcmp(name, "record");
                    if (!record && std::strncmp(name, "builder", 7)) continue;
                    if (record && lo == 0) continue;
                    s32 core = -1;
                    u64 mask = 0;
                    if (R_FAILED(svcGetThreadCoreMask(&core, &mask, h)) || mask != (1ull << hi) || thread_avg[i] < 1.0) continue;
                    const double after = std::max(core_avg[hi] - thread_avg[i], core_avg[lo] + thread_avg[i]);
                    if (after < best_after) { best_after = after; best = i; }
                }
                const Handle h = best >= 0 ? g_thread_slots[best].handle.load() : INVALID_HANDLE;
                if (best >= 0 && R_SUCCEEDED(svcSetThreadCoreMask(h, lo, 1u << lo))) {
                    core_avg[hi] -= thread_avg[best];
                    core_avg[lo] += thread_avg[best];
                    moved_at[best] = t;
                    last_move = t;
                }
            }
        }
    }
}

void StopBalance() {
    g_balance_stop.store(true);
    threadWaitForExit(&g_balance);
    threadClose(&g_balance);
}

void StartBalance() {
    if (R_FAILED(threadCreate(&g_balance, BalanceMain, nullptr, nullptr, 0x10000, 0x20, 2)) || R_FAILED(threadStart(&g_balance))) return;
    std::atexit(StopBalance);
}

int LogWrite(void*, const char* buf, size_t n) {
    if (g_log_fd_for_crash < 0) g_log_fd_for_crash = ::open(kLogPath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (g_log_fd_for_crash < 0) return static_cast<int>(n);
    const ssize_t wrote = ::write(g_log_fd_for_crash, buf, n);
    return wrote < 0 ? -1 : static_cast<int>(wrote);
}

std::vector<std::string> DefaultArgs() {
    return {"--iso", std::string(kDataDir) + "/game.iso", "--no-kbm",
            "--mc-dir", std::string(kDataDir) + "/memcards", "--config", std::string(kDataDir) + "/ps2r.ini"};
}

[[noreturn]] void OnTerminate() {
    const char* what = "no exception object";
    std::string text;
    if (const std::exception_ptr e = std::current_exception()) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& x) {
            text = x.what();
            what = text.c_str();
        } catch (...) {
            what = "not a std::exception";
        }
    }
    std::fprintf(stderr, "[reNsanity] switch: std::terminate (%s)\n", what);

    {
        const u64 base = reinterpret_cast<u64>(&_start);
        const u64 code_lo = reinterpret_cast<u64>(__code_start), code_hi = reinterpret_cast<u64>(__rodata_start);
        const u64 sp = reinterpret_cast<u64>(__builtin_frame_address(0));
        MemoryInfo info{};
        u32 page = 0;
        if (R_SUCCEEDED(svcQueryMemory(&info, &page, sp)) && (info.perm & Perm_R)) {
            const u64 end = std::min<u64>(info.addr + info.size, (sp & ~7ull) + 32768);
            char line[1024];
            int len = std::snprintf(line, sizeof line, "[reNsanity] switch: terminate stack:");
            int found = 0;
            for (u64 a = sp & ~7ull; a + 8 <= end && found < 40 && len < int(sizeof line) - 16; a += 8) {
                const u64 ret = *reinterpret_cast<const u64*>(a);
                if (ret < code_lo + 4 || ret >= code_hi || (ret & 3)) continue;
                const u32 call = *reinterpret_cast<const u32*>(ret - 4);
                if ((call & 0xFC000000u) != 0x94000000u && (call & 0xFFFFFC1Fu) != 0xD63F0000u) continue;
                len += std::snprintf(line + len, sizeof line - size_t(len), " %#llx", (unsigned long long)(ret - 4 - base));
                found++;
            }
            std::fprintf(stderr, "%s\n", line);
        }
    }
    std::fflush(stderr);
    std::abort();
}
}

void Startup(int* argc, char*** argv) {
    std::set_terminate(OnTerminate);

    std::atexit(te_graphics_release_device);

    g_threads_on = true;
    ThreadRegister(envGetMainThreadHandle(), reinterpret_cast<u64>(&_start));
    ::mkdir("sdmc:/switch", 0777);
    ::mkdir(kDataDir, 0777);
    ::mkdir((std::string(kDataDir) + "/memcards").c_str(), 0777);

    if (FILE* f = funopen(nullptr, nullptr, &LogWrite, nullptr, nullptr)) {
        std::setvbuf(f, nullptr, _IOLBF, 0);
        stderr = f;
        stdout = f;
    }

    g_args = DefaultArgs();
    TeMallocCacheEnable(true);
    RaiseClocks();
    StartBalance();
    g_argv.clear();
    g_argv.push_back((*argv && (*argv)[0]) ? (*argv)[0] : const_cast<char*>("reNsanity"));
    for (std::string& a : g_args) g_argv.push_back(a.data());
    g_argv.push_back(nullptr);
    *argc = static_cast<int>(g_argv.size()) - 1;
    *argv = g_argv.data();
}

}
