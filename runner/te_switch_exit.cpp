#include <switch.h>

#include <vector>

extern "C" {
int __real_atexit(void (*fn)(void));
[[noreturn]] void __real_exit(int code);
}

namespace {
Mutex g_lock;
std::vector<void (*)(void)>* g_more = nullptr;
}

extern "C" int __wrap_atexit(void (*fn)(void)) {
    if (__real_atexit(fn) == 0) return 0;
    mutexLock(&g_lock);
    if (!g_more) g_more = new std::vector<void (*)(void)>;
    g_more->push_back(fn);
    mutexUnlock(&g_lock);
    return 0;
}

extern "C" [[noreturn]] void __wrap_exit(int code) {
    for (;;) {
        void (*fn)(void) = nullptr;
        mutexLock(&g_lock);
        if (g_more && !g_more->empty()) { fn = g_more->back(); g_more->pop_back(); }
        mutexUnlock(&g_lock);
        if (!fn) break;
        fn();
    }
    __real_exit(code);
}
