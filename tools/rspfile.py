import os
import sys

def expand_argv(argv):
    out = []
    for a in argv:
        if len(a) > 1 and a.startswith("@") and os.path.isfile(a[1:]):
            with open(a[1:], encoding="utf-8") as f:
                out.extend(line.strip() for line in f if line.strip())
        else:
            out.append(a)
    return out

def tool_args(args, rsp_path, limit=7000):

    if sum(len(a) + 1 for a in args) <= limit:
        return list(args)
    with open(rsp_path, "w", encoding="utf-8") as f:
        for a in args:
            f.write('"' + a + '"\n')
    return ["@" + rsp_path]
