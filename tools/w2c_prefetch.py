#!/usr/bin/env python3

import re
import sys
from pathlib import Path

MACRO = """#define TE_W2C_PREFETCH_SPAN 0x2000000u
#if !WABT_BIG_ENDIAN && !WASM_RT_USE_SEGUE_FOR_THIS_MODULE && !WASM_RT_USE_LOCAL_MEMORY_BASE_SIZE_FOR_THIS_MODULE && \\
    defined(TE_W2C_STATIC_RAM) && defined(TE_W2C_UNCHECKED_MEMORY) && TE_W2C_PREFETCH_HAS_STATIC_RAM
#define TE_W2C_PREFETCH(a) __builtin_prefetch(&te_ee_ram_base()[(u32)(a) & (TE_W2C_PREFETCH_SPAN - 1u)])
#elif !WABT_BIG_ENDIAN
#define TE_W2C_PREFETCH(a) __builtin_prefetch(&wasm_rt_local_memory_base[(u32)(a) & (TE_W2C_PREFETCH_SPAN - 1u)])
#else
#define TE_W2C_PREFETCH(a) ((void)(a))
#endif
"""
DECL = "void w2c_env_ps2e_cache_prefetch(struct w2c_env*, u32);"
CALL = re.compile(r"\bw2c_env_ps2e_cache_prefetch\(instance->w2c_env_instance, (var_[A-Za-z0-9_]+)\);")
ANY_CALL = re.compile(r"\bw2c_env_ps2e_cache_prefetch\(instance->")

def main():
    impl = Path(sys.argv[1])
    h = impl.read_text()
    game_h = (impl.parent / "game.h").read_text() if (impl.parent / "game.h").exists() else ""
    if DECL not in game_h and DECL not in h:
        return 0
    if "#define TE_W2C_PREFETCH_SPAN" not in h:
        static = "static inline uint8_t* te_ee_ram_base(void)" in h
        h = h.rstrip("\n") + "\n\n#define TE_W2C_PREFETCH_HAS_STATIC_RAM %d\n" % static + MACRO
        impl.write_text(h)
    for part in sys.argv[2:]:
        p = Path(part)
        s = p.read_text()
        s, n = CALL.subn(r"TE_W2C_PREFETCH(\1);", s)
        if ANY_CALL.search(s):
            sys.stderr.write("w2c_prefetch: %s calls the prefetch import in a shape this script does not know\n" % part)
            return 1
        if n:
            p.write_text(s)
    return 0

if __name__ == "__main__":
    sys.exit(main())
