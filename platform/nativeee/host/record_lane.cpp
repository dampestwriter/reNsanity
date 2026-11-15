#include "record_lane.h"
#include "graphics_internal.h"
#include "te_host.h"
#include "ps2e_seam.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/skin_geometry.h"
#include "engine/platform/host/platform.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cfenv>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <switch.h>
extern "C" void te_switch_thread_named(const char* role);

namespace te::gfx {
namespace {
constexpr size_t kCommands = 256, kRanges = 65536, kBytes = 8 * 1024 * 1024;
constexpr uint32_t kView = 0x00309BE0, kTarget = 0x00309BDC;
struct Range { uint32_t address, size, offset; bool valid; };
struct Command {
    RecordCall call;
    uint32_t args[9]{};
    size_t first = 0, last = 0;
    std::fenv_t environment{};

    uint64_t fpcr = 0, fpsr = 0;
};
thread_local const Command* executing = nullptr;
thread_local bool onWorker = false;
struct Full {};

struct Batch {
    std::unique_ptr<Command[]> commands{new Command[kCommands]};
    std::unique_ptr<Range[]> ranges{new Range[kRanges]};
    std::unique_ptr<uint8_t[]> bytes{new uint8_t[kBytes]};
    size_t count = 0, nr = 0, nb = 0;

    static constexpr size_t kDedupCells = 4096;
    std::unique_ptr<uint32_t[]> dd{new uint32_t[kDedupCells]()};
    size_t unique = 0;
    void Reset() { count = nr = nb = 0; task = nullptr; if (unique) { std::memset(dd.get(), 0, kDedupCells * sizeof(uint32_t)); unique = 0; } }
    void (*task)() = nullptr;
    bool inflight = false;
};
struct Lane {
    Batch buf[2];
    Batch* cur = &buf[0];
    const Batch* run = nullptr;
    Batch* queue[2]{};
    unsigned head = 0, tail = 0;
    std::atomic<unsigned> outstanding{0};
    static constexpr size_t kReadCells = 131072;
    struct ReadCell { uint32_t epoch = 0, range = 0; };
    std::unique_ptr<ReadCell[]> reads{new ReadCell[kReadCells]};
    uint32_t readEpoch = 0;
    static constexpr size_t kSmallCells = 128;
    std::array<ReadCell, kSmallCells> smallReads{};
    bool smallIndex = false;
    template<size_t Cells> void FillIndex(ReadCell* table, const Command& c) {
        for (size_t i=c.first; i<c.last; ++i) {
            const auto& r=run->ranges[i]; size_t at=Hash(r.address,r.size)&(Cells-1);
            while (table[at].epoch==readEpoch) {
                const auto& old=run->ranges[table[at].range];
                if (r.address==old.address && r.size==old.size) break;
                at=(at+1)&(Cells-1);
            }
            table[at]={readEpoch,uint32_t(i)};
        }
    }
    static size_t Hash(uint32_t address, size_t size) {
        uint64_t k = (uint64_t(address) << 32) | uint32_t(size);
        k ^= k >> 33; k *= 0xff51afd7ed558ccdull; k ^= k >> 33;
        return size_t(k) & (kReadCells - 1);
    }
    void Index(const Command& c) {
        if (++readEpoch == 0) {
            for (size_t i=0; i<kReadCells; ++i) reads[i].epoch=0;
            for (auto& cell : smallReads) cell.epoch=0;
            readEpoch=1;
        }
        const size_t n=c.last-c.first;
        smallIndex = n <= kSmallCells/2;
        if (smallIndex) FillIndex<kSmallCells>(smallReads.data(),c);
        else FillIndex<kReadCells>(reads.get(),c);
    }
    template<size_t Cells> bool Lookup(ReadCell* table, uint32_t address, size_t size, uint8_t*& out) {
        size_t at=Hash(address,size)&(Cells-1);
        while (table[at].epoch==readEpoch) {
            const auto& r=run->ranges[table[at].range];
            if (r.address==address && r.size==size) { out=r.valid ? run->bytes.get()+r.offset : nullptr; return true; }
            at=(at+1)&(Cells-1);
        }
        return false;
    }
    uint8_t* Read(uint32_t address, size_t size) {
        uint8_t* out=nullptr;
        const bool found=smallIndex ? Lookup<kSmallCells>(smallReads.data(),address,size,out)
                                    : Lookup<kReadCells>(reads.get(),address,size,out);
        if (found) return out;

        for (size_t i=executing->first; i<executing->last; ++i) {
            const auto& r=run->ranges[i];
            if (address>=r.address && uint64_t(address)+size<=uint64_t(r.address)+r.size)
                return r.valid ? run->bytes.get()+r.offset+(address-r.address) : nullptr;
        }
        std::fprintf(stderr,"[reNsanity] record: uncaptured read: call %u address %#x bytes %zu\n",unsigned(executing->call),address,size);
        std::abort();
    }
    std::mutex mutex;
    std::condition_variable wake, done;

