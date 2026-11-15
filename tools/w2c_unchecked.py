#!/usr/bin/env python3

import sys
from pathlib import Path

OLD = """#if WASM_RT_MEMCHECK_GUARD_PAGES
#define MEMCHECK_DEFAULT32(mem, local_memory_size, a, t) \\
  WASM_RT_CHECK_BASE(mem);
#else"""
NEW = """#if WASM_RT_MEMCHECK_GUARD_PAGES || defined(TE_W2C_UNCHECKED_MEMORY)
#define MEMCHECK_DEFAULT32(mem, local_memory_size, a, t) \\
  WASM_RT_CHECK_BASE(mem);
#else"""

BASE_OLD = """#else
#define MEM_ADDR_MEMOP(mem, addr, n) MEM_ADDR(mem, addr, n)
#endif"""
BASE_NEW = """#elif defined(TE_W2C_STATIC_RAM) && defined(TE_W2C_UNCHECKED_MEMORY) && !WABT_BIG_ENDIAN
extern uint8_t te_ee_ram[] __attribute__((visibility("hidden")));
static inline uint8_t* te_ee_ram_base(void) {
  uint8_t* p;
  __asm__("adrp %0, te_ee_ram" : "=r"(p));
  return p;
}
#define MEM_ADDR_MEMOP(mem, addr, n) (&te_ee_ram_base()[(addr)])
#elif defined(TE_W2C_UNCHECKED_MEMORY) && !WABT_BIG_ENDIAN
#define MEM_ADDR_MEMOP(mem, addr, n) (&wasm_rt_local_memory_base[(addr)])
#else
#define MEM_ADDR_MEMOP(mem, addr, n) MEM_ADDR(mem, addr, n)
#endif"""

def rewrite(s, old, new, what):
    if new in s:
        return s
    if s.count(old) != 1:
        sys.stderr.write("w2c_unchecked: game-impl.h does not have the expected %s shape\n" % what)
        sys.exit(1)
    return s.replace(old, new, 1)

def main():
    p = Path(sys.argv[1])
    s = p.read_text()
    t = rewrite(s, OLD, NEW, "MEMCHECK_DEFAULT32")
    t = rewrite(t, BASE_OLD, BASE_NEW, "MEM_ADDR_MEMOP")
    if t != s:
        p.write_text(t)
    return 0

if __name__ == "__main__":
    sys.exit(main())
