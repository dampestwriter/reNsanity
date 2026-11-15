import os
import platform
import re
import struct
import subprocess
import sys

SAMPLER_BINDING = {"u_texture": 0, "u_frame": 0, "u_src": 0, "u_rt_texture": 1, "u_clut": 2, "u_idx_tex2": 3,
                   "u_zsrc_tex": 4, "u_yuv_y": 0, "u_yuv_u": 1, "u_yuv_v": 2, "u_yuv_rb": 3, "u_yuv_g": 4,
                   "u_depth": 0, "u_pal": 1}

STD140 = {"int": (4, 4), "uint": (4, 4), "float": (4, 4), "bool": (4, 4), "vec2": (8, 8), "ivec2": (8, 8),
          "uvec2": (8, 8), "vec3": (16, 12), "ivec3": (16, 12), "vec4": (16, 16), "ivec4": (16, 16), "uvec4": (16, 16)}

COPY_FS = r"""
#version 410 core
uniform sampler2D u_src;
uniform ivec2 u_offset;
out vec4 frag_color;
void main() { frag_color = texelFetch(u_src, ivec2(gl_FragCoord.xy) + u_offset, 0); }
"""

SCANOUT_LEAN_FS = r"""
#version 410 core
noperspective in vec2 v_texcoord;
uniform sampler2D u_texture;
out vec4 frag_color;
void main() { frag_color = texture(u_texture, v_texcoord); }
"""

FS_DEFS = ["TE_INT_DIVIDE_IN_FLOAT 1"]

PS2_FS_DEFS = FS_DEFS + ["TE_GPU_NATIVE 1"]
PROGRAMS = [
    ("ps2", ("kPs2VertSrc", ["NATIVE_MESH 1", "ZERO_TO_ONE_DEPTH 1"]), ("kPs2FragSrc", PS2_FS_DEFS)),
    ("strip", ("kPs2VertSrc", []), ("kStencilStripFragSrc", FS_DEFS)),
    ("invert", ("kPs2VertSrc", []), ("kStencilInvertFragSrc", FS_DEFS)),
    ("shadow_count", ("kPs2VertSrc", []), ("kShadowCountFragSrc", [])),
    ("constq", ("kConstVertSrc", []), ("kConstFragSrc", [])),
    ("opaque", ("kConstVertSrc", []), ("kOpaqueMaskFragSrc", [])),
    ("scanout", ("kScanoutVertSrc", []), ("kScanoutFragSrc", [])),
    ("copy", ("kConstVertSrc", []), ("@COPY_FS", [])),
    ("yuv", ("kConstVertSrc", []), ("kYuvFragSrc", [])),
    ("scanout_lean", ("@SCANOUT_LEAN_VS", []), ("@SCANOUT_LEAN_FS", [])),

    ("scanout_fx", ("kScanoutVertSrc", []), ("kScanoutFxFragSrc", [])),

    ("haze", ("kConstVertSrc", []), ("kHazeFragSrc", [])),
]

SPEC_FIELDS = [("u_use_texture", None), ("u_tfx", None), ("u_tcc", None), ("u_tex_lod_hw", None),
               ("TE_MIP_ON", "u_mip_ctl"), ("u_ate", None), ("u_atst", None), ("u_afail", None), ("u_fba", None),
               ("u_colclamp", None), ("u_scanmsk", None), ("u_fog_enabled", None), ("u_blend_enabled", None),
               ("u_sw_blend", None), ("u_blend_a", None), ("u_blend_b", None), ("u_blend_c", None),
               ("u_blend_d", None), ("u_date", None), ("u_datm", None), ("u_fbmsk16_en", None),
               ("u_depth_fmt", None), ("u_idx_fmt", None), ("u_uv_native", None), ("u_fb16", None),
               ("TE_CT16Q_ON", "u_ct16q"), ("u_dthe", None), ("u_zsrc_en", None), ("u_clut_raw", None),
               ("u_flip_v", None), ("TE_LOD_FIXED", "u_mip_ctl.y")]

VS_CASES = [(0, 0, None), (2, 2, None), (2, 3, None),
            (1, 2, 0), (1, 3, 0), (1, 2, 1), (1, 3, 1),
            (3, 2, 1), (3, 3, 1)]