    uint8_t* mem = nullptr;
    uint64_t memSize = 0;
    bool stop = false;

    static constexpr size_t dedupeMin = 96;
    std::thread thread;
    const uint8_t* Copy(Command& c, uint32_t address, size_t size) {

        (void)c;
        Batch& b = *cur;
        const uint8_t* p = uint64_t(address) + size > memSize ? nullptr : mem + address;
        const size_t padded = (size + 15) & ~size_t(15);
        if (size > UINT32_MAX || b.nr == kRanges) throw Full{};
        uint32_t* cell = nullptr;
        size_t at = 0;
        if (size >= dedupeMin) {
            at = (uint32_t((address ^ (uint32_t(size) << 7)) * 0x9E3779B1u)) >> 20 & (Batch::kDedupCells - 1);
            for (; b.dd[at]; at = (at + 1) & (Batch::kDedupCells - 1)) {
                const uint32_t i = b.dd[at] - 1;
                if (i < b.nr && b.ranges[i].address == address && b.ranges[i].size == size) { cell = &b.dd[at]; break; }
            }
            if (cell) {
                const Range r = b.ranges[*cell - 1];
                if (r.valid == (p != nullptr) && (!p || !std::memcmp(p, b.bytes.get() + r.offset, size))) {
                    b.ranges[b.nr++] = Range{address, uint32_t(size), r.offset, r.valid};
                    return p;
                }
            }
        }
        if (p && padded > kBytes - b.nb) throw Full{};
        const uint32_t index = uint32_t(b.nr);
        b.ranges[b.nr++] = Range{address, uint32_t(size), uint32_t(b.nb), p != nullptr};
        if (p) { std::memcpy(b.bytes.get() + b.nb, p, size); b.nb += padded; }
        if (cell) *cell = index + 1;
        else if (size >= dedupeMin && b.unique < Batch::kDedupCells / 2) { b.dd[at] = index + 1; ++b.unique; }

        return p;
    }
    template<class T> const T* Copy(Command& c, uint32_t address) {
        return reinterpret_cast<const T*>(Copy(c, address, sizeof(T)));
    }
    void Materials(Command& c, uint32_t address, uint32_t count) {

        if (const auto* m0 = Copy<te_gfx_material>(c, 0); m0 && m0->magic == TE_GFX_MATERIAL_MAGIC)
            for (uint32_t k = 0; k < std::min<uint32_t>(m0->shaderCount, TE_GFX_MAX_SHADERS); ++k)
                if (m0->shaders[k]) Copy<te_gfx_shader>(c, m0->shaders[k]);
        const auto* resources = reinterpret_cast<const uint32_t*>(Copy(c, address, size_t(count) * 4));
        if (!resources) return;
        for (uint32_t i = 0; i < count; ++i) {
            if (!resources[i]) continue;
            const auto* p = Copy<uint32_t>(c, resources[i] + 8);
            const auto* m = p ? Copy<te_gfx_material>(c, *p) : nullptr;
            if (!m || m->magic != TE_GFX_MATERIAL_MAGIC) continue;
            for (uint32_t k = 0; k < std::min<uint32_t>(m->shaderCount, TE_GFX_MAX_SHADERS); ++k) {
                if (!m->shaders[k]) continue;
                const auto* s = Copy<te_gfx_shader>(c, m->shaders[k]);
                if (s && s->magic == TE_GFX_SHADER_MAGIC && s->texture) Copy<te_gfx_texture>(c, s->texture);
            }
        }
    }
    void View(Command& c) {
        const auto* vp = Copy<uint32_t>(c, kView);
        if (vp && *vp) Copy(c, *vp, 0x170);
    }
    void Snapshot(Command& c) {
        auto* a = c.args;
        if (c.call != RecordCall::Placed) {
            const uint32_t lightAddress = c.call == RecordCall::Rigid ? a[2] : a[4];
            struct Lights { uint32_t directions, colours, ambient; };
            if (const auto* l = Copy<Lights>(c, lightAddress)) {
                Copy(c, l->directions, 48); Copy(c, l->colours, 48); Copy(c, l->ambient, 16);
            }
        }
        if (c.call == RecordCall::Rigid || c.call == RecordCall::Placed) {
            const auto* r = Copy<te_gfx_rigid_model>(c, a[0]);

            View(c);
            if (c.call == RecordCall::Rigid) {
                const auto* tp=Copy<uint32_t>(c,kTarget);
                if (tp && *tp) Copy<RenderTargetDescription>(c,*tp);
            }
            if (r) {
                const auto* m = r->model ? Copy<te_gfx_model>(c, r->model) : nullptr;
                if (m) Materials(c, r->materials, std::min(r->count, m->subModelCount));
            }
            if (c.call == RecordCall::Placed) { Copy(c, a[1], 0x108); Copy(c, a[2], 64); return; }
            Copy(c, a[1], 64);
        } else {
            View(c);
            Copy(c, a[3], 64);
            if (int32_t(a[2]) > 0 && a[2] <= title::SkinGeometry::kMaxJoints) Copy(c, a[1], size_t(a[2]) * 64);
            if (c.call == RecordCall::Skin) {
                const auto* s = Copy<te_gfx_skin>(c, a[0]);
                if (s) Materials(c, s->materials, s->count);
            } else {
                const auto* s = Copy<te_gfx_blend_skin>(c, a[0]);
                if (!s) return;
                if (int32_t(a[7]) < 0 || a[7] > 64) return;
                const auto* shapes = a[7] ? reinterpret_cast<const int32_t*>(Copy(c, a[6], size_t(a[7]) * 4)) : nullptr;
                if (a[7]) Copy(c, a[5], size_t(a[7]) * 4);
                const auto* sub = s->count ? reinterpret_cast<const uint32_t*>(Copy(c, s->subModels, size_t(s->count) * 4)) : nullptr;
                if (!sub || s->count > 4096) return;
                Materials(c, s->materials, s->count);
                for (uint32_t i = 0; i < s->count; ++i) {
                    const auto* sm = Copy<te_gfx_blend_sub_model>(c, sub[i]);
                    if (!sm || !sm->count || sm->count > 4096) continue;
                    const auto* parts = reinterpret_cast<const uint32_t*>(Copy(c, sm->parts, size_t(sm->count) * 4));
                    if (!parts) continue;
                    for (uint32_t j = 0; j < sm->count; ++j) {
                        const auto* p = Copy<te_gfx_blend_part>(c, parts[j]);
                        if (!p || !p->packet || !p->packetSize) continue;

                        Copy(c, p->packet, p->packetSize);
                        const auto* addresses = p->shapeCount ? reinterpret_cast<const uint32_t*>(Copy(c, p->shapeAddresses, size_t(p->shapeCount) * 4)) : nullptr;
                        const auto* counts = p->shapeCount ? reinterpret_cast<const uint32_t*>(Copy(c, p->shapeCounts, size_t(p->shapeCount) * 4)) : nullptr;
                        if (!shapes || !addresses || !counts) continue;
                        for (uint32_t k = 0; k < a[7]; ++k) {
                            const int32_t shape = shapes[k];
                            if (shape >= 0 && shape < p->shapeCount && counts[shape]) Copy(c, addresses[shape], size_t(counts[shape]) * 4);
                        }
                    }
                }
            }
        }

    }
    void Run() {
        onWorker = true;
        ps2r::host::SetCurrentThreadName("te-record");

        if (R_FAILED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, 2, 1ull << 2)) ||
            R_FAILED(svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3B))) {
            std::fprintf(stderr, "[reNsanity] record: cannot place recording worker on core 2\n"); std::abort();
        }
        te_switch_thread_named("record");
        for (;;) {
            Batch* b;
            { std::unique_lock<std::mutex> lock(mutex);
              wake.wait(lock, [&] { return stop || head != tail; });
              if (head == tail) return;
              b = queue[head++ & 1]; }
            run = b;
            for (size_t i = 0; i < b->count; ++i) {
                const auto& c = b->commands[i]; executing = &c; Index(c);
                if (std::fesetenv(&c.environment)) std::abort();
                __asm__ volatile("msr fpcr, %0" :: "r"(c.fpcr));
                __asm__ volatile("msr fpsr, %0" :: "r"(c.fpsr));
                const auto* a = c.args;
                switch (c.call) {
                case RecordCall::Rigid: ps2e_graphics_draw_rigid_model(a[0], a[1], a[2], a[3]); break;
                case RecordCall::Placed: ps2e_graphics_draw_placed_model(a[0], a[1], a[2]); break;
                case RecordCall::Skin: ps2e_graphics_draw_skin(a[0], a[1], a[2], a[3], a[4], a[5]); break;
                case RecordCall::Blend: ps2e_graphics_draw_blend_skin(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]); break;
                }
                executing = nullptr;
            }
            if (b->task) b->task();

