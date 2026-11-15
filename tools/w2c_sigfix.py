#!/usr/bin/env python3

import re
import sys
from pathlib import Path

PROTO = re.compile(r"^(\w+) (w2c_\w+?)_signature_mismatch0x3A(\w+)\((w2c_\w+)\*((?:, \w+)*)\);$", re.M)

def proto_of(impl, module, name):
    m = re.search(rf"^(\w+) {re.escape(module)}_{re.escape(name)}\({re.escape(module)}\*((?:, \w+)*)\);$", impl, re.M)
    if not m:
        return None
    return m.group(1), [t for t in m.group(2).split(", ") if t]

def main():
    args = sys.argv[1:]
    impl_path, shards = Path(args[0]), [Path(a) for a in args[1:]]
    impl = impl_path.read_text()
    stubs = []
    for m in PROTO.finditer(impl):
        ret, module, name, _inst, params = m.groups()
        stubs.append((ret, module, name, [t for t in params.split(", ") if t]))
    adapted, missing = [], []
    texts = {s: s.read_text() for s in shards}
    for ret, module, name, params in stubs:
        real = proto_of(impl, module, name)
        if real is None:
            missing.append(name)
            continue
        rret, rparams = real
        stub_fn = f"{module}_signature_mismatch0x3A{name}"
        args_c = []
        for i, t in enumerate(rparams):
            args_c.append(f"({t})var_p{i}" if i < len(params) else f"({t})0")
        call = f"{module}_{name}(instance{''.join(', ' + a for a in args_c)})"
        if ret == "void":
            body = f"(void){call};"
        elif rret == "void":
            zero = "0.0f" if ret == "f32" else ("0.0" if ret == "f64" else "0")
            body = f"{call}; return {zero};"
        else:
            body = f"return ({ret}){call};"
        for s, text in texts.items():

            pat = re.compile(rf"(^{re.escape(ret)} {re.escape(stub_fn)}\({re.escape(module)}\* instance[^\n]*\{{\n(?:[^\n]*\n)*?)  UNREACHABLE;\n", re.M)
            new, n = pat.subn(lambda mm: mm.group(1) + "  " + body + "\n", text, count=1)
            if n:
                texts[s] = new
                adapted.append(name)
                break
        else:
            if not any(stub_fn in t and "  " + body + "\n" in t for t in texts.values()):
                missing.append(name)
    for s, text in texts.items():
        if text != s.read_text():
            s.write_text(text)
    if missing:
        print(f"w2c_sigfix: signature-mismatch stubs left as traps: {', '.join(missing)}", file=sys.stderr)
    return 1 if missing else 0

if __name__ == "__main__":
    sys.exit(main())