def variant_keys(path):
    keys = []
    for line in open(path):
        k = line.strip()
        if not k:
            continue
        if len(k) != len(SPEC_FIELDS) or any(c not in "0123456789abcdex" for c in k):
            raise SystemExit("dk_shadergen: bad variant key %r" % k)
        keys.append(k)
    return keys

def key_defines(key):
    out = []
    for (name, _), c in zip(SPEC_FIELDS, key):
        if c == "x":
            continue
        v = int(c, 16)
        out.append("#define %s %s" % (name, ("true" if v else "false") if name.startswith("TE_") else v))
    return "\n".join(out) + "\n"

def key_words(key):
    lo = hi = 0
    for f, c in enumerate(key):
        v = 15 if c == "x" else int(c, 16)
        if f < 16:
            lo |= v << (4 * f)
        else:
            hi |= v << (4 * (f - 16))
    return lo, hi

UNIFORM_RE = re.compile(r"^\s*uniform\s+(\w+)\s+(\w+)\s*(\[\s*(\d+)\s*\])?\s*;.*$")
VARYING_RE = re.compile(r"^(\s*)((?:noperspective|flat|smooth)\s+)?(in|out)\s+(\w+)\s+(\w+)\s*;(.*)$")

def literals(path):
    text = open(path, encoding="utf-8").read()
    out = {}
    for m in re.finditer(r"const char\* const (\w+) = R\"(\w*)\((.*?)\)\2\";", text, re.S):
        out[m.group(1)] = m.group(3)
    return out

def scan_uniforms(src):

    res = []
    for line in src.splitlines():
        m = UNIFORM_RE.match(line)
        if m and not m.group(1).startswith("sampler"):
            res.append((m.group(1), m.group(2), int(m.group(4)) if m.group(4) else None))
    return res

def layout(members):

    off = 0
    res = []
    for t, n, c in members:
        a, s = STD140[t]
        if c is not None:
            a = 16
            stride = 16
            size = 16 * c
        else:
            stride = s
            size = s
        off = (off + a - 1) // a * a
        res.append((t, n, c, off, stride))
        off += size
    return res, (off + 15) // 16 * 16

def convert(src, stage, defines, members, varyings):
    lines = src.strip("\n").splitlines()
    out = []
    block_done = False
    for line in lines:
        if line.startswith("#version"):
            out.append("#version 460")
            for d in defines:
                out.append("#define " + d)
            if members:
                out.append("layout(std140, binding = 0) uniform TeUniforms {")
                for t, n, c, _, _ in members:
                    out.append("    %s %s%s;" % (t, n, "[%d]" % c if c is not None else ""))
                out.append("};")
            block_done = True
            continue
        if line.lstrip().startswith("layout(std140) uniform TeBones"):

            out.append(line.replace("layout(std140) uniform TeBones", "layout(std140, binding = 1) uniform TeBones"))
            continue
        m = UNIFORM_RE.match(line)
        if m:
            if m.group(1).startswith("sampler"):
                name = m.group(2)
                if name not in SAMPLER_BINDING:
                    raise SystemExit("dk_shadergen: sampler %s has no binding" % name)
                out.append("layout(binding = %d) uniform %s %s;" % (SAMPLER_BINDING[name], m.group(1), name))
            else:
                out.append("")
            continue
        m = VARYING_RE.match(line)
        if m and not line.lstrip().startswith("layout"):
            indent, interp, io, typ, name, rest = m.groups()
            interp = interp or ""
            if stage == "frag" and io == "out":
                out.append("%slayout(location = 0) out %s %s;%s" % (indent, typ, name, rest))
                continue
            if (stage == "vert" and io == "out") or (stage == "frag" and io == "in"):
                if name not in varyings:
                    if stage == "frag":
                        raise SystemExit("dk_shadergen: fragment input %s has no vertex output" % name)
                    varyings[name] = len(varyings)
                out.append("%slayout(location = %d) %s%s %s %s;%s" % (indent, varyings[name], interp, io, typ, name, rest))
                continue
        out.append(line)
    if not block_done:
        raise SystemExit("dk_shadergen: no #version line")
    return "\n".join(out) + "\n"

DKSH_HEADER = struct.Struct("<6I")
DKSH_PROGRAM = struct.Struct("<8I")

def self_branch(word):

    return word >> 48 == 0xE240 and ((word >> 20) & 0xFFFFFF) >= 0xFFFFE8
