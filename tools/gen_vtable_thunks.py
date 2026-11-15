#!/usr/bin/env python3

import argparse
import re
import subprocess
import sys
from pathlib import Path

CTYPE = {"i32": "unsigned int", "i64": "unsigned long long", "f32": "float", "f64": "double"}

def read_tsv(path):
    rows, vtables, conflicts = [], [], []
    for line in Path(path).read_text().splitlines():
        if not line.strip():
            continue
        if line.startswith("#vtable\t"):
            vtables.append(line.split("\t")[1])
        elif line.startswith("#conflict\t"):
            parts = line.split("\t")
            sym, slot, rest = parts[1], int(parts[2]), parts[3]
            sigs = [s.strip().split(" x")[0] for s in rest.split("|")]
            conflicts.append((sym, slot, sigs))
        elif line.startswith("#"):
            continue
        else:
            sym, slot, sig, classes, cpp, sites = (line.split("\t") + [""] * 6)[:6]
            rows.append((sym, int(slot), sig, classes, cpp, sites))
    return rows, vtables, conflicts

def parse_sig(sig):
    m = re.fullmatch(r"\((.*?)\)\s*->\s*(\w+)", sig)
    params = [p.strip() for p in m.group(1).split(",") if p.strip()]
    return params, m.group(2)

def sig_ident(sig):
    params, res = parse_sig(sig)
    return "_".join(params) + "__" + res

def read_symbol_addrs(path):
    syms = {}
    for line in Path(path).read_text().splitlines():
        m = re.match(r"(\w+)\s*=\s*0x([0-9A-Fa-f]+);", line)
        if m:
            syms[m.group(1)] = int(m.group(2), 16)
    return syms

def address_of(name, syms):
    if name in syms:
        return syms[name]
    m = re.fullmatch(r"D_([0-9A-Fa-f]{8})", name)
    return int(m.group(1), 16) if m else None

def typed_sigs(rows):
    return sorted({r[2] for r in rows if r[2] != "eabi"}, key=lambda s: (len(parse_sig(s)[0]), s))

def emit_wasm(rows, pool, out):
    sigs = typed_sigs(rows)
    L = ["typedef unsigned int te_u32; typedef unsigned long long te_u64; typedef float te_f32; typedef double te_f64;",
         'extern "C" te_u64 te_vt_dispatch(te_u32 fn, const te_u64* ints, const te_f32* floats) __asm__("CallEabiTrampoline");',
         'extern "C" te_f32 te_vt_dispatch_f(te_u32 fn, const te_u64* ints, const te_f32* floats) __asm__("CallEabiTrampolineF");',
         'extern "C" void te_vt_pure_body(void) __asm__("AbstractMethodCallException");',
         f"#define TE_VT_POOL {pool}u",
         f'extern "C" __attribute__((used, visibility("default"))) te_u32 te_vt_targets[{len(sigs) * pool}] = {{}};',
         ""]
    desc = []
    target = 0
    for si, sig in enumerate(sigs):
        params, res = parse_sig(sig)
        ident = sig_ident(sig)
        ptypes = [CTYPE[p] for p in params]
        plist = ", ".join(f"{t} a{i}" for i, t in enumerate(ptypes)) or "void"
        rtype = "void" if res == "nil" else CTYPE[res]
        pack = []
        ni = 0
        for i, p in enumerate(params):
            if p == "i32":
                pack.append(f"I[{ni}] = (te_u64)(long long)(int)a{i};"); ni += 1
            elif p == "i64":
                pack.append(f"I[{ni}] = a{i};"); ni += 1
            else:
                raise SystemExit(f"gen_vtable_thunks: float parameter in a typed slot signature {sig}")
        if ni > 8:
            raise SystemExit(f"gen_vtable_thunks: {sig} needs more than 8 integer registers")
        for k in range(pool):
            name = f"te_vt_{ident}_{k}"
            body = [f"te_u64 I[8] = {{}}; te_f32 F[8] = {{}};", *pack]
            call = f"te_vt_dispatch(te_vt_targets[{target}], I, F)"
            if res == "nil":
                body.append(f"(void){call};")
            elif res == "f32":
                body.append(f"return te_vt_dispatch_f(te_vt_targets[{target}], I, F);")
            elif res == "i64":
                body.append(f"return {call};")
            elif res == "f64":
                body.append(f"union {{ te_u64 i; te_f64 d; }} u; u.i = {call}; return u.d;")
            else:
                body.append(f"return (te_u32){call};")
            L.append(f'extern "C" __attribute__((used, noinline)) {rtype} {name}({plist}) {{ {" ".join(body)} }}')
            desc.append((si, 0, target, name))
            target += 1

        name = f"te_vt_pure_{ident}"
        ret = "" if res == "nil" else (" return 0.0f;" if res == "f32" else (" return 0.0;" if res == "f64" else " return 0;"))
        L.append(f'extern "C" __attribute__((used, noinline)) {rtype} {name}({plist}) {{ te_vt_pure_body();{ret} }}')
        desc.append((si, 1, 0, name))
        L.append("")
    L.append("struct te_vt_thunk { te_u32 sig; te_u32 kind; te_u32 target; void (*function)(void); };")
    L.append(f'extern "C" __attribute__((used, visibility("default"))) const te_vt_thunk te_vt_thunks[{len(desc)}] = {{')
    for si, kind, target, name in desc:
        L.append(f"    {{{si}u, {kind}u, {target}u, (void (*)(void))&{name}}},")
    L.append("};")
    L.append(f'extern "C" __attribute__((used, visibility("default"))) const te_u32 te_vt_thunk_count = {len(desc)}u;')
    L.append(f'extern "C" __attribute__((used, visibility("default"))) const te_u32 te_vt_signature_count = {len(sigs)}u;')
    L.append("")
    Path(out).write_text("\n".join(L))
    return sigs, len(desc)

