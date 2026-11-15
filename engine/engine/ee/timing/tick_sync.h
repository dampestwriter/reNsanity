#pragma once

#include <atomic>
#include <cstdint>
#include <functional>

class TickSync {
public:
    enum class Cause : uint8_t { Boundary = 0, CdvdDelivery, Demand, Deferred, Count };
    using Body = std::function<uint64_t(Cause)>;

    void SetBody(Body body) { body_ = std::move(body); }

    uint64_t FireInline(Cause cause);

    void NotePending(Cause cause);

    int FirePending();

    int FirePendingOne();

    uint64_t Pending() const { return pending_.load(std::memory_order_acquire); }

private:
    Body body_;
    std::atomic<uint64_t> pending_{0};
};