WAIT_ALL = 0x3F << 11

def wait_all_barriers(blob, name):
    data = bytearray(blob)
    _, _, control_sz, code_sz, programs_off, num_programs = DKSH_HEADER.unpack_from(data, 0)
    code = control_sz
    for p in range(num_programs):
        ptype, entry, _, _, _, _, alt_entry, alt_gprs = DKSH_PROGRAM.unpack_from(data, programs_off + 64 * p)
        entries = [entry] + ([alt_entry] if ptype == 0 and alt_gprs else [])
        for ep in entries:
            g, ended, groups = code + ep + 0x50, False, 0
            while not ended:
                if g + 32 > code + code_sz:
                    raise SystemExit("dk_shadergen: %s: program %d runs past its code without its closing branch" % (name, p))
                (ctrl,) = struct.unpack_from("<Q", data, g)
                for k in range(3):
                    ctrl |= WAIT_ALL << (21 * k)
                    (word,) = struct.unpack_from("<Q", data, g + 8 + 8 * k)
                    ended = ended or self_branch(word)
                struct.pack_into("<Q", data, g, ctrl)
                g += 32
                groups += 1
            if not groups:
                raise SystemExit("dk_shadergen: %s: program %d has no instructions" % (name, p))
    return bytes(data)

def uam(tool, glsl_path, stage, out_path):

    cmd = ["setarch", platform.machine(), "-R", tool, "-s", stage, "-o", out_path, glsl_path]
    outs = []
    for attempt in range(2):
        r = subprocess.run(cmd, capture_output=True, text=True, env={})
        msg = (r.stdout + r.stderr).strip()
        if r.returncode != 0 or not os.path.exists(out_path):
            sys.stderr.write("dk_shadergen: uam failed on %s:\n%s\n" % (glsl_path, msg))
            raise SystemExit(1)
        outs.append(open(out_path, "rb").read())
    if outs[0] != outs[1]:
        raise SystemExit("dk_shadergen: uam compiled %s twice to different bytes" % glsl_path)
    if msg:

        with open(out_path + ".log", "w") as f:
            f.write(msg + "\n")
    blob = wait_all_barriers(outs[0], os.path.basename(out_path))
    with open(out_path, "wb") as f:
        f.write(blob)
    UAM_RAW[out_path] = outs[0]
    return blob

UAM_RAW = {}

