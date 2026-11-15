#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct te_fiber_ctx {
    uint64_t sp;
    uint64_t x[12];
    uint64_t d[8];
} te_fiber_ctx;

void te_fiber_switch(te_fiber_ctx* save, const te_fiber_ctx* load);

void te_fiber_prepare(te_fiber_ctx* ctx, void* stack, size_t size, void (*fn)(void*), void* arg);

void* te_fiber_map_stack(uint64_t hint, size_t size, int* at_hint);

#define te_fiber_guard_bytes ((size_t)65536)

#ifdef __cplusplus
}
#endif
