#!/usr/bin/env python3

import re
import struct
import sys
from pathlib import Path

LITERAL = re.compile(r"\b(var_f\d+ = )(-?)(\d+(?:\.\d*)?(?:[eE][-+]?\d+)?);")

def exact(m):
    sign, text = m.group(2), m.group(3)
    value = struct.unpack("<f", struct.pack("<f", float(text)))[0]
    spelled = value.hex()
    if spelled in ("inf", "nan"):
        raise SystemExit("w2c_f32const: %s does not fit a float" % text)
    return "%s%s%sf;" % (m.group(1), sign, spelled)

def main():
    for name in sys.argv[1:]:
        p = Path(name)
        s = p.read_text()
        t, n = LITERAL.subn(exact, s)
        if n:
            p.write_text(t)
    return 0

if __name__ == "__main__":
    sys.exit(main())