def main():
    if len(sys.argv) not in (4, 5):
        raise SystemExit("usage: dk_shadergen.py <dk_shader_sources.glsl> <uam> <out_dir> [<dk_ps2_variants.txt>]")
    src_path, tool, out_dir = sys.argv[1:4]
    keys = variant_keys(sys.argv[4]) if len(sys.argv) == 5 else []
    os.makedirs(out_dir, exist_ok=True)
    lit = literals(src_path)

    def baseline(text, macro):
        blocks = re.compile(r"^[ \t]*#ifdef " + macro + r"\n(.*?)^#endif\n", re.M | re.S)
        return blocks.sub(lambda m: m.group(1).split("#else\n", 1)[1] if "#else\n" in m.group(1) else "", text)
    full_lit = dict(lit)
    lean_lit = dict(lit)
    lit = {name: baseline(text, "TE_NATIVE_LEAN") for name, text in lit.items()}
    fast_lit = dict(lit)
    alpha_lit = {name: baseline(text, "TE_ATEST_FAST") for name, text in lit.items()}
    lit = {name: baseline(text, "TE_ALPHA_FIRST") for name, text in alpha_lit.items()}
    lit["@COPY_FS"] = COPY_FS
    lit["@SCANOUT_LEAN_VS"] = lit["kScanoutVertSrc"].replace("out vec2 v_texcoord;", "noperspective out vec2 v_texcoord;")
    lit["@SCANOUT_LEAN_FS"] = SCANOUT_LEAN_FS
    blobs = []
    header = []
    layouts = {}
    vs_variants = []
    for name, (vs_name, vs_defs), (fs_name, fs_defs) in PROGRAMS:
        if vs_name not in lit or fs_name not in lit:
            raise SystemExit("dk_shadergen: %s: literal %s / %s not found" % (name, vs_name, fs_name))
        vs, fs = lit[vs_name], lit[fs_name]
        def block_members(vs_text, fs_text):
            members = []
            seen = {}
            for t, n, c in scan_uniforms(vs_text) + scan_uniforms(fs_text):
                if n in seen:
                    if seen[n] != (t, c):
                        raise SystemExit("dk_shadergen: %s: uniform %s declared with two types" % (name, n))
                    continue
                seen[n] = (t, c)
                members.append((t, n, c))
            return members

        lay, _ = layout(block_members(vs, fs))
        layout_vs = full_lit.get(vs_name, vs)
        layout_fs = full_lit.get(fs_name, fs)
        lay_full, size = layout(block_members(layout_vs, layout_fs))
        if lay_full[:len(lay)] != lay:
            raise SystemExit("dk_shadergen: %s: the lean members must follow every other uniform" % name)
        layouts[name] = lay_full
        varyings = {}
        vs_glsl = convert(vs, "vert", vs_defs, lay, varyings)
        fs_glsl = convert(fs, "frag", fs_defs, lay, varyings)
        vp = os.path.join(out_dir, name + "_vsh.glsl")
        fp = os.path.join(out_dir, name + "_fsh.glsl")
        open(vp, "w").write(vs_glsl)
        open(fp, "w").write(fs_glsl)
        vb = uam(tool, vp, "vert", os.path.join(out_dir, name + "_vsh.dksh"))
        fb = uam(tool, fp, "frag", os.path.join(out_dir, name + "_fsh.dksh"))
        blobs.append((name, vb, fb, size))
        if name == "ps2":
            ps2_fs_glsl = fs_glsl
            ps2_alpha_glsl = convert(alpha_lit[fs_name], "frag", fs_defs, lay, varyings)
            ps2_fast_glsl = convert(fast_lit[fs_name], "frag", fs_defs, lay, varyings)
            ps2_lean_glsl = convert(lean_lit[fs_name], "frag", fs_defs, lay_full, varyings)
        if name == "ps2":
            block_end = vs_glsl.index("\n};\n") + 4
            for i, (native, uv, lighting) in enumerate(VS_CASES):
                controls = dict(u_native=native, u_uv_mode=uv)
                if lighting is not None:
                    controls["u_mesh_lit"] = lighting
                macros = "".join("#define %s %d\n" % item for item in controls.items())
                text = vs_glsl[:block_end] + macros + vs_glsl[block_end:]
                vp = os.path.join(out_dir, "%sv%03d_vsh.glsl" % (name, i))
                dp = os.path.join(out_dir, "%sv%03d_vsh.dksh" % (name, i))
                open(vp, "w").write(text)
                variant_blob = uam(tool, vp, "vert", dp)
                vs_variants.append((name, i, variant_blob))
        header.append("namespace %s {" % name)
        header.append("constexpr uint32_t kUboSize = %d;" % max(size, 16))
        for t, n, c, off, stride in lay_full:
            header.append("constexpr uint32_t %s = %d;" % (n, off))

        fs_names = {n for _, n, _ in scan_uniforms(full_lit.get(fs_name, fs))}
        fs_chunks = 0
        for t, n, c, off, stride in lay_full:
            if n in fs_names:
                end = off + (stride * c if c is not None else STD140[t][1])
                for chunk in range(off // 16, (end + 15) // 16): fs_chunks |= 1 << chunk
        header.append("constexpr uint32_t kFragmentChunks = 0x%xu;" % fs_chunks)
        header.append("}")

    block_end = ps2_fs_glsl.index("\n};\n") + 4
    variants = []
    alpha_variants = []
    fast_variants = []
    lean_variants = []
    for i, key in enumerate(keys):
        base_text = ps2_fs_glsl[:block_end] + key_defines(key) + ps2_fs_glsl[block_end:]
        text = base_text
        fp = os.path.join(out_dir, "ps2v%03d_fsh.glsl" % i)
        open(fp, "w").write(text)
        dp = os.path.join(out_dir, "ps2v%03d_fsh.dksh" % i)
        variants.append((key, uam(tool, fp, "frag", dp), UAM_RAW[dp]))

        alpha_keep = key[0] == "1" and key[5] == "1" and key[7] == "0"
        text = ps2_alpha_glsl[:block_end] + "#define TE_ALPHA_FIRST 1\n" + key_defines(key) + ps2_alpha_glsl[block_end:] if alpha_keep else text
        fp = os.path.join(out_dir, "ps2alpha%03d_fsh.glsl" % i)
        dp = os.path.join(out_dir, "ps2alpha%03d_fsh.dksh" % i)
        open(fp, "w").write(text)
        if alpha_keep:
            alpha_variants.append((key, uam(tool, fp, "frag", dp), UAM_RAW[dp]))
        else:

            alpha_variants.append(variants[-1])
            open(dp, "wb").write(variants[-1][1])

        native_fields = {"u_depth_fmt": 0, "u_idx_fmt": 0, "u_sw_blend": 0, "u_date": 0,
                         "u_zsrc_en": 0, "u_fog_enabled": 0, "u_colclamp": 1, "u_fba": 0,
                         "u_fb16": 0, "u_fbmsk16_en": 0, "u_scanmsk": 0, "u_uv_native": 0, "TE_CT16Q_ON": 0}
        folded = {name: c for (name, _), c in zip(SPEC_FIELDS, key)}
        native_keep = alpha_keep and all(folded[n] == str(v) for n, v in native_fields.items())
        fast_text = ps2_fast_glsl[:block_end] + "#define TE_ATEST_FAST 1\n" + key_defines(key) + ps2_fast_glsl[block_end:]
        fp = os.path.join(out_dir, "ps2atest%03d_fsh.glsl" % i)
        dp = os.path.join(out_dir, "ps2atest%03d_fsh.dksh" % i)
        open(fp, "w").write(fast_text if native_keep else base_text)
        if native_keep:
            fast_variants.append((key, uam(tool, fp, "frag", dp), UAM_RAW[dp]))
        else:
            fast_variants.append(variants[-1])
            open(dp, "wb").write(variants[-1][1])

        lean_native = all(folded[n] == str(v) for n, v in native_fields.items())
        lean_text = ps2_lean_glsl[:block_end] + "#define TE_ATEST_FAST 1\n#define TE_NATIVE_LEAN 1\n" + key_defines(key) + ps2_lean_glsl[block_end:]
        fp = os.path.join(out_dir, "ps2lean%03d_fsh.glsl" % i)
        dp = os.path.join(out_dir, "ps2lean%03d_fsh.dksh" % i)
        open(fp, "w").write(lean_text if lean_native else base_text)
        if lean_native:
            lean_variants.append((key, uam(tool, fp, "frag", dp), UAM_RAW[dp]))
        else:
            lean_variants.append(variants[-1])
            open(dp, "wb").write(variants[-1][1])
    alpha_aliases = [a[1:] == b[1:] for a, b in zip(variants, alpha_variants)]
    fast_aliases = [a[1:] == b[1:] for a, b in zip(variants, fast_variants)]
    lean_aliases = [a[1:] == b[1:] for a, b in zip(variants, lean_variants)]
    offs = {n: off for _, n, _, off, _ in layouts["ps2"]}
    for name, src_uniform in SPEC_FIELDS:
        if (src_uniform or name).split(".")[0] not in offs:
            raise SystemExit("dk_shadergen: specialisation field %s has no uniform in the ps2 block" % name)
    key_offsets = []
    for name, src in SPEC_FIELDS:
        member, _, component = (src or name).partition(".")
        key_offsets.append(offs[member] + (4 * "xyzw".index(component) if component else 0))
    h = ["#pragma once", "#include <cstdint>", "namespace ps2r {", "namespace render {", "namespace dkgen {", "",
         "struct Blob { const uint8_t* data; uint32_t size; };",
         "struct Program { const char* name; Blob vs, fs; uint32_t ubo_size; };",
         "enum ProgramId : int {"]
    for name, _, _, _ in blobs:
        h.append("    k_%s," % name)
    h += ["    kProgramCount", "};", "extern const Program kPrograms[kProgramCount];", ""]
    h += ["struct FsVariant { uint64_t key_lo, key_hi; Blob fs; };",
          "constexpr int kPs2VariantCount = %d;" % len(variants),
          "extern const FsVariant kPs2Variants[kPs2VariantCount > 0 ? kPs2VariantCount : 1];",
          "extern const FsVariant kPs2VariantsAlphaFirst[kPs2VariantCount > 0 ? kPs2VariantCount : 1];",
          "extern const FsVariant kPs2VariantsAtestFast[kPs2VariantCount > 0 ? kPs2VariantCount : 1];",
          "extern const FsVariant kPs2VariantsLean[kPs2VariantCount > 0 ? kPs2VariantCount : 1];",
          "constexpr uint32_t kPs2KeyOffsets[%d] = {%s};" % (
              len(SPEC_FIELDS), ", ".join(map(str, key_offsets))), ""]
    h += ["constexpr int kPs2VsVariantCount = %d;" % len(VS_CASES),
          "extern const Blob kPs2VsVariants[kPs2VsVariantCount];",
          ""]
    h += header
    h += ["", "}", "}", "}", ""]
    hdr_tmp = "\n".join(h)

    c = ['#include "dk_shaders_gen.h"',
         "namespace ps2r {", "namespace render {", "namespace dkgen {", "namespace {"]
    for name, vb, fb, _ in blobs:
        for tag, b in (("vs", vb), ("fs", fb)):
            c.append("alignas(256) const uint8_t k_%s_%s[%d] = {" % (name, tag, len(b)))
            for i in range(0, len(b), 24):
                c.append("    " + ",".join("0x%02x" % x for x in b[i:i + 24]) + ",")
            c.append("};")
    for prefix, entries in (("ps2v", variants), ("ps2alpha", alpha_variants), ("ps2atest", fast_variants),
                            ("ps2lean", lean_variants)):
        for i, (key, b, raw) in enumerate(entries):
            if (prefix == "ps2alpha" and alpha_aliases[i]) or (prefix == "ps2atest" and fast_aliases[i]) or \
               (prefix == "ps2lean" and lean_aliases[i]):
                continue
            for tag, data in (("fs", b),):
                c.append("alignas(256) const uint8_t k_%s%03d_%s[%d] = {" % (prefix, i, tag, len(data)))
                for j in range(0, len(data), 24):
                    c.append("    " + ",".join("0x%02x" % x for x in data[j:j + 24]) + ",")
                c.append("};")
    for name, index, data in vs_variants:
        c.append("alignas(256) const uint8_t k_%sv%03d_vs[%d] = {" % (name, index, len(data)))
        for j in range(0, len(data), 24):
            c.append("    " + ",".join("0x%02x" % x for x in data[j:j + 24]) + ",")
        c.append("};")
    c.append("}")
    for arr, tag, prefix, entries in (("kPs2Variants", "fs", "ps2v", variants),
                                    ("kPs2VariantsAlphaFirst", "fs", "ps2alpha", alpha_variants),
                                    ("kPs2VariantsAtestFast", "fs", "ps2atest", fast_variants),
                                    ("kPs2VariantsLean", "fs", "ps2lean", lean_variants)):
        c.append("const FsVariant %s[kPs2VariantCount > 0 ? kPs2VariantCount : 1] = {" % arr)
        for i, (key, b, raw) in enumerate(entries):
            lo, hi = key_words(key)
            symbol = "ps2v" if ((prefix == "ps2alpha" and alpha_aliases[i]) or (prefix == "ps2atest" and fast_aliases[i]) or
                                (prefix == "ps2lean" and lean_aliases[i])) else prefix
            c.append("    {0x%016xull, 0x%016xull, {k_%s%03d_%s, %d}}," % (lo, hi, symbol, i, tag, len(b)))
        if not variants:
            c.append("    {0, 0, {nullptr, 0}},")
        c.append("};")
    c.append("const Blob kPs2VsVariants[kPs2VsVariantCount] = {")
    for name, index, data in vs_variants:
        c.append("    {k_%sv%03d_vs, %d}," % (name, index, len(data)))
    c.append("};")
    c.append("const Program kPrograms[kProgramCount] = {")
    for name, vb, fb, size in blobs:
        c.append('    {"%s", {k_%s_vs, %d}, {k_%s_fs, %d}, %d},' % (name, name, len(vb), name, len(fb), max(size, 16)))
    c += ["};", "}", "}", "}", ""]

    for fname, text in (("dk_shaders_gen.h", hdr_tmp), ("dk_shaders_gen.cpp", "\n".join(c))):
        p = os.path.join(out_dir, fname)
        if not os.path.exists(p) or open(p).read() != text:
            open(p, "w").write(text)

if __name__ == "__main__":
    main()
