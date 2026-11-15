#include "te_fiber.h"

#include <string.h>
#include <stdlib.h>

extern void te_fiber_trampoline(void);

_Static_assert(offsetof(te_fiber_ctx, x) == 8 && offsetof(te_fiber_ctx, d) == 104, "te_fiber_ctx: x19 at 8, d8 at 104 (te_fiber_switch.S)");

void te_fiber_prepare(te_fiber_ctx* ctx, void* stack, size_t size, void (*fn)(void*), void* arg) {
    memset(ctx, 0, sizeof *ctx);
    uint64_t top = ((uint64_t)(uintptr_t)stack + size) & ~(uint64_t)15;
    ctx->sp = top;
    ctx->x[0] = (uint64_t)(uintptr_t)fn;
    ctx->x[1] = (uint64_t)(uintptr_t)arg;
    ctx->x[10] = 0;
    ctx->x[11] = (uint64_t)(uintptr_t)te_fiber_trampoline;
}

void* te_fiber_map_stack(uint64_t hint, size_t size, int* at_hint) {
    const size_t guard = te_fiber_guard_bytes;
    void* want = (void*)(uintptr_t)(hint - guard);

    (void)want;
    void* p = malloc(size + guard);
    if (!p) return NULL;
    want = NULL;
    if (at_hint) *at_hint = (p == want);
    return (char*)p + guard;
}
