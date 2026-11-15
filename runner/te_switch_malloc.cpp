#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <switch.h>
#include <reent.h>

extern "C" {
void* __real__malloc_r(struct _reent*, size_t);
void __real__free_r(struct _reent*, void*);
void* __real__memalign_r(struct _reent*, size_t, size_t);
size_t _malloc_usable_size_r(struct _reent*, void*);
}
namespace {
using Lock = Mutex;
void LockTake(Lock* l) { mutexLock(l); }
void LockGive(Lock* l) { mutexUnlock(l); }

void* RealMalloc(size_t n) {
    return __real__malloc_r(_REENT, n);
}
void RealFree(void* p) { __real__free_r(_REENT, p); }
void* RealMemalign(size_t a, size_t n) {
    return __real__memalign_r(_REENT, a, n);
}
size_t Usable(void* p) { return _malloc_usable_size_r(_REENT, p); }
}

namespace {

constexpr int kSmall = 32 + 56;
constexpr int kClasses = kSmall + 16;
constexpr size_t kLargest = 65536;
constexpr int kDepotBatches = 16;
constexpr size_t kDepotBytes = 128u << 10;

constexpr size_t ClassSize(int c) {
    if (c < 32) return size_t(c + 1) * 16;
    if (c < kSmall) return 512 + size_t(c - 31) * 64;
    const int k = (c - kSmall) / 4, q = (c - kSmall) % 4 + 1;
    return (size_t(4096) << k) + (size_t(1024) << k) * size_t(q);
}
int ClassFor(size_t n) {
    if (n <= 512) return int((n + 15) / 16) - 1;
    if (n <= 4096) return 31 + int((n - 512 + 63) / 64);
    int c = kSmall;
    while (ClassSize(c) < n) ++c;
    return c;
}
int ClassBelow(size_t usable) {
    if (usable < 16) return -1;
    if (usable < 512) return int(usable / 16) - 1;
    if (usable < 4096) return 31 + int((usable - 512) / 64);
    if (usable > kLargest + kLargest / 4) return -1;
    int c = kSmall - 1;
    while (c + 1 < kClasses && ClassSize(c + 1) <= usable) ++c;
    return c;
}

constexpr unsigned Cap(int c) {
    const size_t n = 32768 / ClassSize(c);
    const unsigned least = c < kSmall ? 8u : 4u;
    return n < least ? least : n > 128 ? 128u : unsigned(n);
}
constexpr unsigned Batch(int c) { return Cap(c) / 2; }
constexpr int DepotBatches(int c) {
    const size_t n = kDepotBytes / (size_t(Batch(c)) * ClassSize(c));
    return n < 1 ? 1 : n > size_t(kDepotBatches) ? kDepotBatches : int(n);
}

inline void*& Next(void* p) { return static_cast<void**>(p)[0]; }
inline void*& NextBatch(void* p) { return static_cast<void**>(p)[1]; }

struct Bin {
    void* head;
    unsigned count;
};
constexpr int kKinds = 2;
thread_local Bin t_bins[kKinds][kClasses];

thread_local bool t_exited = false;

struct alignas(64) Depot {
    Lock lock;
    void* batches;
    int count;
};
Depot g_depots[kKinds][kClasses];

alignas(64) std::atomic<bool> g_on{false};

void Refill(int kind, int c, Bin& bin) {
    Depot& d = g_depots[kind][c];
    LockTake(&d.lock);
    void* batch = d.batches;
    if (batch) {
        d.batches = NextBatch(batch);
        --d.count;
    }
    LockGive(&d.lock);
    if (batch) {
        bin.head = batch;
        bin.count = Batch(c);
        return;
    }
    const unsigned fresh = Batch(c) / 4 ? Batch(c) / 4 : 1;
    for (unsigned i = 0; i < fresh; ++i) {
        void* p = kind ? RealMemalign(32, ClassSize(c)) : RealMalloc(ClassSize(c));
        if (!p) break;
        Next(p) = bin.head;
        bin.head = p;
        ++bin.count;
    }
}

void Spill(int kind, int c, Bin& bin) {
    void* batch = bin.head;
    void* last = batch;
    for (unsigned i = 1; i < Batch(c); ++i) last = Next(last);
    bin.head = Next(last);
    bin.count -= Batch(c);
    Next(last) = nullptr;
    Depot& d = g_depots[kind][c];
    LockTake(&d.lock);
    const bool kept = d.count < DepotBatches(c);
    if (kept) {
        NextBatch(batch) = d.batches;
        d.batches = batch;
        ++d.count;
    }
    LockGive(&d.lock);
    if (kept) return;
    while (batch) {
        void* next = Next(batch);
        RealFree(batch);
        batch = next;
    }
}

void* CacheAlloc(size_t n, int kind) {
    const int c = ClassFor(n ? n : 1);
    Bin* bin = &t_bins[kind][c];
    if (!bin->head && kind == 0 && t_bins[1][c].head) bin = &t_bins[1][c];
    if (!bin->head) Refill(kind, c, *bin);
    void* p = bin->head;
    if (!p) return kind ? RealMemalign(32, ClassSize(c)) : RealMalloc(ClassSize(c));
    bin->head = Next(p);
    --bin->count;
    return p;
}

void* CacheMalloc(size_t n) {
    if (n > kLargest || !g_on.load(std::memory_order_relaxed) || t_exited) return RealMalloc(n);
    return CacheAlloc(n, 0);
}

void CacheFree(void* p) {
    if (!p) return;
    if (!g_on.load(std::memory_order_relaxed) || t_exited) return RealFree(p);
    const int c = ClassBelow(Usable(p));
    if (c < 0) return RealFree(p);
    const int kind = (reinterpret_cast<uintptr_t>(p) & 31) == 0;
    Bin& bin = t_bins[kind][c];
    Next(p) = bin.head;
    bin.head = p;
    if (++bin.count >= Cap(c)) Spill(kind, c, bin);
}

void* CacheMemalign(size_t align, size_t n) {

    if (n > kLargest || align > 32 || !g_on.load(std::memory_order_relaxed) || t_exited) return RealMemalign(align, n);
    return CacheAlloc(n, align > 16);
}

void* CacheCalloc(size_t count, size_t size) {
    size_t n = 0;
    if (__builtin_mul_overflow(count, size, &n)) {
        errno = ENOMEM;
        return nullptr;
    }
    void* p = CacheMalloc(n);
    if (p) std::memset(p, 0, n);
    return p;
}

}

void TeMallocCacheEnable(bool on) { g_on.store(on); }

void TeMallocCacheThreadExit() {
    t_exited = true;
    for (Bin(&bins)[kClasses] : t_bins)
        for (Bin& bin : bins) {
            while (bin.head) {
                void* next = Next(bin.head);
                RealFree(bin.head);
                bin.head = next;
            }
            bin.count = 0;
        }
}



extern "C" {
void* __wrap__malloc_r(struct _reent*, size_t n) { return CacheMalloc(n); }
void __wrap__free_r(struct _reent*, void* p) { CacheFree(p); }
void* __wrap__memalign_r(struct _reent*, size_t align, size_t n) { return CacheMemalign(align, n); }
void* __wrap__calloc_r(struct _reent*, size_t count, size_t size) { return CacheCalloc(count, size); }
}
