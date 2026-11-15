#!/usr/bin/env python3

import argparse
import re
import sys
from pathlib import Path

EXTRA_IMPORTS = [

    ("ps2e_system_kputs", "void", [("u32", "text")], False),
    ("CallEabiTrampoline", "u64", [("u32", "function"), ("u32", "integers"), ("u32", "floats")], True),
    ("CallEabiTrampolineF", "f32", [("u32", "function"), ("u32", "integers"), ("u32", "floats")], True),
]

SEAM_CONCURRENT_EXACT = {

    "ps2e_graphics_draw_rigid_model", "ps2e_graphics_draw_placed_model", "ps2e_graphics_draw_skin", "ps2e_graphics_draw_blend_skin",

    "ps2e_graphics_load_chunk_planes", "ps2e_graphics_load_portal_planes", "ps2e_graphics_load_culling_view",
    "ps2e_graphics_cull_load_link", "ps2e_graphics_cull_load_model", "ps2e_graphics_cull_test_box", "ps2e_graphics_cull_test_box_at",
    "ps2e_graphics_cull_result", "ps2e_graphics_cull_matrices", "ps2e_graphics_clip_box", "ps2e_graphics_clip_box_at",

    "ps2e_graphics_load_particle_view",

    "ps2e_system_kputs", "ps2e_time_ticks", "ps2e_cache_prefetch",
}

SEAM_CONCURRENT_PREFIX = ("ps2e_math_", "ps2e_vu0_", "ps2e_audio_", "ps2e_pads_", "ps2e_files_", "ps2e_saves_", "ps2e_disc_",
                          "ps2e_io_", "ps2e_stream_", "ps2e_sound_")

def seam_concurrent(name):
    return name in SEAM_CONCURRENT_EXACT or name.startswith(SEAM_CONCURRENT_PREFIX)

PRIMS_I32 = {"u32", "s32", "u16", "s16", "u8", "s8", "int", "unsigned", "char", "short", "bool", "size_t"}
WASM2C_TYPE = {"u32": "u32", "s32": "u32", "u64": "u64", "s64": "u64", "f32": "f32", "f64": "f64", "void": "void"}

def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", text)

def snake(name):
    s = re.sub(r"(?<=[a-z0-9])([A-Z])", r"_\1", name)
    s = re.sub(r"([A-Z]+)([A-Z][a-z])", r"\1_\2", s)
    return s.lower()

def find_namespace_blocks(text):

    for m in re.finditer(r"namespace\s+Platform::(\w+)\s*\{", text):
        depth = 1
        i = m.end()
        while i < len(text) and depth:
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
            i += 1
        yield m.group(1), text[m.end():i - 1]

def top_level_statements(body):

    out, depth, cur = [], 0, []
    for ch in body:
        cur.append(ch)
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                out.append("".join(cur))
                cur = []
        elif ch == ";" and depth == 0:
            out.append("".join(cur))
            cur = []
    return [" ".join(s.split()) for s in out if s.strip()]

def collect_types(text):
    enums = set(re.findall(r"\benum\s+(?:class\s+)?(\w+)", text))
    structs = set(re.findall(r"\bstruct\s+(\w+)\s*(?:\{|;)", text))
    return enums, structs

def split_params(s):
    parts, depth, cur = [], 0, []
    for ch in s:
        if ch in "(<[":
            depth += 1
        elif ch in ")>]":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    if "".join(cur).strip():
        parts.append("".join(cur).strip())
    return [p for p in parts if p and p != "void"]