            DrainBuilds();
            run = nullptr;
            { std::lock_guard<std::mutex> lock(mutex); b->inflight = false; outstanding.fetch_sub(1, std::memory_order_release); }
            done.notify_all();
        }
    }

    void Publish() {
        if (!thread.joinable()) {

            EnsureDevice(); DeferringBuilds();
            thread = std::thread([this] { Run(); });
        }
        Batch* b = cur; Batch* other = (b == &buf[0]) ? &buf[1] : &buf[0];
        { std::unique_lock<std::mutex> lock(mutex);
          b->inflight = true; queue[tail++ & 1] = b; outstanding.fetch_add(1, std::memory_order_relaxed);
          wake.notify_one();
          if (other->inflight) done.wait(lock, [&] { return !other->inflight; }); }
        other->Reset();
        cur = other;
    }

    void Drain() {
        if (!cur->count && !cur->task && outstanding.load(std::memory_order_acquire) == 0) return;
        if (cur->count || cur->task) Publish();
        if (outstanding.load(std::memory_order_acquire) != 0) {
            std::unique_lock<std::mutex> lock(mutex);
            done.wait(lock, [&] { return outstanding.load(std::memory_order_relaxed) == 0; });
        }
    }
    void Stop() {
        Drain();
        if (thread.joinable()) {
            { std::lock_guard<std::mutex> lock(mutex); stop = true; }
            wake.notify_one(); thread.join();
        }
    }
    ~Lane() { Stop(); }
};
Lane* lane = nullptr;

