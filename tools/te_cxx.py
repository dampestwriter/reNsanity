#!/usr/bin/env python3

import argparse
import re
import subprocess
import sys
from pathlib import Path

CXXFLAGS_BASE = [
    "-std=gnu++26", "-D_EE", "-fno-exceptions", "-fno-rtti", "-fno-threadsafe-statics",
    "-fno-asynchronous-unwind-tables", "-fno-common", "-fno-strict-aliasing", "-ffunction-sections",
    "-fdata-sections", "-ffp-contract=off", "-fno-delete-null-pointer-checks", "-Wall", "-Wno-invalid-offsetof",

]

RE_FENCE = re.compile(r'asm\s*\(\s*""\s*:\s*"\+f"\s*\(\s*([A-Za-z_]\w*)\s*\)\s*\)\s*;')

RE_THUNK = re.compile(r'asm\s*\(\s*\(\s*Abi::Thunk<.*?>\s*\(\s*Abi::Direction::(Export|Import)\s*,\s*"(\w+)"\s*\)\s*\)\s*\)\s*;',
                      re.DOTALL)

RE_RAW = re.compile(r'asm\s*(?:volatile\s*)?\(\s*R"\((.*?)\)"\s*\)\s*;', re.DOTALL)
RE_RAW_GLOBL = re.compile(r'^\s*\.globl\s+(\w+)', re.MULTILINE)
RE_RAW_JUMP = re.compile(r'^\s*(?:jal|j)\s+(\w+)', re.MULTILINE)

RE_TOUCH = re.compile(r'asm\s+volatile\s*\(\s*"lb \$0, 0\(%0\)"\s*:\s*:\s*"r"\s*\(\s*([A-Za-z_]\w*)\s*\)\s*\)\s*;')

RE_LQSQ = re.compile(r'asm\s+volatile\s*\(\s*"lq \$8, 0\(%1\)\\n\\t"\s*"sq \$8, 0\(%0\)"\s*:\s*:\s*"r"\s*\(\s*(\w+)\s*\)\s*,'
                     r'\s*"r"\s*\(\s*(\w+)\s*\)\s*:\s*"\$8"\s*,\s*"memory"\s*\)\s*;')

RE_TRAMP_F = re.compile(r'(f32\s+CallEabiFloat\s*\([^;]*?\)\s*asm\s*\(\s*"CallEabiTrampoline)(")')

RE_PREFETCH = re.compile(r'(?<![\w.])__builtin_prefetch\s*\(')
PREFETCH_DECL = 'extern "C" void ps2e_cache_prefetch(unsigned int);\n'


def preprocess(clang, flags, src):
    cmd = [clang, "-E", *flags, str(src)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        return None, r.stderr
    return r.stdout, r.stderr

def rewrite(text):
    def fence(m):
        return '__asm__("" : "+m"(%s));' % m.group(1)
    text = RE_FENCE.sub(fence, text)

    def thunk(m):
        direction, name = m.group(1), m.group(2)
        if direction == "Export":
            return 'extern "C" void te_alias_%s(void) asm("%s") __attribute__((alias("%s_n32")));' % (name, name, name)
        return ";"
    text = RE_THUNK.sub(thunk, text)

    def raw(m):
        body = m.group(1)
        globls = RE_RAW_GLOBL.findall(body)
        jumps = RE_RAW_JUMP.findall(body)
        aliases = [(g, g + "_n32") for g in globls if (g + "_n32") in jumps]
        return " ".join('extern "C" void te_alias_%s(void) asm("%s") __attribute__((alias("%s")));' % (a[0], a[0], a[1])
                        for a in aliases) or ";"
    text = RE_RAW.sub(raw, text)

    def touch(m):
        return "(void)(%s);" % m.group(1)
    text = RE_TOUCH.sub(touch, text)

    def lqsq(m):
        to, frm = m.group(1), m.group(2)
        return ("__builtin_memcpy((void*)((unsigned int)(%s) & ~15u), (const void*)((unsigned int)(%s) & ~15u), 16);"
                % (to, frm))
    text = RE_LQSQ.sub(lqsq, text)

    def prefetches(text):
        out, i, n = [], 0, 0
        for m in RE_PREFETCH.finditer(text):
            if m.start() < i:
                continue
            depth, j, args, start = 1, m.end(), [], m.end()
            while depth:
                c = text[j]
                if c in "([{":
                    depth += 1
                elif c in ")]}":
                    depth -= 1
                elif c == "," and depth == 1:
                    args.append(text[start:j])
                    start = j + 1
                j += 1
            args.append(text[start:j - 1])
            out.append(text[i:m.start()])
            out.append("ps2e_cache_prefetch((unsigned int)(%s))" % args[0].strip())
            i, n = j, n + 1
        out.append(text[i:])
        return ("".join(out), n) if n else (text, 0)
    text, prefetched = prefetches(text)
    if prefetched:
        text = PREFETCH_DECL + text
    return RE_TRAMP_F.sub(r"\1F\2", text)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wasi-sdk", required=True)
    ap.add_argument("--game", required=True, help="the game/ tree's root (has include/)")
    ap.add_argument("--shim", required=True, help="platform/nativeee/sdk_shim")
    ap.add_argument("--std", default="gnu++26")
    ap.add_argument("-O", dest="opt", default="2")
    ap.add_argument("-I", dest="incs", action="append", default=[])
    ap.add_argument("-D", dest="defs", action="append", default=[])
    ap.add_argument("-o", dest="out", required=True)
    ap.add_argument("src")
    a = ap.parse_args()

    sdk = Path(a.wasi_sdk)
    clang = str(sdk / "bin" / "clang++")
    sysroot = str(sdk / "share" / "wasi-sysroot")
    target = ["--target=wasm32-wasi", f"--sysroot={sysroot}"]
    flags = [*target, *CXXFLAGS_BASE, f"-O{a.opt}"]
    flags = [f if not f.startswith("-std=") else f"-std={a.std}" for f in flags]
    inc = [f"-I{Path(a.game) / 'include'}", f"-I{a.shim}", *[f"-I{i}" for i in a.incs]]
    defs = [f"-D{d}" for d in a.defs]

    pp, err = preprocess(clang, [*flags, *inc, *defs], a.src)
    if pp is None:
        sys.stderr.write(err)
        return 1
    text = rewrite(pp)
    r = subprocess.run([clang, *flags, "-x", "c++", "-c", "-o", a.out, "-"], input=text, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stderr)
    return 0 if r.returncode == 0 else 1

if __name__ == "__main__":
    sys.exit(main())
