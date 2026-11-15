#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
static inline int DIntr(void) { return 1; }
static inline int EIntr(void) { return 1; }

void* EndOfHeap(void);

int kputs(char* text);
#ifdef __cplusplus
}
#endif
