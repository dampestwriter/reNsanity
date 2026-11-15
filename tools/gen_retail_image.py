#!/usr/bin/env python3

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rspfile

IMAGE_START = 0x2D9D90
IMAGE_END = 0x3DB200

SECTIONS = [(".vutext", 0x2D9D90, 0x2E6ED0), (".data", 0x2E6F00, 0x2EC3D2), (".rodata", 0x2EC400, 0x309868),
            (".sdata", 0x309880, 0x30A460), (".sbss", 0x30A480, 0x30ACD8), (".bss", 0x30AD00, 0x3DB200)]

def read_symbol_addrs(path):
    syms = {}
    for line in path.read_text().splitlines():
        m = re.match(r"(\w+)\s*=\s*0x([0-9A-Fa-f]+);\s*(?://\s*(.*))?", line)
        if not m:
            continue
        name, addr, rest = m.group(1), int(m.group(2), 16), (m.group(3) or "")
        size = None
        ms = re.search(r"size:0x([0-9A-Fa-f]+)", rest)
        if ms:
            size = int(ms.group(1), 16)
        syms[name] = {"addr": addr, "func": "type:func" in rest or "jtbl_label" in rest, "size": size, "rest": rest}
    return syms

def undefined_symbols(nm, objects, rsp):
    und = set()
    r = subprocess.run([nm, *rspfile.tool_args(["-u", *objects], rsp)], capture_output=True, text=True)
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[-2] == "U":
            und.add(parts[-1])
    return und

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", required=True)
    ap.add_argument("--nm", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("objects", nargs="*")
    a = ap.parse_args(rspfile.expand_argv(sys.argv[1:]))

    syms = read_symbol_addrs(Path(a.game) / "symbol_addrs.txt")
    und = undefined_symbols(a.nm, a.objects, a.out + ".nm.rsp") if a.objects else set()

    labels = {}
    def add(name, addr, size):
        if not (IMAGE_START <= addr < IMAGE_END):
            return False
        labels.setdefault(addr, []).append((name, size))
        return True

    unresolved_outside = []
    for name, s in syms.items():
        if s["func"]:
            continue
        if not add(name, s["addr"], s["size"]):
            if name in und:
                unresolved_outside.append((name, hex(s["addr"])))
    for name in sorted(und):
        m = re.fullmatch(r"D_([0-9A-F]{8})", name)
        if m and name not in syms:
            if not add(name, int(m.group(1), 16), None):
                unresolved_outside.append((name, m.group(1)))

    addrs = sorted(labels)
    lines = ['\t.section\t.bss.te_retail_image,"",@', "\t.globl\tte_retail_image", "\t.p2align\t4", "te_retail_image:"]
    cur = IMAGE_START
    for i, addr in enumerate(addrs):
        if addr > cur:
            lines.append(f"\t.skip\t0x{addr - cur:X}")
            cur = addr
        nxt = addrs[i + 1] if i + 1 < len(addrs) else IMAGE_END
        for name, size in labels[addr]:
            sz = size if size else max(1, min(4, nxt - addr))
            sz = min(sz, IMAGE_END - addr)
            lines.append(f"\t.globl\t{name}")
            lines.append(f"{name}:")
            lines.append(f"\t.size\t{name}, 0x{sz:X}")
    if cur < IMAGE_END:
        lines.append(f"\t.skip\t0x{IMAGE_END - cur:X}")
    lines.append(f"\t.size\tte_retail_image, 0x{IMAGE_END - IMAGE_START:X}")
    lines.append("")
    Path(a.out).write_text("\n".join(lines))

    if unresolved_outside:
        print(f"gen_retail_image: symbols outside the image: {unresolved_outside}", file=sys.stderr)
    return 0

if __name__ == "__main__":
    sys.exit(main())
