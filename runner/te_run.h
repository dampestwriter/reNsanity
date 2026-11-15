#pragma once
#ifdef __cplusplus
extern "C" {
#endif
struct w2c_game;

int te_run_entry(struct w2c_game* instance);
const char* te_trap_name(int trap);
#ifdef __cplusplus
}
#endif