Lane& Get() { static Lane instance; if (!lane) lane = &instance; return instance; }
}
bool CaptureRecord(RecordCall call, const uint32_t* args) {
    if (executing) return false;

    if (!lane) { EnsureDevice(); DeferringBuilds(); }
    Lane& l = Get();
    if (l.stop) return false;
    l.mem = te::GuestMemory(); l.memSize = l.mem ? te::GuestMemorySize() : 0;
    if (l.cur->count == kCommands) l.Publish();
    const size_t argc = call == RecordCall::Placed ? 3 : call == RecordCall::Rigid ? 4 : call == RecordCall::Skin ? 6 : 9;
    for (int attempt = 0; attempt != 2; ++attempt) {
        Batch& b = *l.cur;
        Command c; c.call = call; c.first = b.nr;
        if (std::fegetenv(&c.environment)) std::abort();
        __asm__ volatile("mrs %0, fpcr" : "=r"(c.fpcr));
        __asm__ volatile("mrs %0, fpsr" : "=r"(c.fpsr));
        std::copy_n(args, argc, c.args);
        const size_t nr = b.nr, nb = b.nb;
        try {
            l.Snapshot(c); c.last = b.nr;
            b.commands[b.count++] = c;
            return true;
        } catch (Full&) {
            b.nr = nr; b.nb = nb;
            if (b.count) l.Publish();
        }
    }

    l.Drain();

    return false;
}
bool RecordFast() { return true; }
void RecordFastBoundary() {

    DrainRecordLane();
}
void DrainRecordLane() { if (!onWorker && lane) lane->Drain(); }
bool RecordTask(void (*task)()) {
    if (onWorker || !lane) return false;
    lane->cur->task = task; lane->Drain(); return true;
}
void StopRecordLane() {
    if (lane) lane->Stop();
}
uint8_t* RecordGuestPointer(uint32_t address, size_t size) {
    if (!executing) return te::GuestPointer(address, size);
    return lane->Read(address, size);
}
}

extern "C" void te_record_barrier(void) { te::gfx::DrainRecordLane(); }
