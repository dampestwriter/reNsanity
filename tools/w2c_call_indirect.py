#!/usr/bin/env python3

import sys
from pathlib import Path

OLD_SLOW = """  if (index >= table->size) {
    // Table index out of bounds. Raise Wasm trap.
    TRAP(CALL_INDIRECT);
    return false;
  }
  const wasm_rt_funcref_t* const func_ref = &table->data[index];
  if (!expected_type || !func_ref->func || !func_ref->func_type ||
      memcmp(expected_type, func_ref->func_type, sha256size) != 0) {
    // Type check failed. Raise Wasm trap.
    TRAP(CALL_INDIRECT);
    return false;
  }"""
NEW_SLOW = """  if (index >= table->size) {
    te_call_indirect_bad(table, expected_type, index);
    TRAP(CALL_INDIRECT);
    return false;
  }
  const wasm_rt_funcref_t* const func_ref = &table->data[index];
  if (!expected_type || !func_ref->func || !func_ref->func_type) {
    te_call_indirect_bad(table, expected_type, index);
    TRAP(CALL_INDIRECT);
    return false;
  }
  if (memcmp(expected_type, func_ref->func_type, sha256size) != 0) {
    return false;
  }"""
ANCHOR = "W2C_COLD_FUNC static bool func_types_eq_slowpath("
DECL = "void te_call_indirect_bad(const wasm_rt_funcref_table_t* table, const char* expected_type, uint32_t index);\n"
OLD_MACRO = """#define CALL_INDIRECT(table, t, ft, x, ...) \\
  (CHECK_CALL_INDIRECT(table, ft, x),       \\
   DO_CALL_INDIRECT(table, t, x, __VA_ARGS__))"""
NEW_MACRO = """#include "%s"
#define CALL_INDIRECT(table, t, ft, x, ...)                                      \\
  (CHECK_CALL_INDIRECT(table, ft, x) ? DO_CALL_INDIRECT(table, t, x, __VA_ARGS__) \\
                                     : te_adapt_##ft(&(table), x, __VA_ARGS__))"""

def main():
    args = sys.argv[1:]
    header = "eabi_adapt.h"
    if "--adapt-header" in args:
        i = args.index("--adapt-header")
        header = Path(args[i + 1]).name
        del args[i:i + 2]
    p = Path(args[0])
    s = p.read_text()
    if DECL in s:
        return 0
    if OLD_SLOW not in s or OLD_MACRO not in s or ANCHOR not in s:
        sys.stderr.write("w2c_call_indirect: game-impl.h does not have the expected func_types_eq_slowpath / CALL_INDIRECT shape\n")
        return 1
    s = s.replace(OLD_SLOW, NEW_SLOW, 1).replace(OLD_MACRO, NEW_MACRO % header, 1).replace(ANCHOR, DECL + ANCHOR, 1)
    p.write_text(s)
    return 0

if __name__ == "__main__":
    sys.exit(main())