def parse_param(p, index):
    p = re.sub(r"=\s*[^,]+$", "", p).strip()

    m = re.match(r"^(.*?)\(\s*\*\s*(\w+)\s*\)\s*\((.*)\)$", p)
    if m:
        return {"text": p, "name": m.group(2), "type": p.replace(m.group(2), "", 1), "kind": "ptr"}

    m = re.match(r"^(.*?)\b(\w+)\s*\[[^\]]*\]$", p)
    if m:
        return {"text": p, "name": m.group(2), "type": m.group(1).strip(), "kind": "ptr"}
    m = re.match(r"^(.*?)\b(\w+)$", p)
    if not m:
        return {"text": p, "name": f"a{index}", "type": p, "kind": None}
    type_, name = m.group(1).strip(), m.group(2)
    if type_ == "" or name in PRIMS_I32 or name in ("f32", "u64", "s64", "f64", "float", "double"):

        type_, name = p, f"a{index}"
    return {"text": p, "name": name, "type": type_, "kind": None}

def classify(type_, enums, structs):
    t = type_.replace("const", "").replace("volatile", "").strip()
    if "*" in t or "&" in t:
        return "ptr"
    base = t.split()[-1] if t.split() else t
    if base in ("f32", "float"):
        return "f32"
    if base in ("f64", "double"):
        return "f64"
    if base in ("u64", "s64", "long"):
        return "u64" if base == "u64" else "s64"
    if base in ("bool",):
        return "bool"
    if base in PRIMS_I32 or base in enums or re.match(r"^(u|s)(8|16|32)$", base):
        return "enum" if base in enums else "i32"
    if base in structs or base[0].isupper():
        return "byval"
    return "i32"

def wasm_type_of(kind):
    return {"ptr": "u32", "f32": "f32", "f64": "f64", "u64": "u64", "s64": "s64", "bool": "s32", "enum": "s32",
            "i32": "s32", "byval": "u32", "void": "void"}[kind]