def module_types(wasm_objdump, wasm):
    r = subprocess.run([wasm_objdump, "-x", "-j", "Type", wasm], capture_output=True, text=True, check=True)
    types = {}
    for line in r.stdout.splitlines():
        m = re.match(r"\s*-\s*type\[(\d+)\]\s*\((.*?)\)\s*->\s*(.*)", line)
        if m:
            params = ", ".join(p.strip() for p in m.group(2).split(",") if p.strip())
            res = m.group(3).strip()
            types[f"({params}) -> {res}"] = int(m.group(1))
    return types

def emit_host(rows, vtables, conflicts, syms, types, module, out, pure_name="AbstractMethodCallException"):
    sigs = typed_sigs(rows)
    sig_index = {s: i for i, s in enumerate(sigs)}

    starts = sorted({a for a in (address_of(v, syms) for v in vtables) if a is not None} |
                    {a for a in (address_of(r[0], syms) for r in rows) if a is not None})

    conflict_map = {(sym, slot): sigs_ for sym, slot, sigs_ in conflicts}
    table = []
    dropped_no_type, dropped_no_addr = [], []
    for sym, slot, sig, classes, cpp, sites in rows:
        if sig == "eabi":
            continue
        addr = address_of(sym, syms)
        if addr is None:
            dropped_no_addr.append(sym)
            continue
        prefs = [sig] + [s for s in conflict_map.get((sym, slot), []) if s != sig and s != "eabi"]
        tids = [types[s] for s in prefs if s in types]
        if not tids:
            dropped_no_type.append((sym, slot, sig))
            continue
        table.append((addr, slot, sig_index[sig], tids))
    table.sort()
    L = ["#include <stdint.h>", "#include <string.h>", f'#include "{module}.h"', "",
         "#if defined(__GNUC__) || defined(__clang__)", "#define TE_FUNC_TYPE_DECL(x) extern const char* const x",
         "#else", "#define TE_FUNC_TYPE_DECL(x) extern const char x[]", "#endif"]
    for t in sorted(types.values()):
        L.append(f"TE_FUNC_TYPE_DECL(w2c_{module}_t{t});")

    L += ["", "struct te_vt_type_name_row { const char* const* id; const char* name; };",
          f"const struct te_vt_type_name_row te_vt_type_names[{max(1, len(types))}] = {{"]
    for sig, t in sorted(types.items(), key=lambda kv: kv[1]):
        L.append(f'    {{&w2c_{module}_t{t}, "{sig}"}},')
    if not types:
        L.append("    {0, \"\"},")
    L += ["};", f"const uint32_t te_vt_type_name_count = {len(types)}u;"]
    L += ["", "struct te_vt_slot_row { uint32_t vtable; uint16_t slot; uint16_t sig; const char* const* types[4]; uint32_t ntypes; };",
          f"const struct te_vt_slot_row te_vt_slot_rows[{max(1, len(table))}] = {{"]
    for addr, slot, si, tids in table:
        tt = [f"&w2c_{module}_t{t}" for t in tids[:4]] + ["0"] * (4 - min(4, len(tids)))
        L.append(f"    {{0x{addr:08X}u, {slot}, {si}, {{{', '.join(tt)}}}, {min(4, len(tids))}u}},")
    if not table:
        L.append("    {0, 0, 0, {0, 0, 0, 0}, 0},")
    L += ["};", f"const uint32_t te_vt_slot_row_count = {len(table)}u;", "",
          f"const uint32_t te_vt_vtable_starts[{max(1, len(starts))}] = {{"]
    for i in range(0, len(starts), 8):
        L.append("    " + ", ".join(f"0x{x:08X}u" for x in starts[i:i + 8]) + ",")
    if not starts:
        L.append("    0,")
    L += ["};", f"const uint32_t te_vt_vtable_start_count = {len(starts)}u;", "",
          f"const char* const te_vt_signatures[{max(1, len(sigs))}] = {{"]
    for s in sigs:
        L.append(f'    "{s}",')
    if not sigs:
        L.append('    "",')
    L += ["};", f"const uint32_t te_vt_signature_count_host = {len(sigs)}u;",
          f"const uint32_t te_vt_pure_virtual_address = 0x{address_of(pure_name, syms) or 0:08X}u;  // {pure_name}", ""]
    Path(out).write_text("\n".join(L))
    return table, dropped_no_type, dropped_no_addr, starts

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tsv", required=True)
    ap.add_argument("--pool", type=int, default=32)
    ap.add_argument("--wasm-out", default=None)
    ap.add_argument("--host-out", default=None)
    ap.add_argument("--wasm", default=None, help="the linked module (host mode)")
    ap.add_argument("--wasm-objdump", default=None)
    ap.add_argument("--symbols", default=None, help="game/symbol_addrs.txt (host mode)")
    ap.add_argument("--module", default="game")
    a = ap.parse_args()
    rows, vtables, conflicts = read_tsv(a.tsv)
    if a.wasm_out:
        emit_wasm(rows, a.pool, a.wasm_out)
    if a.host_out:
        if not (a.wasm and a.wasm_objdump and a.symbols):
            raise SystemExit("gen_vtable_thunks: --host-out needs --wasm, --wasm-objdump and --symbols")
        types = module_types(a.wasm_objdump, a.wasm)
        syms = read_symbol_addrs(a.symbols)
        emit_host(rows, vtables, conflicts, syms, types, a.module, a.host_out)
    return 0

if __name__ == "__main__":
    sys.exit(main())
