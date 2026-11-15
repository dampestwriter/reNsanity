#!/usr/bin/env python3

import argparse
import re
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rspfile

def read_symbol_addrs(path):
    syms = {}
    for line in path.read_text().splitlines():
        m = re.match(r"(\w+)\s*=\s*0x([0-9A-Fa-f]+);\s*(?://\s*(.*))?", line)
        if m:
            rest = m.group(3) or ""
            syms[m.group(1)] = (int(m.group(2), 16), "type:func" in rest)
    return syms

def defined_functions(nm, objects, rsp):
    defined = set()
    r = subprocess.run([nm, *rspfile.tool_args(["--defined-only", *objects], rsp)], capture_output=True, text=True)
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in ("T", "t", "W"):
            defined.add(parts[2])
    return defined

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", required=True)
    ap.add_argument("--nm", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("objects", nargs="+")
    a = ap.parse_args(rspfile.expand_argv(sys.argv[1:]))

    syms = read_symbol_addrs(Path(a.game) / "symbol_addrs.txt")
    defined = defined_functions(a.nm, a.objects, a.out + ".nm.rsp")

    entries = {}
    for name, (addr, is_func) in syms.items():
        if not is_func:
            continue
        if name in defined:
            entries[addr] = name
        elif name + "_n32" in defined:
            entries[addr] = name + "_n32"

    for name in sorted(defined):
        m = re.fullmatch(r"(?:func|FUN)_([0-9A-Fa-f]{8})(_n32)?", name)
        if not m:
            continue
        addr = int(m.group(1), 16)
        if not (0x00100000 <= addr < 0x002D9D90) or addr in entries:
            continue
        if m.group(2) and name[:-4] in defined:
            continue
        entries[addr] = name

    addrs = sorted(entries)
    L = ["typedef unsigned int te_u32;", "typedef void (*te_fn)(void);",
         "struct te_retail_function { te_u32 address; te_fn function; };"]
    for i, addr in enumerate(addrs):
        L.append(f'extern void te_rf_{i}(void) __asm__("{entries[addr]}");')
    L.append("__attribute__((used, visibility(\"default\"))) const struct te_retail_function te_retail_functions[] = {")
    for i, addr in enumerate(addrs):
        L.append(f"    {{0x{addr:08X}u, &te_rf_{i}}},")
    L.append("};")
    L.append(f"__attribute__((used, visibility(\"default\"))) const te_u32 te_retail_function_count = {len(addrs)}u;")
    L.append("")
    Path(a.out).write_text("\n".join(L))
    return 0

if __name__ == "__main__":
    sys.exit(main())
