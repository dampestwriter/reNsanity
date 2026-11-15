#!/usr/bin/env python3

import argparse
import re
import subprocess
import sys
from pathlib import Path

CTYPE = {"i32": "u32", "i64": "u64", "f32": "f32", "f64": "f64"}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wasm-objdump", required=True)
    ap.add_argument("--wasm", required=True)
    ap.add_argument("--module", default="game")
    ap.add_argument("--out", required=True)
    ap.add_argument("--adapt-header", default=None, help="prototypes of the call-site adapters (included by game-impl.h)")
    a = ap.parse_args()
    r = subprocess.run([a.wasm_objdump, "-x", "-j", "Type", a.wasm], capture_output=True, text=True, check=True)
    types = []
    for line in r.stdout.splitlines():
        m = re.match(r"\s*-\s*type\[(\d+)\]\s*\((.*?)\)\s*->\s*(.*)", line)
        if not m:
            continue
        idx = int(m.group(1))
        params = [p.strip() for p in m.group(2).split(",") if p.strip()]
        res = m.group(3).strip()
        results = [] if res == "nil" else [x.strip() for x in res.strip("()").split(",")]
        types.append((idx, params, results))
    mod = a.module
    for idx, params, results in types:
        if len(results) > 1:
            raise SystemExit(f"gen_eabi_dispatch: type t{idx} has {len(results)} results; the call-site adapter handles one")
    L = [f"#include \"{mod}.h\"", "#include <string.h>", "#include <stdio.h>", "#include <stdlib.h>", ""]

    L.append("#if defined(__GNUC__) || defined(__clang__)")
    L.append("#define TE_FUNC_TYPE_DECL(x) extern const char* const x")
    L.append("#else")
    L.append("#define TE_FUNC_TYPE_DECL(x) extern const char x[]")
    L.append("#endif")
    for idx, params, results in types:
        L.append(f"TE_FUNC_TYPE_DECL(w2c_{mod}_t{idx});")
    L += ["", "typedef union { u64 i; f32 f; f64 d; } te_eabi_result;", "",
          f"extern struct w2c_{mod}* te_env_instance(struct w2c_env* env);",
          f"extern wasm_rt_memory_t* te_env_memory(struct w2c_env* env);",
          "",
          "static te_eabi_result te_eabi_call_arrays(wasm_rt_funcref_table_t* table, u32 fn, const u64* I, const f32* F) {",
          "  te_eabi_result out; out.i = 0;",
          "  if (fn >= table->size || !table->data[fn].func) {",
          "    fprintf(stderr, \"[reNsanity] CallEabi: bad function index %u (table size %u)\\n\", fn, table->size);",
          "    wasm_rt_trap(WASM_RT_TRAP_CALL_INDIRECT);",
          "  }",
          "  wasm_rt_funcref_t ref = table->data[fn];",
          "  void* mi = ref.module_instance;",
          "  (void)I; (void)F; (void)mi;"]
    for idx, params, results in types:
        sig = ", ".join(["void*"] + [CTYPE[p] for p in params])
        ret = "void" if not results else CTYPE[results[0]]
        args = ["mi"]
        ni = nf = 0
        ok = True
        for p in params:
            if p in ("i32", "i64"):
                if ni >= 8:
                    ok = False
                args.append(f"({CTYPE[p]})I[{ni}]")
                ni += 1
            else:
                if nf >= 8:
                    ok = False
                args.append(f"({CTYPE[p]})F[{nf}]")
                nf += 1
        L.append(f"  if (ref.func_type == w2c_{mod}_t{idx} || memcmp(ref.func_type, w2c_{mod}_t{idx}, 32) == 0) {{")
        if not ok:
            L.append(f"    fprintf(stderr, \"[reNsanity] CallEabi: signature t{idx} needs more than 8 integers/floats\\n\"); abort();")
        else:
            call = f"(({ret} (*)({sig}))ref.func)({', '.join(args)})"
            if ret == "void":
                L.append(f"    {call};")
            elif ret == "u32":
                L.append(f"    out.i = (u64)(s64)(s32){call};")
            elif ret == "u64":
                L.append(f"    out.i = {call};")
            elif ret == "f32":
                L.append(f"    out.f = {call};")
            else:
                L.append(f"    out.d = {call};")
        L.append("    return out;")
        L.append("  }")
    L += ["  fprintf(stderr, \"[reNsanity] CallEabi: unknown function type for index %u (type id:\", fn);",
          "  for (int k = 0; k < 32; k++) fprintf(stderr, \" %02x\", (unsigned char)ref.func_type[k]);",
          "  fprintf(stderr, \")\\n\"); abort();", "}", "",
          "static te_eabi_result te_eabi_call(struct w2c_env* env, u32 fn, u32 ints_addr, u32 floats_addr) {",
          f"  struct w2c_{mod}* inst = te_env_instance(env);",
          f"  wasm_rt_funcref_table_t* table = w2c_{mod}_0x5F_indirect_function_table(inst);",
          "  wasm_rt_memory_t* mem = te_env_memory(env);",
          "  if ((u64)ints_addr + 64 > mem->size || (u64)floats_addr + 32 > mem->size) wasm_rt_trap(WASM_RT_TRAP_OOB);",
          "  u64 I[8]; f32 F[8];",
          "  memcpy(I, mem->data + ints_addr, sizeof I);",
          "  memcpy(F, mem->data + floats_addr, sizeof F);",
          "  return te_eabi_call_arrays(table, fn, I, F);",
          "}", "",
          "u64 w2c_env_CallEabiTrampoline(struct w2c_env* env, u32 fn, u32 ints, u32 floats) {",
          "  return te_eabi_call(env, fn, ints, floats).i;", "}", "",
          "f32 w2c_env_CallEabiTrampolineF(struct w2c_env* env, u32 fn, u32 ints, u32 floats) {",
          "  return te_eabi_call(env, fn, ints, floats).f;", "}", ""]
    H = ["#ifndef TE_EABI_ADAPT_H", "#define TE_EABI_ADAPT_H", "#ifdef __cplusplus", 'extern "C" {', "#endif"]
    for idx, params, results in types:
        ret = "void" if not results else CTYPE[results[0]]
        plist = ", ".join(["wasm_rt_funcref_table_t* table", "u32 x", "void* module_instance"] +
                          [f"{CTYPE[p]} a{i}" for i, p in enumerate(params)])
        name = f"te_adapt_w2c_{mod}_t{idx}"
        H.append(f"{ret} {name}({plist});")
        body = ["  u64 I[8] = {0}; f32 F[8] = {0};"]
        ni = nf = 0
        ok = True
        for i, p in enumerate(params):
            if p == "i32":
                body.append(f"  I[{ni}] = (u64)(s64)(s32)a{i};"); ni += 1
            elif p == "i64":
                body.append(f"  I[{ni}] = a{i};"); ni += 1
            elif p == "f32":
                body.append(f"  F[{nf}] = a{i};"); nf += 1
            else:
                body.append(f"  F[{nf}] = (f32)a{i};"); nf += 1
            if ni > 8 or nf > 8:
                ok = False
        L.append(f"{ret} {name}({plist}) {{")
        L.append("  (void)module_instance;")
        if not ok:
            L.append(f"  fprintf(stderr, \"[reNsanity] call_indirect adapter: t{idx} needs more than 8 integers/floats\\n\"); abort();")
        else:
            L += body
            L.append("  te_eabi_result r = te_eabi_call_arrays(table, x, I, F);")
            if ret == "void":
                L.append("  (void)r;")
            elif ret == "u32":
                L.append("  return (u32)r.i;")
            elif ret == "u64":
                L.append("  return r.i;")
            elif ret == "f32":
                L.append("  return r.f;")
            else:
                L.append("  return r.d;")
        L.append("}")
    H += ["#ifdef __cplusplus", "}", "#endif", "#endif", ""]
    Path(a.out).write_text("\n".join(L) + "\n")
    if a.adapt_header:
        Path(a.adapt_header).write_text("\n".join(H))
    return 0

if __name__ == "__main__":
    sys.exit(main())
