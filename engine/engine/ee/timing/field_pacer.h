#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

class BoundaryProtocol;
class TickSync;

class FieldPacer {
public:

    FieldPacer(TickSync& ticks, BoundaryProtocol& protocol, uint32_t hz = 60, std::function<void()> on_thread_start = {});
    ~FieldPacer();
    FieldPacer(const FieldPacer&) = delete;
    FieldPacer& operator=(const FieldPacer&) = delete;

    void Start();
    void Stop();
    bool Running() const { return thread_.joinable(); }
    uint32_t Hz() const { return hz_; }

    uint64_t Ticks() const { return ticks_noted_.load(std::memory_order_relaxed); }


    static int64_t EdgeNs(int64_t anchor_ns, uint64_t k, uint32_t hz);

    bool EdgeOfTickNs(uint64_t tick, int64_t* edge_ns) const;

    int64_t PeriodNs() const { return static_cast<int64_t>(1000000000ull / (hz_ == 0 ? 60u : hz_)); }

private:
    void Loop();

    TickSync& ticks_;
    BoundaryProtocol& protocol_;
    uint32_t hz_;
    std::function<void()> on_thread_start_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> ticks_noted_{0};

    std::atomic<int64_t> last_edge_ns_{0};
    std::atomic<uint64_t> last_edge_tick_{0};
};