def parse_header(path):
    text = strip_comments(path.read_text())
    enums, structs = collect_types(text)
    fns = []
    for ns, body in find_namespace_blocks(text):
        for st in top_level_statements(body):
            if st.endswith("}"):

                continue
            if not st.endswith(";") or "(" not in st:
                continue
            head = st[:-1].strip()
            if re.match(r"^(enum|struct|class|constexpr|inline|template|using|typedef|static_assert|CHECK_|extern|friend)\b", head):
                continue
            if re.match(r"^[A-Z_]+\(", head):
                continue

            m = re.search(r"\sRETAIL(?:_N32)?\(\w+\)$", head)
            if m:
                head = head[:m.start()].strip()
            noreturn = "[[noreturn]]" in head
            head = head.replace("[[noreturn]]", "").strip()
            m = re.match(r"^(.*?)\b(\w+)\s*\((.*)\)\s*$", head, re.DOTALL)
            if not m:
                continue
            ret, name, params = m.group(1).strip(), m.group(2), m.group(3)
            if name in ("operator",):
                continue
            plist = [parse_param(p, i) for i, p in enumerate(split_params(params))]
            for p in plist:
                p["kind"] = p["kind"] or classify(p["type"], enums, structs)
            rkind = "void" if ret == "void" else classify(ret, enums, structs)
            fns.append({"ns": ns, "name": name, "ret": ret, "ret_kind": rkind, "params": plist,
                        "noreturn": noreturn})
    return fns, enums, structs

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--module", default="game")
    ap.add_argument("--wasm-side", default="memory",
                    help="comma list of headers implemented INSIDE the module (platform/nativeee/wasm/<h>.cpp): no imports")
    a = ap.parse_args()
    inc = Path(a.game) / "include" / "platform"
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)

    wasm_side = {x.strip() for x in a.wasm_side.split(",") if x.strip()}

    wasm_side_single = set()
    tsv_fns = Path(__file__).resolve().parent / "wasm_side_functions.txt"
    if tsv_fns.exists():
        for line in tsv_fns.read_text().splitlines():
            line = line.split("#", 1)[0].strip()
            if line:
                wasm_side_single.add(line)

    tsv_imp = Path(__file__).resolve().parent / "platform_imports.tsv"
    if tsv_imp.exists():
        for line in tsv_imp.read_text().splitlines():
            if not line.strip() or line.startswith("#"):
                continue
            name, rt, params = (line.split("\t") + ["", ""])[:3]
            plist = [tuple(x.split(":")) for x in params.split(",")] if params.strip() else []
            EXTRA_IMPORTS.append((name, rt, plist, False))
    all_fns, headers = [], sorted(inc.glob("*.h"))
    seen = {}
    wasm_side_fns = []
    for h in headers:
        fns, _, _ = parse_header(h)
        if h.stem in wasm_side:
            for f in fns:
                f["header"] = h.name
            wasm_side_fns += fns
            continue
        for f in fns:
            f["header"] = h.name
            if f"{f['ns']}::{f['name']}" in wasm_side_single:
                wasm_side_fns.append(f)
                continue
            base = f"ps2e_{snake(f['ns'])}_{snake(f['name'])}"
            n = seen.get(base, 0) + 1
            seen[base] = n
            f["import"] = base if n == 1 else f"{base}_{n}"
            all_fns.append(f)

    second = []
    tsv = Path(__file__).resolve().parent / "second_seam.tsv"
    for line in tsv.read_text().splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        sym, imp, ret, params = line.split("\t")[:4]
        plist = [tuple(x.split(":")) for x in params.split(",")] if params.strip() else []
        second.append({"symbol": sym, "import": imp, "ret": ret, "params": plist})

    H = ["#pragma once", "#include <stdint.h>",
         "#ifdef __cplusplus", "extern \"C\" {", "#endif",
         "typedef uint32_t ps2e_u32; typedef int32_t ps2e_s32; typedef uint64_t ps2e_u64; typedef int64_t ps2e_s64;",
         "typedef float ps2e_f32; typedef double ps2e_f64;", ""]
    cur = None
    for f in all_fns:
        if f["header"] != cur:
            cur = f["header"]
        ps = []
        if f["ret_kind"] == "byval":
            ps.append("ps2e_u32 out")
        for p in f["params"]:
            ps.append(f"ps2e_{wasm_type_of(p['kind'])} {p['name']}")
        rt = "void" if f["ret_kind"] in ("void", "byval") else f"ps2e_{wasm_type_of(f['ret_kind'])}"
        H.append(f"{rt} {f['import']}({', '.join(ps) or 'void'});")
    H.append("")
    for f in second:
        rt = "void" if f["ret"] == "void" else "ps2e_u32" if f["ret"] in ("ptr", "u32") else "ps2e_" + f["ret"]
        H.append(f"{rt} {f['import']}({', '.join('ps2e_' + ('u32' if k == 'ptr' else k) + ' ' + n for k, n in f['params']) or 'void'});")
    H.append("")
    for name, rt, ps, runner in EXTRA_IMPORTS:
        H.append(f"{'void' if rt == 'void' else 'ps2e_' + rt} {name}({', '.join('ps2e_' + t + ' ' + n for t, n in ps) or 'void'});")
    H += ["#ifdef __cplusplus", "}", "#endif", ""]
    (out / "ps2e_seam.h").write_text("\n".join(H))

    forwarder_files = []
    for h in headers:
        if h.stem in wasm_side:
            continue
        F = ["#include \"common.h\"", f"#include \"platform/{h.name}\"", "#include \"ps2e_seam.h\"", ""]
        for f in [x for x in all_fns if x["header"] == h.name]:
            sig_params = ", ".join(p["text"] if re.search(r"\b%s\b" % re.escape(p["name"]), p["text"]) else f"{p['text']} {p['name']}" for p in f["params"])
            args, pre = [], []
            for p in f["params"]:
                k = p["kind"]
                if k == "ptr":
                    if "&" in p["text"] and "*" not in p["text"].split("&")[0]:
                        args.append(f"reinterpret_cast<ps2e_u32>(&{p['name']})")
                    else:
                        args.append(f"reinterpret_cast<ps2e_u32>({p['name']})")
                elif k == "byval":
                    pre.append(f"    auto te_copy_{p['name']} = {p['name']};")
                    args.append(f"reinterpret_cast<ps2e_u32>(&te_copy_{p['name']})")
                elif k in ("bool", "enum", "i32"):
                    args.append(f"static_cast<ps2e_s32>({p['name']})")
                elif k == "s64":
                    args.append(f"static_cast<ps2e_s64>({p['name']})")
                else:
                    args.append(p["name"])
            F.append(f"namespace Platform::{f['ns']}")
            F.append("{")
            F.append(f"{'[[noreturn]] ' if f['noreturn'] else ''}{f['ret']} {f['name']}({sig_params})")
            F.append("{")
            F += pre
            rk = f["ret_kind"]
            call = f"{f['import']}({', '.join(args)})"
            if rk == "void":
                F.append(f"    {call};")
                if f["noreturn"]:
                    F.append("    __builtin_trap();")
            elif rk == "byval":
                F.append(f"    {f['ret']} te_out;")
                F.append(f"    {f['import']}({', '.join(['reinterpret_cast<ps2e_u32>(&te_out)'] + args)});")
                F.append("    return te_out;")
            elif rk == "ptr":
                F.append(f"    return reinterpret_cast<{f['ret']}>({call});")
            else:
                F.append(f"    return static_cast<{f['ret']}>({call});")
            F.append("}")
            F.append("}")
            F.append("")
        name = f"seam_forwarders_{h.stem}.cpp"
        (out / name).write_text("\n".join(F))
        forwarder_files.append(name)
    F = ["#include \"ps2e_seam.h\"", "extern \"C\" {"]
    for f in second:
        ct = {"ptr": "ps2e_u32", "u32": "ps2e_u32", "s32": "ps2e_s32", "f32": "ps2e_f32", "void": "void"}
        ps = ", ".join(f"{ct[k]} {n}" for k, n in f["params"])
        F.append(f"{ct[f['ret']]} te_second_{f['symbol']}({ps or 'void'}) asm(\"{f['symbol']}\");")
        F.append(f"{ct[f['ret']]} te_second_{f['symbol']}({ps or 'void'}) {{ {'return ' if f['ret'] != 'void' else ''}{f['import']}({', '.join(n for _, n in f['params'])}); }}")
    F += ["}", ""]
    (out / "seam_forwarders_second.cpp").write_text("\n".join(F))
    forwarder_files.append("seam_forwarders_second.cpp")
    (out / "seam_forwarders.txt").write_text("\n".join(forwarder_files) + "\n")

    imports = [f["import"] for f in all_fns] + [f["import"] for f in second] + [e[0] for e in EXTRA_IMPORTS]
    (out / "seam_imports.txt").write_text("\n".join(imports) + "\n")

    mod = a.module
    T = [f"#include \"{mod}.h\"", "#include \"ps2e_seam.h\"", "",
         "extern void te_record_barrier(void) __attribute__((weak));",
         "#define TE_BARRIER() do { if (te_record_barrier) te_record_barrier(); } while (0)", ""]
    def seam_body(rt, imp, args):

        bar = '' if seam_concurrent(imp) else 'TE_BARRIER(); '
        if rt == 'void':
            return f"    (void)env; {bar}{imp}({', '.join(args)});"
        return f"    (void)env; {bar}{rt} r_ = ({rt}){imp}({', '.join(args)}); return r_;"
    def w2c_sig(rt_kind, params):
        ps = ["struct w2c_env* env"]
        for t, n in params:
            ps.append(f"{WASM2C_TYPE[t]} {n}")
        return ps
    for f in all_fns:
        params = []
        if f["ret_kind"] == "byval":
            params.append(("u32", "out"))
        params += [(wasm_type_of(p["kind"]), p["name"]) for p in f["params"]]
        rt = "void" if f["ret_kind"] in ("void", "byval") else WASM2C_TYPE[wasm_type_of(f["ret_kind"])]
        T.append(f"{rt} w2c_env_{f['import']}({', '.join(w2c_sig(f['ret_kind'], params))}) {{")
        T.append(seam_body(rt, f['import'], [n for _, n in params]))
        T.append("}")
    for f in second:
        ps = [("u32" if k == "ptr" else k, n) for k, n in f["params"]]
        rt = "void" if f["ret"] == "void" else WASM2C_TYPE["u32" if f["ret"] == "ptr" else f["ret"]]
        T.append(f"{rt} w2c_env_{f['import']}({', '.join(w2c_sig(None, ps))}) {{")
        T.append(seam_body(rt, f['import'], [n for _, n in ps]))
        T.append("}")
    T.append("")
    for name, rt, ps, runner in EXTRA_IMPORTS:
        if runner:
            continue
        T.append(f"{WASM2C_TYPE[rt] if rt != 'void' else 'void'} w2c_env_{name}({', '.join(w2c_sig(None, ps))}) {{")
        T.append(seam_body(WASM2C_TYPE[rt] if rt != 'void' else 'void', name, [n for _, n in ps]))
        T.append("}")
    T.append("")
    (out / "seam_host_thunks.c").write_text("\n".join(T))

    S = ["#include \"ps2e_seam.h\"", "#include <cstdio>", "",
         "#define TE_WEAK __attribute__((weak))", "#define TE_STUB(name) name", "#define TE_STUB_END(name)",
         "static void te_stub_once(const char* name, bool* said) { if (!*said) { *said = true; std::fprintf(stderr, \"[reNsanity] UNIMPLEMENTED seam import: %s\\n\", name); } }",
         "extern \"C\" {"]
    for f in all_fns:
        ps = []
        if f["ret_kind"] == "byval":
            ps.append("ps2e_u32 out")
        ps += [f"ps2e_{wasm_type_of(p['kind'])} {p['name']}" for p in f["params"]]
        rt = "void" if f["ret_kind"] in ("void", "byval") else f"ps2e_{wasm_type_of(f['ret_kind'])}"
        S.append(f"TE_WEAK {rt} TE_STUB({f['import']})({', '.join(ps) or 'void'}) {{ static bool said; te_stub_once(\"{f['import']}\", &said);"
                 + (" " + " ".join(f"(void){p['name']};" for p in f["params"]) if f["params"] else "")
                 + (" (void)out;" if f["ret_kind"] == "byval" else "")
                 + (" return 0;" if rt != "void" else "") + f" }} TE_STUB_END({f['import']})")
    for f in second:
        ct = {"ptr": "ps2e_u32", "u32": "ps2e_u32", "s32": "ps2e_s32", "f32": "ps2e_f32", "void": "void"}
        ps = ", ".join(f"{ct[k]} {n}" for k, n in f["params"])
        S.append(f"TE_WEAK {ct[f['ret']]} TE_STUB({f['import']})({ps or 'void'}) {{ static bool said; te_stub_once(\"{f['import']}\", &said); "
                 + " ".join(f"(void){n};" for _, n in f["params"]) + (" return 0;" if f["ret"] != "void" else "") + f" }} TE_STUB_END({f['import']})")
    for name, rt, ps, runner in EXTRA_IMPORTS:
        if runner:
            continue
        S.append(f"TE_WEAK {'void' if rt == 'void' else 'ps2e_' + rt} TE_STUB({name})({', '.join('ps2e_' + t + ' ' + n for t, n in ps)}) {{ static bool said; te_stub_once(\"{name}\", &said); "
                 + " ".join(f"(void){n};" for _, n in ps) + (" return 0;" if rt != "void" else "") + f" }} TE_STUB_END({name})")
    S += ["}", ""]
    (out / "seam_stubs.cpp").write_text("\n".join(S))

    return 0

if __name__ == "__main__":
    sys.exit(main())
