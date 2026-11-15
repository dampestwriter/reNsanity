#pragma once
#include <chrono>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>

extern "C" void te_switch_thread_named(const char*);
#include "engine/platform/host/platform.h"
#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <vector>

class IsoReadCache {
public:
    static constexpr size_t kPage = 256u << 10, kBudget = 64u << 20;
    using Reader = std::function<size_t(uint64_t, size_t, uint8_t*)>;
    IsoReadCache(uint64_t size, Reader read) : size_(size), read_(std::move(read)), worker_([this] { Run(); }) {
        static const bool registered = [] { std::atexit(StopAll); return true; }();
        (void)registered;
        std::lock_guard<std::mutex> l(LiveMutex());
        Live().push_back(this);
    }
    ~IsoReadCache() {
        {
            std::lock_guard<std::mutex> l(LiveMutex());
            Live().erase(std::find(Live().begin(), Live().end(), this));
        }
        Stop();
    }

    void Stop() {
        { std::lock_guard<std::mutex> l(mutex_); stop_ = true; }
        wake_.notify_all();
        std::lock_guard<std::mutex> l(join_mutex_);
        if (worker_.joinable()) worker_.join();
    }
    void Prefetch(uint64_t off, size_t n) {
        if (off >= size_ || !n) return;
        const uint64_t end = off + std::min<uint64_t>(n, size_ - off);
        std::lock_guard<std::mutex> l(mutex_);
        for (uint64_t at = off / kPage * kPage; at < end; at += kPage) {
            if (pages_.count(at)) continue;
            if (!Room()) break;
            auto e = Add(at); queue_.push_back(e);
        }
        wake_.notify_one();
    }
    size_t Read(uint64_t off, size_t n, uint8_t* dst) {
        size_t done = 0;
        while (done < n && off + done < size_) {
            const uint64_t at = (off + done) / kPage * kPage;
            std::shared_ptr<Page> e;
            {
                std::unique_lock<std::mutex> l(mutex_);
                if (stop_) {
                    l.unlock();
                    done += read_(off + done, size_t(std::min<uint64_t>(n - done, size_ - (off + done))), dst + done);
                    break;
                }
                auto it = pages_.find(at);
                if (it == pages_.end()) {
                    while (!Room()) wake_.wait(l);
                    e = Add(at); queue_.push_front(e); wake_.notify_one();
                } else {
                    e = it->second;
                    if (!e->started) { queue_.push_front(e); wake_.notify_one(); }
                }
                e->used = ++clock_;
                while (!e->done && !(stop_ && !e->started)) wake_.wait(l);
                if (!e->done) continue;
            }
            const size_t within = size_t(off + done - at);
            if (within >= e->got) break;
            const size_t take = std::min(n - done, e->got - within);
            std::memcpy(dst + done, e->bytes.get() + within, take);
            done += take;

            e.reset(); wake_.notify_all();
            if (take == 0) break;
        }
        return done;
    }
private:

    static std::mutex& LiveMutex() { static auto* m = new std::mutex; return *m; }
    static std::vector<IsoReadCache*>& Live() { static auto* v = new std::vector<IsoReadCache*>; return *v; }
    static void StopAll() {
        std::lock_guard<std::mutex> l(LiveMutex());
        for (IsoReadCache* c : Live()) c->Stop();
    }
    struct Free { void operator()(uint8_t* p) const { ps2r::host::AlignedFree(p); } };
    struct Page {
        uint64_t off = 0, used = 0;
        bool started = false, done = false;
        size_t got = 0;
        std::unique_ptr<uint8_t, Free> bytes;
    };
    bool Room() {
        if (pages_.size() < kBudget/kPage) return true;
        auto oldest = pages_.end();
        for (auto i = pages_.begin(); i != pages_.end(); ++i)
            if (i->second.use_count() == 1 && i->second->done &&
                (oldest == pages_.end() || i->second->used < oldest->second->used)) oldest = i;
        if (oldest == pages_.end()) return false;
        pages_.erase(oldest); return true;
    }
    std::shared_ptr<Page> Add(uint64_t off) {
        auto p = std::make_shared<Page>(); p->off = off; p->used = ++clock_;
        pages_.emplace(off, p); return p;
    }
    void Run() {
        ps2r::platform::host::PinCurrentThreadInteractive("te-io");
        te_switch_thread_named("io");
        for (;;) {
            std::shared_ptr<Page> e;
            {
                std::unique_lock<std::mutex> l(mutex_);
                wake_.wait(l, [&] { return stop_ || !queue_.empty(); });
                if (stop_) return;
                e = queue_.front(); queue_.pop_front();
                if (e->started) continue;
                e->started = true;
            }
            const size_t n = size_t(std::min<uint64_t>(kPage, size_ - e->off));
            e->bytes.reset(static_cast<uint8_t*>(ps2r::host::AlignedAlloc(4096, n)));
            if (!e->bytes) { std::fprintf(stderr, "[io.cache] allocation failed\n"); std::abort(); }
            const size_t got = read_(e->off, n, e->bytes.get());
            {
                std::lock_guard<std::mutex> l(mutex_);
                e->got = got; e->done = true;
            }
            e.reset(); wake_.notify_all();
        }
    }
    uint64_t size_;
    Reader read_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::map<uint64_t, std::shared_ptr<Page>> pages_;
    std::deque<std::shared_ptr<Page>> queue_;
    bool stop_ = false;
    std::mutex join_mutex_;
    uint64_t clock_ = 0;
    std::thread worker_;
};
