#!/usr/bin/env python3

import argparse
import re
import subprocess
import sys

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wasm-objdump", required=True)
    ap.add_argument("--wasm", required=True)
    ap.add_argument("--imports", required=True)
    ap.add_argument("--image-start", type=int, required=True)
    ap.add_argument("--image-end", type=int, required=True)
    a = ap.parse_args()
    allowed = {l.strip() for l in open(a.imports) if l.strip()}
    dump = subprocess.run([a.wasm_objdump, "-x", a.wasm], capture_output=True, text=True, check=True).stdout
    imports, bad, memory_import = [], [], None
    for m in re.finditer(r"- (func|memory|table|global)\[\d+\].*?<- (\S+)\.(\S+)", dump):
        kind, mod, name = m.groups()
        if kind == "memory":
            memory_import = f"{mod}.{name}"
            continue
        imports.append(f"{mod}.{name}")
        if mod != "env" or name not in allowed or kind != "func":
            bad.append(f"{kind} {mod}.{name}")
    exports = re.findall(r'-> "([^"]+)"', dump)
    seg0 = re.search(r"- segment\[0\] <[^>]*> memory=0 size=(\d+) - init i32=(\d+)", dump)
    problems = []
    if memory_import != "env.memory":
        problems.append(f"memory must be imported as env.memory (got {memory_import})")
    if bad:
        problems.append("imports outside the seam: " + ", ".join(bad))
    for e in ("entry", "te_retail_image", "te_retail_functions", "te_retail_function_count", "__indirect_function_table"):
        if e not in exports:
            problems.append(f"missing export {e}")
    globs = {m.group(1): int(m.group(2)) for m in re.finditer(r"<([^>]+)> - init i32=(\d+)", dump)}
    if globs.get("te_retail_image") != a.image_start:
        problems.append(f"the retail image must sit at {a.image_start} (got {globs.get('te_retail_image')})")
    for name in ("__data_end", "__heap_base"):
        if globs.get(name) != a.image_end:
            problems.append(f"{name} must be the retail image's end {a.image_end} (got {globs.get(name)}): the C++'s data must sit below the image")
    if seg0 and int(seg0.group(2)) + int(seg0.group(1)) > a.image_start:
        problems.append(f"the first data segment ({seg0.group(2)}+{seg0.group(1)}) overlaps the retail image")
    if problems:
        print("check_module: FAILED\n  " + "\n  ".join(problems), file=sys.stderr)
        return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
