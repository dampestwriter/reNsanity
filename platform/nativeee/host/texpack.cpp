#include "texpack.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <deque>
#include <system_error>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <switch.h>

namespace te::gfx::texpack {
namespace {

struct Pack {
    std::mutex m;
    bool on = false;
    std::string dir;
    uint64_t budget = 512ull << 20, held = 0;
    struct Entry { std::string path; uint64_t offset = 0; uint32_t size = 0; };
    std::unordered_map<std::string, Entry> files;
    std::mutex archive_m;
    std::FILE* archive = nullptr;
};

uint32_t Rd32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

constexpr const char* kDir = "sdmc:/switch/reNsanity/texpack";

void Open(Pack& p) {
    p.on = true;
    p.dir = kDir;

    if (SmallProcess()) p.budget = 64ull << 20;
    std::unordered_map<std::string, Pack::Entry> files;

    const std::string bin = p.dir + "/texpack.bin";
    if (std::FILE* f = std::fopen(bin.c_str(), "rb")) {
        uint8_t head[12];
        uint32_t count = 0;
        if (std::fread(head, 1, 12, f) == 12 && std::memcmp(head, "TEPK", 4) == 0 && Rd32(head + 4) == 1) count = Rd32(head + 8);
        std::vector<uint8_t> table(size_t(count) * 24);
        if (count && std::fread(table.data(), 1, table.size(), f) == table.size()) {
            for (uint32_t i = 0; i < count; ++i) {
                const uint8_t* e = table.data() + size_t(i) * 24;
                const uint64_t key = Rd32(e) | uint64_t(Rd32(e + 4)) << 32;
                const int w = e[8] | e[9] << 8, h = e[10] | e[11] << 8;
                files[Name(key, w, h)] = {bin, Rd32(e + 16) | uint64_t(Rd32(e + 20)) << 32, Rd32(e + 12)};
            }
            p.archive = f;
        } else {
            std::fclose(f);
        }
    }

    if (DIR* d = opendir(p.dir.c_str())) {
        while (const dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > 4 && (n.compare(n.size() - 4, 4, ".dds") == 0 || n.compare(n.size() - 4, 4, ".DDS") == 0))
                files[n.substr(0, n.size() - 4)] = {p.dir + "/" + n};
        }
        closedir(d);
    }
    {
        std::lock_guard<std::mutex> lock(p.m);
        p.files.swap(files);
    }
}

Pack& P() {
    static Pack& p = *new Pack;
    return p;
}

}

bool Available() {
    static const bool present = [] {
        struct stat st;
        return stat((std::string(kDir) + "/texpack.bin").c_str(), &st) == 0 && S_ISREG(st.st_mode);
    }();
    return present;
}

bool Enabled() { return P().on; }

void SetEnabled(bool on) {
    Pack& p = P();
    if (on && !p.on) Open(p);
    else if (!on && p.on) p.on = false;
}

uint64_t Key(const uint32_t* rgba, int width, int height) {
    uint64_t h = 0xcbf29ce484222325ull;
    const auto* b = reinterpret_cast<const uint8_t*>(rgba);
    const size_t n = size_t(width) * size_t(height) * 4u;
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 0x100000001b3ull;
    h = (h ^ uint64_t(width)) * 0x100000001b3ull;
    return (h ^ uint64_t(height)) * 0x100000001b3ull;
}

std::string Name(uint64_t key, int width, int height) {
    char s[64];
    std::snprintf(s, sizeof s, "%dx%d_%016llx", width, height, (unsigned long long)key);
    return s;
}

size_t LevelBytes(Format f, int w, int h) {
    const size_t bw = size_t(std::max(1, (w + 3) / 4)), bh = size_t(std::max(1, (h + 3) / 4));
    switch (f) {
        case kRgba8: return size_t(w) * size_t(h) * 4u;
        case kBc1: return bw * bh * 8u;
        case kBc3: case kBc7: return bw * bh * 16u;
        default: return 0;
    }
}

bool ParseDds(const uint8_t* d, size_t size, Replacement* out, std::string* error) {
    const auto fail = [&](const char* what) { if (error) *error = what; return false; };
    if (size < 128 || std::memcmp(d, "DDS ", 4) != 0 || Rd32(d + 4) != 124) return fail("not a DDS file");
    const int h = int(Rd32(d + 12)), w = int(Rd32(d + 16));
    const uint32_t mips = std::max<uint32_t>(1, Rd32(d + 28));
    const uint32_t pf_flags = Rd32(d + 80), fourcc = Rd32(d + 84), bits = Rd32(d + 88);
    size_t at = 128;
    Format f = kNone;
    if (pf_flags & 0x4) {
        if (fourcc == 0x31545844u) f = kBc1;
        else if (fourcc == 0x35545844u) f = kBc3;
        else if (fourcc == 0x30315844u) {
            if (size < 148) return fail("DX10 header cut");
            const uint32_t dxgi = Rd32(d + 128);
            at = 148;
            if (dxgi == 71 || dxgi == 72) f = kBc1;
            else if (dxgi == 77 || dxgi == 78) f = kBc3;
            else if (dxgi == 98 || dxgi == 99) f = kBc7;
            else if (dxgi == 28 || dxgi == 29) f = kRgba8;
        }
    } else if ((pf_flags & 0x40) && bits == 32 && Rd32(d + 92) == 0xFFu && Rd32(d + 96) == 0xFF00u &&
               Rd32(d + 100) == 0xFF0000u) {
        f = kRgba8;
    }
    if (f == kNone) return fail("format not BC1 / BC3 / BC7 / RGBA8");
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || (w & (w - 1)) || (h & (h - 1))) return fail("size not a power of two (8192 at most)");
    out->width = w;
    out->height = h;
    out->format = f;
    out->level_offset.clear();
    out->level_size.clear();
    size_t total = 0;
    for (uint32_t i = 0; i < std::min<uint32_t>(mips, 14); ++i) {
        const int lw = std::max(1, w >> i), lh = std::max(1, h >> i);
        const size_t n = LevelBytes(f, lw, lh);
        if (at + total + n > size) break;
        out->level_offset.push_back(total);
        out->level_size.push_back(n);
        total += n;
        if (lw == 1 && lh == 1) break;
    }
    if (out->level_size.empty()) return fail("no whole level");
    out->bytes.assign(d + at, d + at + total);
    return true;
}

