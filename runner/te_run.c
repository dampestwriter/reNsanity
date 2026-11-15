#include "te_run.h"

#include "game.h"
#include "wasm-rt.h"
#include "wasm-rt-exceptions.h"
#include "wasm-rt-impl.h"

int te_run_entry(struct w2c_game* instance) {
    wasm_rt_trap_t trap = (wasm_rt_trap_t)wasm_rt_impl_try();
    if (trap != 0) {
        return (int)trap;
    }
    w2c_game_entry(instance);
    return -1;
}

const char* te_trap_name(int trap) {
    return wasm_rt_strerror((wasm_rt_trap_t)trap);
}
