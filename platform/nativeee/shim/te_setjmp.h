#ifndef TE_SHIM_SETJMP_H
#define TE_SHIM_SETJMP_H
#include <setjmp.h>
#ifndef sigsetjmp
#define sigsetjmp(env, savemask) setjmp(env)
#endif
#ifndef siglongjmp
#define siglongjmp(env, val) longjmp(env, val)
#endif
#endif