bool Has(uint64_t key, int width, int height) {
    Pack& p = P();
    if (!p.on) return false;
    std::lock_guard<std::mutex> lock(p.m);
    return p.files.count(Name(key, width, height)) != 0;
}

bool Find(uint64_t key, int width, int height, Replacement* out, std::string* why) {
    Pack& p = P();
    if (!p.on) return false;
    const std::string name = Name(key, width, height);
    Pack::Entry entry;
    {
        std::lock_guard<std::mutex> lock(p.m);
        auto it = p.files.find(name);
        if (it == p.files.end()) return false;
        entry = it->second;
    }
    std::vector<uint8_t> bytes;
    if (entry.size) {
        bytes.resize(entry.size);
        std::lock_guard<std::mutex> lock(p.archive_m);
        std::fseek(p.archive, long(entry.offset), SEEK_SET);
        bytes.resize(std::fread(bytes.data(), 1, bytes.size(), p.archive));
    } else {
        std::FILE* f = std::fopen(entry.path.c_str(), "rb");
        if (!f) {
            if (why) *why = "cannot open " + entry.path;
            return false;
        }
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (n > 0) {
            bytes.resize(size_t(n));
            bytes.resize(std::fread(bytes.data(), 1, bytes.size(), f));
        }
        std::fclose(f);
    }
    std::string error;
    if (!ParseDds(bytes.data(), bytes.size(), out, &error)) {
        if (why) *why = name + ": " + error;
        return false;
    }
    std::lock_guard<std::mutex> lock(p.m);
    if (out->width < width || out->height < height || out->width % width || out->height % height ||
        out->width / width != out->height / height) {
        if (why) *why = name + ": not the texture's shape at a whole scale";
        return false;
    }
    if (p.held + out->bytes.size() > p.budget) {
        if (why) *why = name + ": over the budget";
        return false;
    }
    p.held += out->bytes.size();
    return true;
}

namespace {
struct Readers {
    using Job = std::function<void()>;
    std::mutex m;
    std::condition_variable wake, idle;
    std::deque<Job> queue;
    std::vector<std::thread> threads;
    unsigned pending = 0;
    bool started = false, stop = false;
};
Readers& R() { static Readers& r = *new Readers; return r; }

void StopReaders() {
    Readers& r = R();
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lock(r.m);
        r.stop = true;
        r.pending -= std::min<unsigned>(r.pending, unsigned(r.queue.size()));
        r.queue.clear();
        threads.swap(r.threads);
    }
    r.wake.notify_all();
    for (std::thread& t : threads) t.join();
    r.idle.notify_all();
}

void ReaderLoop() {

    (void)svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3B);
    Readers& r = R();
    std::unique_lock<std::mutex> lock(r.m);
    for (;;) {
        r.wake.wait(lock, [&] { return r.stop || !r.queue.empty(); });
        if (r.stop) return;
        Readers::Job job = std::move(r.queue.front());
        r.queue.pop_front();
        lock.unlock();
        job();
        lock.lock();
        if (--r.pending == 0) r.idle.notify_all();
    }
}
}

void Async(std::function<void()> job) {
    Readers& r = R();
    std::unique_lock<std::mutex> lock(r.m);
    if (!r.started) {
        r.started = true;
        std::atexit(StopReaders);

        for (int i = 0; i < 2; ++i) {
            try { r.threads.emplace_back(ReaderLoop); }
            catch (const std::system_error&) {}
        }
    }
    if (r.stop) return;
    if (r.threads.empty()) {
        lock.unlock();
        job();
        return;
    }
    ++r.pending;
    r.queue.push_back(std::move(job));
    r.wake.notify_one();
}

void Hold() {
    Readers& r = R();
    std::lock_guard<std::mutex> lock(r.m);
    ++r.pending;
}
void Unhold() {
    Readers& r = R();
    std::lock_guard<std::mutex> lock(r.m);
    if (r.pending && --r.pending == 0) r.idle.notify_all();
}

void Released(uint64_t bytes) {
    Pack& p = P();
    std::lock_guard<std::mutex> lock(p.m);
    p.held -= std::min(p.held, bytes);
}

bool SmallProcess() {
    static const bool small = [] {
        u64 total = 0;
        svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        return total < (2ull << 30);
    }();
    return small;
}

}
