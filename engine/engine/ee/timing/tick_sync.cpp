#include "engine/ee/timing/tick_sync.h"

#include <limits>

uint64_t TickSync::FireInline(Cause cause) {
    if (!body_) return 0;

    return body_(cause);
}

void TickSync::NotePending(Cause) {
    pending_.fetch_add(1, std::memory_order_acq_rel);
}

int TickSync::FirePending() {
    int n = 0;
    while (FirePendingOne()) ++n;
    return n;
}

int TickSync::FirePendingOne() {
    for (;;) {
        uint64_t p = pending_.load(std::memory_order_acquire);
        if (p == 0) return 0;
        if (!pending_.compare_exchange_weak(p, p - 1, std::memory_order_acq_rel)) continue;
        FireInline(Cause::Deferred);
        return 1;
    }
}

