const char* const kPs2VertSrc = R"GLSL(
#version 410 core

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_texcoord;
layout(location = 2) in vec4 a_color;
layout(location = 3) in float a_fog;

uniform vec2 u_screen_size;












uniform vec2 u_uv_scale;
uniform int u_uv_mode;







noperspective out vec3 v_texcoord;
out vec4 v_color;
out float v_fog;

#ifdef NATIVE_MESH




layout(location = 4) in vec3 a_normal;
layout(location = 5) in vec4 a_litcolor;
layout(location = 6) in float a_q;
uniform int u_native;
uniform int u_mesh_lit;
uniform vec4 u_mesh[5];

uniform vec4 u_mesh_light[7];



layout(location = 7) in vec4 a_bone;
layout(location = 8) in vec3 a_weight;
layout(std140) uniform TeBones { vec4 u_bones[464]; };
uniform int u_bone_vec4;
#endif

void main() {
    vec3 position = a_position;
    vec3 texcoord = a_texcoord;
    vec4 color = a_color;
#ifdef NATIVE_MESH
    if (u_native == 1 || u_native == 3) {
        vec3 mpos = a_position;
        vec3 mnormal = a_normal;
        if (u_native == 3) {


            int palette = u_bone_vec4 & 0xFFFF;
            vec3 pos = (u_bone_vec4 >> 16) != 0 ? u_bones[palette + int(a_bone.w)].xyz : a_position;
            vec3 sp = vec3(0.0);
            vec3 sn = vec3(0.0);
            for (int b = 0; b < 3; ++b) {
                int at = int(a_bone[b]);
                if (at < 0 || at + 3 >= palette) continue;
                vec4 r0 = u_bones[at], r1 = u_bones[at + 1], r2 = u_bones[at + 2], tr = u_bones[at + 3];
                sp += a_weight[b] * (tr.xyz + r0.xyz * pos.x + r1.xyz * pos.y + r2.xyz * pos.z);
                if (b == 0) sn = r0.xyz * a_normal.x + r1.xyz * a_normal.y + r2.xyz * a_normal.z;
            }
            mpos = sp;
            mnormal = sn;
        }
        vec4 c = u_mesh[3] + u_mesh[0] * mpos.x + u_mesh[1] * mpos.y + u_mesh[2] * mpos.z;
        float rq = 1.0 / c.w;

        position = vec3(floor(c.xy * rq) / 16.0 - u_mesh[4].xy, u_mesh_lit == 2 ? trunc(-c.z * rq) : floor(c.z * rq));
        texcoord = vec3((a_texcoord.xy + u_mesh[4].zw) * rq, a_texcoord.z * rq);

        if (a_texcoord.z == 0.0) texcoord = vec3(0.0, 0.0, 1.0);
        if (u_mesh_lit == 1) {
            vec3 lit = u_mesh_light[6].xyz + u_mesh_light[3].xyz * max(0.0, dot(mnormal, u_mesh_light[0].xyz))
                                           + u_mesh_light[4].xyz * max(0.0, dot(mnormal, u_mesh_light[1].xyz))
                                           + u_mesh_light[5].xyz * max(0.0, dot(mnormal, u_mesh_light[2].xyz));
            color = vec4(min(a_litcolor.rgb * lit, vec3(1.0)), a_litcolor.a);
            if (u_native == 3) {


                ivec4 q = (ivec4(a_color * 255.0 + 0.5) << 8) | ivec4(a_litcolor * 255.0 + 0.5);
                q = (q << 16) >> 16;
                ivec3 rgb = ivec3(min(lit * vec3(q.rgb), vec3(255.0))) & 255;
                color = vec4(vec3(rgb) / 255.0, float(min(q.a, 255) & 255) / 255.0);
            }
        }
    } else if (u_native == 2) {
        texcoord = vec3(a_texcoord.xy, a_q == 0.0 ? 1.0 : a_q);
    }
#endif


    vec2 ndc = (position.xy / u_screen_size) * 2.0 - 1.0;
















#ifdef ZERO_TO_ONE_DEPTH
    float z_ndc = position.z / 4294967296.0;
#else
    float z_ndc = position.z / 2147483648.0 - 1.0;
#endif
    gl_Position = vec4(ndc, z_ndc, 1.0);

    vec3 tc = texcoord;
    if (u_uv_mode == 1)      tc = vec3(texcoord.xy * 0.0625, 1.0);
    else if (u_uv_mode == 2) tc = vec3(texcoord.xy * u_uv_scale, texcoord.z);
    else if (u_uv_mode == 3) tc = vec3(0.0, 0.0, 1.0);
    v_texcoord = tc;
    v_color = color;
    v_fog = a_fog;
}
)GLSL";

const char* const kPs2FragSrc = R"GLSL(
#version 410 core
#define GSMIP_AAF 1
#define S83_IDXSCALE 1



noperspective in vec3 v_texcoord;
in vec4 v_color;
in float v_fog;

uniform sampler2D u_texture;
uniform sampler2D u_rt_texture;
uniform int u_use_texture;
uniform vec2 u_tex_size;




uniform int u_blend_a;
uniform int u_blend_b;
uniform int u_blend_c;
uniform int u_blend_d;
uniform float u_fix_alpha;
uniform int u_blend_enabled;
uniform int u_sw_blend;
uniform int u_flip_v;


uniform int u_tfx;
uniform int u_tcc;








uniform ivec2 u_mip_ctl;
uniform vec2 u_mip_lodp;





uniform int u_tex_lod_hw;


uniform int u_fba;
uniform int u_colclamp;
uniform int u_scanmsk;


uniform int u_zte;
uniform int u_ztst;


uniform int u_ate;
uniform int u_atst;
uniform int u_aref;
uniform int u_afail;


uniform int u_date;
uniform int u_datm;


uniform int u_fog_enabled;
uniform vec3 u_fogcol;








uniform int u_fbmsk16_en;
uniform int u_fbmsk16;






















uniform int u_depth_fmt;


uniform int u_depth_zoff;
























uniform int u_idx_fmt;
uniform int u_idx_self;










uniform int u_uv_native;










uniform int u_draw_scale;
uniform sampler2D u_clut;


uniform sampler2D u_idx_tex2;















uniform int u_fb16;






uniform int u_ct16q;
uniform int u_dthe;
uniform int u_dimx_lo;
uniform int u_dimx_hi;
uniform int u_zsrc_en;
uniform sampler2D u_zsrc_tex;




uniform int u_zsrc_num;
uniform int u_zsrc_den;
uniform int u_clut_raw;
#ifdef TE_NATIVE_LEAN
uniform vec2 u_tex_rsize;
#endif



#ifndef TE_MIP_ON
#define TE_MIP_ON (u_mip_ctl.x > 0)
#endif
#ifndef TE_LOD_FIXED
#define TE_LOD_FIXED (u_mip_ctl.y != 0)
#endif
#ifndef TE_CT16Q_ON
#define TE_CT16Q_ON (u_ct16q != 0)
#endif

out vec4 frag_color;

#ifdef S83_IDXSCALE











#ifndef TE_INT_DIVIDE_IN_FLOAT
int te_idiv(int a, int b) { return a / b; }
ivec2 te_idiv(ivec2 a, int b) { return a / b; }
int te_imod(int a, int b) { return a % b; }
#else
int te_idiv(int a, int b) { return int(floor((float(a) + 0.5) / float(b))); }
ivec2 te_idiv(ivec2 a, int b) { return ivec2(te_idiv(a.x, b), te_idiv(a.y, b)); }
int te_imod(int a, int b) { return a - b * te_idiv(a, b); }
#endif

vec2 S83NativeCenter(vec2 rt) {
    if (u_draw_scale <= 1) return rt;
    float s = float(u_draw_scale);
    vec2 fc = gl_FragCoord.xy;
    vec2 d  = (floor(fc / s) + 0.5) * s - fc;
    return rt + dFdx(rt) * d.x + dFdy(rt) * d.y;
}
#endif













uvec4 TeIdx8Colour(ivec2 t) {
    int i = int(texelFetch(u_texture, t, 0).r * 255.0 + 0.5);
    return uvec4(floor(texelFetch(u_clut, ivec2(i, 0), 0) * 255.0 + 0.5));
}
vec4 TeIdx8(vec2 uv) {
    if ((u_idx_self & 1) == 0) {
        int i = int(texture(u_texture, uv).r * 255.0 + 0.5);
        return floor(texelFetch(u_clut, ivec2(i, 0), 0) * 255.0 + 0.5);
    }
    ivec2 sz = textureSize(u_texture, 0);
    vec2 p = uv * vec2(sz) - 0.5;
    vec2 pf = floor(p);
    ivec2 a = ivec2(pf);
    uvec2 w = uvec2(floor((p - pf) * 256.0 + 0.5));
    a += ivec2(w >> 8u);
    w &= 255u;
    ivec2 b = a + 1;
    if ((u_idx_self & 2) != 0) { a.x = clamp(a.x, 0, sz.x - 1); b.x = clamp(b.x, 0, sz.x - 1); }
    else { a.x = a.x & (sz.x - 1); b.x = b.x & (sz.x - 1); }
    if ((u_idx_self & 4) != 0) { a.y = clamp(a.y, 0, sz.y - 1); b.y = clamp(b.y, 0, sz.y - 1); }
    else { a.y = a.y & (sz.y - 1); b.y = b.y & (sz.y - 1); }
    uint w11 = (w.x * w.y + 128u) >> 8u;
    uint w00 = 256u - w.x - w.y + w11, w10 = w.x - w11, w01 = w.y - w11;
    uvec4 s = TeIdx8Colour(ivec2(a.x, a.y)) * (257u * w00) + TeIdx8Colour(ivec2(b.x, a.y)) * (257u * w10) +
              TeIdx8Colour(ivec2(a.x, b.y)) * (257u * w01) + TeIdx8Colour(ivec2(b.x, b.y)) * (257u * w11);
    return floor(vec4((s + 128u) >> 8u) / 65535.0 * 255.0 + 0.5);
}

void main() {
#ifdef TE_GPU_NATIVE





#ifdef TE_GPU_NATIVE_EXACT
    const bool te_fog_native = true;
#else
    bool te_fog_native = u_fog_enabled == 0;
#endif
    if (u_depth_fmt == 0 && u_idx_fmt == 0 && u_sw_blend == 0 && u_date == 0 && u_zsrc_en == 0 && te_fog_native &&
        u_colclamp != 0 && u_fba == 0 && u_fb16 == 0 && u_fbmsk16_en == 0 && u_scanmsk == 0 && u_uv_native == 0 &&
        (u_ate == 0 || u_afail == 0) && !TE_CT16Q_ON) {
#ifdef TE_GPU_NATIVE_EXACT



        vec4 res = v_color;
        if (u_use_texture != 0) {
            float inv_q = (v_texcoord.z != 0.0) ? (1.0 / v_texcoord.z) : 1.0;
            vec2 mip_tc = v_texcoord.xy * inv_q;
            vec2 uv = mip_tc / u_tex_size;
            if (u_flip_v != 0) uv.y = 1.0 - uv.y;
            vec4 vc = floor(v_color * 255.0 + 0.5);
            vec4 tn;
            if (TE_MIP_ON) {
                float q_abs = abs(v_texcoord.z);
                float lod = TE_LOD_FIXED ? u_mip_lodp.x : log2(1.0 / max(q_abs, 1e-30)) * u_mip_lodp.y + u_mip_lodp.x;
#ifdef GSMIP_AAF
                vec2 gsm_tc = mip_tc;
                float gsm_r2 = max(dot(dFdx(gsm_tc), dFdx(gsm_tc)), dot(dFdy(gsm_tc), dFdy(gsm_tc)));
                float gsm_lam = 0.5 * log2(max(gsm_r2, 1e-30));
#if GSMIP_AAF == 2
                lod = gsm_lam;
#else
                lod = max(min(lod, float(u_mip_ctl.x)), gsm_lam);
#endif
#endif
                if (u_tex_lod_hw != 0) tn = texture(u_texture, uv);
                else tn = textureLod(u_texture, uv, lod);
            } else {
                tn = texture(u_texture, uv);
            }
            vec4 tex = floor(tn * 255.0 + 0.5);
            tex.a = (tex.a >= 255.0) ? 128.0 : floor(tex.a * 0.5);
            vec4 FxT = trunc((vc * tex) / 128.0);
            if (u_tfx == 0) { res = FxT; if (u_tcc == 0) res.a = vc.a; }
            else if (u_tfx == 1) { res.rgb = tex.rgb; res.a = (u_tcc == 1) ? tex.a : vc.a; }
            else if (u_tfx == 2) { res.rgb = FxT.rgb + vc.a; res.a = (u_tcc == 1) ? tex.a + vc.a : vc.a; }
            else { res.rgb = FxT.rgb + vc.a; res.a = (u_tcc == 1) ? tex.a : vc.a; }
            res = min(res, 255.0) / 255.0;
        }
        res = clamp(res, 0.0, 1.0);
        if (u_fog_enabled != 0) res.rgb = mix(u_fogcol, res.rgb, v_fog);
        if (u_ate != 0) {
            int a = int(res.a * 255.0 + 0.5);
            bool pass = u_atst == 1 || (u_atst == 2 && a < u_aref) || (u_atst == 3 && a <= u_aref) ||
                        (u_atst == 4 && a == u_aref) || (u_atst == 5 && a >= u_aref) || (u_atst == 6 && a > u_aref) ||
                        (u_atst == 7 && a != u_aref);
            if (!pass) discard;
        }
        res.a = clamp(res.a * (255.0 / 128.0), 0.0, 1.0);
        frag_color = res;
        return;
#else
#ifdef TE_NATIVE_LEAN



        {
            const float kl = 255.0 / 128.0;
            vec4 rl = v_color;
            rl.a = v_color.a * kl;
            vec4 tl = vec4(0.0);
            if (u_use_texture != 0) {
                float ql = v_texcoord.z;
                float inv_ql = 1.0 / ql;
                if (ql == 0.0) inv_ql = 1.0;
                vec2 uvl = v_texcoord.xy * inv_ql * u_tex_rsize;
                if (u_flip_v != 0) uvl.y = 1.0 - uvl.y;
                tl = texture(u_texture, uvl);
                if (u_tcc != 0) rl.a = (u_tfx == 0) ? v_color.a * tl.a * kl : (u_tfx == 2) ? tl.a + v_color.a * kl : tl.a;
            }
            if (u_ate != 0) {
                int al = int(rl.a * 128.0 + 0.5);
                bool passl = u_atst == 1 || (u_atst == 2 && al < u_aref) || (u_atst == 3 && al <= u_aref) ||
                             (u_atst == 4 && al == u_aref) || (u_atst == 5 && al >= u_aref) || (u_atst == 6 && al > u_aref) ||
                             (u_atst == 7 && al != u_aref);
                if (!passl) discard;
            }
            if (u_use_texture != 0)
                rl.rgb = (u_tfx == 1) ? tl.rgb : v_color.rgb * tl.rgb * kl + ((u_tfx >= 2) ? vec3(v_color.a) : vec3(0.0));
            frag_color = clamp(rl, 0.0, 1.0);
            return;
        }
#endif
        const float k = 255.0 / 128.0;
        vec4 r = v_color;
        r.a = v_color.a * k;
#ifdef TE_ATEST_FAST


        vec4 t = vec4(0.0);
#endif
        if (u_use_texture != 0) {
            float inv_q = (v_texcoord.z != 0.0) ? (1.0 / v_texcoord.z) : 1.0;
            vec2 uv = v_texcoord.xy * inv_q / u_tex_size;
            if (u_flip_v != 0) uv.y = 1.0 - uv.y;
#ifdef TE_ATEST_FAST
            t = texture(u_texture, uv);
#else
            vec4 t = texture(u_texture, uv);
#endif

#ifdef TE_ATEST_FAST
#else
            r.rgb = (u_tfx == 1) ? t.rgb : v_color.rgb * t.rgb * k + ((u_tfx >= 2) ? vec3(v_color.a) : vec3(0.0));
#endif
            if (u_tcc != 0) r.a = (u_tfx == 0) ? v_color.a * t.a * k : (u_tfx == 2) ? t.a + v_color.a * k : t.a;
        }
        if (u_ate != 0) {
            int a = int(r.a * 128.0 + 0.5);
            bool pass = u_atst == 1 || (u_atst == 2 && a < u_aref) || (u_atst == 3 && a <= u_aref) ||
                        (u_atst == 4 && a == u_aref) || (u_atst == 5 && a >= u_aref) || (u_atst == 6 && a > u_aref) ||
                        (u_atst == 7 && a != u_aref);
            if (!pass) discard;
        }
#ifdef TE_ATEST_FAST


        if (u_use_texture != 0)
            r.rgb = (u_tfx == 1) ? t.rgb : v_color.rgb * t.rgb * k + ((u_tfx >= 2) ? vec3(v_color.a) : vec3(0.0));
#endif
        frag_color = clamp(r, 0.0, 1.0);
        return;
#endif
    }
#endif




    if (u_scanmsk == 2) {

        if (te_imod(int(gl_FragCoord.y), 2) == 0) discard;
    } else if (u_scanmsk == 3) {

        if (te_imod(int(gl_FragCoord.y), 2) != 0) discard;
    }

    vec4 result;

    if (u_use_texture != 0) {






        float inv_q = (v_texcoord.z != 0.0) ? (1.0 / v_texcoord.z) : 1.0;







        vec2 mip_tc = v_texcoord.xy * inv_q;
        vec2 uvn_rt = mip_tc;
#ifdef S83_IDXSCALE
        if (u_uv_native != 0) uvn_rt = S83NativeCenter(uvn_rt);
#endif

        vec2 uv = uvn_rt / u_tex_size;

        if (u_flip_v != 0) {
            uv.y = 1.0 - uv.y;
        }


        #ifdef TE_ALPHA_FIRST

        vec4 vc = vec4(0.0, 0.0, 0.0, floor(v_color.a * 255.0 + 0.5));
#else
        vec4 vc = floor(v_color * 255.0 + 0.5);
#endif
        vec4 tex;
#ifdef TE_ALPHA_FIRST
        vec3 deferred_rgb = vec3(0.0);
        bool defer_rgb = false;
#endif
        if (u_depth_fmt == 2) {





            float df = texture(u_texture, uv).r;
            uint d = (df >= 1.0) ? 0xFFFFFFFFu
                                 : uint(max(df, 0.0) * exp2(32.0));
            uint z16 = min(d, 0xFFFFu);
            uint r5 = z16 & 0x1Fu;
            uint g5 = (z16 >> 5) & 0x1Fu;
            uint b5 = (z16 >> 10) & 0x1Fu;
            tex = vec4(float((r5 << 3u) | (r5 >> 2u)),
                       float((g5 << 3u) | (g5 >> 2u)),
                       float((b5 << 3u) | (b5 >> 2u)),
                       ((z16 >> 15) != 0u) ? 255.0 : 0.0);
        } else if (u_depth_fmt == 1) {









            float df = texture(u_texture, uv).r;
            uint z = (df >= 1.0) ? 0xFFFFFFFFu
                                 : uint(max(df, 0.0) * exp2(32.0));
            tex = vec4(float( z         & 0xFFu),
                       float((z >>  8u) & 0xFFu),
                       float((z >> 16u) & 0xFFu),
                       0.0);
            uint a8 = (z >> 24u) & 0xFFu;
            tex.a = (a8 >= 128u) ? 255.0 : float(a8 << 1u);
        } else if (u_depth_fmt == 3) {










            vec2  rt = v_texcoord.xy * inv_q;
#ifdef S83_IDXSCALE
            rt = S83NativeCenter(rt);
#endif
            ivec2 t  = ivec2(floor(rt));
            int lx = t.x & 63, ly = t.y & 63;
            int PX = (lx & 7)  | (ly & 0x38);
            int PY = (ly & 7)  | ((lx & 0x30) >> 1);
            int hw = (lx >> 3) & 1;
            int PXg = (t.x & ~63) | PX;
            int PYg = (t.y >> 6) * 32 + PY + u_depth_zoff;

            vec2 uvz = (vec2(PXg, PYg) + 0.5) / u_tex_size;
            if (u_flip_v != 0) uvz.y = 1.0 - uvz.y;
            float df = texture(u_texture, uvz).r;
            uint z = (df >= 1.0) ? 0xFFFFFFFFu
                                 : uint(max(df, 0.0) * exp2(32.0));




            uint z16 = (hw != 0) ? ((z >> 16u) & 0xFFFFu) : (z & 0xFFFFu);
            uint r5 = z16 & 0x1Fu;
            uint g5 = (z16 >> 5) & 0x1Fu;
            uint b5 = (z16 >> 10) & 0x1Fu;
            tex = vec4(float((r5 << 3u) | (r5 >> 2u)),
                       float((g5 << 3u) | (g5 >> 2u)),
                       float((b5 << 3u) | (b5 >> 2u)),
                       ((z16 >> 15u) != 0u) ? 255.0 : 0.0);
        } else if (u_idx_fmt == 7) {










            ivec2 ztc = ivec2(gl_FragCoord.xy);
            ztc = clamp(ztc, ivec2(0), textureSize(u_texture, 0) - 1);
            float df = texelFetch(u_texture, ztc, 0).r;
            uint z = (df >= 1.0) ? 0xFFFFFFFFu
                                 : uint(max(df, 0.0) * exp2(32.0));
            uint idx = (z >> uint(u_idx_self & 31)) & 0xFFu;
            tex = floor(texelFetch(u_clut, ivec2(int(idx), 0), 0) * 255.0 + 0.5);
        } else if (u_idx_fmt == 4) {


















            vec2  rtc = v_texcoord.xy * inv_q;
#ifdef S83_IDXSCALE
            rtc = S83NativeCenter(rtc);
#endif
            ivec2 tdim4 = ivec2(u_tex_size);
            ivec2 tc  = clamp(ivec2(floor(rtc)), ivec2(0), tdim4 - 1);
            if (u_flip_v != 0) tc.y = tdim4.y - 1 - tc.y;
#ifdef S83_IDXSCALE









            {
                ivec2 ts4 = (u_idx_self != 0) ? textureSize(u_rt_texture, 0)
                                              : textureSize(u_texture, 0);
                tc *= max(te_idiv(ts4.x, max(tdim4.x, 1)), 1);
            }
#endif
            vec4 src = (u_idx_self != 0)
                     ? texelFetch(u_rt_texture, tc, 0)
                     : texelFetch(u_texture,   tc, 0);
            uint a_gpu = uint(floor(src.a * 255.0 + 0.5));
            uint idx = (a_gpu >= 255u) ? 128u : (a_gpu >> 1u);



            tex = floor(texelFetch(u_clut, ivec2(int(idx), u_clut_raw), 0) * 255.0 + 0.5);
        } else if (u_idx_fmt == 5) {






















            vec2  rtc = v_texcoord.xy * inv_q;
#ifdef S83_IDXSCALE
            rtc = S83NativeCenter(rtc);
#endif
            ivec2 tc  = ivec2(floor(rtc));
            int ppr  = int(u_tex_size.x) >> 6;
            int page = (tc.y >> 5)*ppr + (tc.x >> 6);
            int lx = tc.x & 63, ly = tc.y & 31;
            int lX = (lx & 7) | 8 | ((ly & 0x18) << 1);
            int lY = (lx & 0x38) | (ly & 7);
            int X  = te_imod(page, ppr)*64 + lX;
            int Y  = te_idiv(page, ppr)*64 + lY;
            int h  = int(u_tex_size.y);
            int wmax = int(u_tex_size.x) - 1;
            vec4 src;
            if (Y < h) {
                ivec2 t0 = clamp(ivec2(X, Y), ivec2(0), ivec2(wmax, h - 1));
                if (u_flip_v != 0) t0.y = h - 1 - t0.y;
#ifdef S83_IDXSCALE

                t0 *= max(te_idiv(textureSize(u_texture, 0).x,
                                  max(int(u_tex_size.x), 1)), 1);
#endif
                src = texelFetch(u_texture, t0, 0);
            } else {
                ivec2 t1 = clamp(ivec2(X, Y - h), ivec2(0), ivec2(wmax, h - 1));
                if (u_flip_v != 0) t1.y = h - 1 - t1.y;
#ifdef S83_IDXSCALE

                t1 *= max(te_idiv(textureSize(u_idx_tex2, 0).x,
                                  max(int(u_tex_size.x), 1)), 1);
#endif
                src = texelFetch(u_idx_tex2, t1, 0);
            }
            uvec3 c = uvec3(floor(src.rgb * 255.0 + 0.5));
            uint a  = uint(floor(src.a * 255.0 + 0.5));
            uint idx = ((c.g >> 6u) & 3u) | ((c.b >> 3u) << 2u)
                     | ((a > 127u) ? 0x80u : 0u);

            tex = floor(texelFetch(u_clut, ivec2(int(idx), u_clut_raw), 0) * 255.0 + 0.5);
        } else if (u_idx_fmt == 6) {





















            vec2  rtc = v_texcoord.xy * inv_q;
#ifdef S83_IDXSCALE
            rtc = S83NativeCenter(rtc);
#endif
            int ppr8   = max(u_idx_self & 15, 1);
            int twl    = (u_idx_self >> 4) & 15;
            int thl    = (u_idx_self >> 8) & 15;




            if ((u_idx_self & 0x1000) == 0) {
                rtc *= vec2(float(1 << twl), float(1 << thl)) / u_tex_size;
            }
            ivec2 tc   = ivec2(floor(rtc));
            tc.x &= (1 << twl) - 1;
            tc.y &= (1 << thl) - 1;
            int ppr16 = int(u_tex_size.x) >> 6;




            int pgoff = (u_idx_self >> 16) & 63;
            int page  = pgoff + (tc.y >> 6)*ppr8 + (tc.x >> 7);
            int br = (tc.x >> 4) & 7;
            int bc = (tc.y >> 4) & 3;
            int lx = tc.x & 15, ly = tc.y & 15;
            int lane = (ly >> 1) & 1;
            int cr = ((ly >> 2) << 1) | (ly & 1);
            int cc = lx ^ ((((ly + 2) >> 2) & 1) << 2);
            int X  = te_imod(page, ppr16)*64 + bc*16 + cc;
            int Y  = te_idiv(page, ppr16)*64 + br*8  + cr;
            int wmax6 = int(u_tex_size.x) - 1;
            int h6    = int(u_tex_size.y);
            ivec2 t6 = clamp(ivec2(X, Y), ivec2(0), ivec2(wmax6, h6 - 1));
            if (u_flip_v != 0) t6.y = h6 - 1 - t6.y;
#ifdef S83_IDXSCALE

            t6 *= max(te_idiv(textureSize(u_texture, 0).x,
                              max(int(u_tex_size.x), 1)), 1);
#endif
            vec4 src = texelFetch(u_texture, t6, 0);


            uvec3 c = uvec3(floor(src.rgb * 255.0 + 0.5));
            uint a  = uint(floor(src.a * 255.0 + 0.5));
            uint hw = (c.r >> 3u) | ((c.g >> 3u) << 5u)
                    | ((c.b >> 3u) << 10u) | ((a > 127u) ? 0x8000u : 0u);
            uint idx = (lane != 0) ? ((hw >> 8u) & 0xFFu) : (hw & 0xFFu);

            tex = floor(texelFetch(u_clut, ivec2(int(idx), u_clut_raw), 0) * 255.0 + 0.5);
        } else if (u_idx_fmt == 8) {
            tex = TeIdx8(uv);
        } else if (TE_MIP_ON) {

            float q_abs = abs(v_texcoord.z);
            float lod = TE_LOD_FIXED
                ? u_mip_lodp.x
                : log2(1.0 / max(q_abs, 1e-30)) * u_mip_lodp.y
                      + u_mip_lodp.x;
#ifdef GSMIP_AAF





            vec2 gsm_tc = mip_tc;
            float gsm_r2 = max(dot(dFdx(gsm_tc), dFdx(gsm_tc)),
                               dot(dFdy(gsm_tc), dFdy(gsm_tc)));
            float gsm_lam = 0.5 * log2(max(gsm_r2, 1e-30));
#if GSMIP_AAF == 2
            lod = gsm_lam;
#else




            lod = max(min(lod, float(u_mip_ctl.x)), gsm_lam);
#endif
#endif
            vec4 tex_norm;
            if (u_tex_lod_hw != 0) tex_norm = texture(u_texture, uv);
            else tex_norm = textureLod(u_texture, uv, lod);
#ifdef TE_ALPHA_FIRST
            tex = vec4(0.0, 0.0, 0.0, floor(tex_norm.a * 255.0 + 0.5));
            deferred_rgb = tex_norm.rgb;
            defer_rgb = true;
#else
            tex = floor(tex_norm * 255.0 + 0.5);
#endif
        } else {
            vec4 tex_norm = texture(u_texture, uv);
#ifdef TE_ALPHA_FIRST
            tex = vec4(0.0, 0.0, 0.0, floor(tex_norm.a * 255.0 + 0.5));
            deferred_rgb = tex_norm.rgb;
            defer_rgb = true;
#else
            tex = floor(tex_norm * 255.0 + 0.5);
#endif
        }













        if (u_idx_fmt == 0 || u_clut_raw == 0) {
            tex.a = (tex.a >= 255.0) ? 128.0 : floor(tex.a * 0.5);
        }






        if (u_ate != 0 && u_afail == 0) {
            float ea;
            if (u_tfx == 0) ea = (u_tcc == 0) ? vc.a : trunc((vc.a * tex.a) / 128.0);
            else if (u_tfx == 2) ea = (u_tcc == 1) ? tex.a + vc.a : vc.a;
            else ea = (u_tcc == 1) ? tex.a : vc.a;
            ea = clamp(min(ea, 255.0) / 255.0, 0.0, 1.0);
            int ea_int = int(ea * 255.0 + 0.5);
            bool ea_pass = u_atst == 1 || (u_atst == 2 && ea_int < u_aref) || (u_atst == 3 && ea_int <= u_aref) ||
                           (u_atst == 4 && ea_int == u_aref) || (u_atst == 5 && ea_int >= u_aref) ||
                           (u_atst == 6 && ea_int > u_aref) || (u_atst == 7 && ea_int != u_aref);
            if (!ea_pass) discard;
        }

#ifdef TE_ALPHA_FIRST
        if (defer_rgb) tex.rgb = floor(deferred_rgb * 255.0 + 0.5);
        vc.rgb = floor(v_color.rgb * 255.0 + 0.5);
#endif



        vec4 FxT = trunc((vc * tex) / 128.0);

        if (u_tfx == 0) {

            result = FxT;
            if (u_tcc == 0) result.a = vc.a;
        } else if (u_tfx == 1) {

            result.rgb = tex.rgb;
            result.a = (u_tcc == 1) ? tex.a : vc.a;
        } else if (u_tfx == 2) {

            result.rgb = FxT.rgb + vc.a;
            result.a = (u_tcc == 1) ? tex.a + vc.a : vc.a;
        } else {

            result.rgb = FxT.rgb + vc.a;
            result.a = (u_tcc == 1) ? tex.a : vc.a;
        }


        result = min(result, 255.0) / 255.0;
    } else {


        result = v_color;
    }




    result = clamp(result, 0.0, 1.0);








    if (u_fog_enabled != 0) {
        float f = v_fog;
        result.rgb = mix(u_fogcol, result.rgb, f);
    }





#ifdef TE_ALPHA_FIRST

    if (u_ate != 0 && !(u_use_texture != 0 && u_afail == 0)) {
#else
    if (u_ate != 0) {
#endif


        int alpha_int = int(result.a * 255.0 + 0.5);
        bool alpha_pass = true;

        if (u_atst == 0) {
            alpha_pass = false;
        } else if (u_atst == 1) {
            alpha_pass = true;
        } else if (u_atst == 2) {
            alpha_pass = (alpha_int < u_aref);
        } else if (u_atst == 3) {
            alpha_pass = (alpha_int <= u_aref);
        } else if (u_atst == 4) {
            alpha_pass = (alpha_int == u_aref);
        } else if (u_atst == 5) {
            alpha_pass = (alpha_int >= u_aref);
        } else if (u_atst == 6) {
            alpha_pass = (alpha_int > u_aref);
        } else {
            alpha_pass = (alpha_int != u_aref);
        }

        if (!alpha_pass) {









            if (u_afail == 0) {
                discard;
            }





            if (u_afail == 2) {

                result = vec4(0.0);
            }
            if (u_afail == 3) {

                result.a = 0.0;
            }
        }
    }









    if (u_date != 0) {
        vec4 dst = texelFetch(u_rt_texture, ivec2(gl_FragCoord.xy), 0);

        int dst_alpha_int = int(dst.a * 255.0 + 0.5);
        bool dst_msb = (dst_alpha_int & 0x80) != 0;
        if (u_datm == 0) {

            if (dst_msb) discard;
        } else {

            if (!dst_msb) discard;
        }
    }















    if (u_zsrc_en != 0) {
        ivec2 zdim = textureSize(u_zsrc_tex, 0);
        int zn = max(u_zsrc_num, 1), zd = max(u_zsrc_den, 1);
        ivec2 zc = clamp(te_idiv(ivec2(gl_FragCoord.xy) * zn, zd), ivec2(0),
                         zdim - ivec2(1));
        vec4 zpix = texelFetch(u_zsrc_tex, zc, 0);
        vec4 zb = floor(zpix * 255.0 + 0.5);
        float dstz = (zb.a * 16777216.0 + zb.b * 65536.0 +
                      zb.g * 256.0 + zb.r) / 4294967296.0;
        if (u_zsrc_en == 3) {
            if (gl_FragCoord.z <= dstz) discard;
        } else {
            if (gl_FragCoord.z < dstz) discard;
        }
    }






    if (u_blend_enabled != 0 && u_sw_blend != 0) {

        vec4 RT = texelFetch(u_rt_texture, ivec2(gl_FragCoord.xy), 0);


        vec3 Cs = result.rgb * 255.0;



        float As = clamp(result.a * (255.0 / 128.0), 0.0, 1.0);
        vec3 Cd = RT.rgb * 255.0;
        float Ad = clamp(RT.a * (255.0 / 128.0), 0.0, 1.0);


        vec3 A_val;
        if (u_blend_a == 0) A_val = Cs;
        else if (u_blend_a == 1) A_val = Cd;
        else A_val = vec3(0.0);


        vec3 B_val;
        if (u_blend_b == 0) B_val = Cs;
        else if (u_blend_b == 1) B_val = Cd;
        else B_val = vec3(0.0);


        float C_val;
        if (u_blend_c == 0) C_val = As;
        else if (u_blend_c == 1) C_val = Ad;
        else C_val = u_fix_alpha;


        vec3 D_val;
        if (u_blend_d == 0) D_val = Cs;
        else if (u_blend_d == 1) D_val = Cd;
        else D_val = vec3(0.0);


        vec3 blended = (A_val - B_val) * C_val + D_val;








        if (u_colclamp != 0) {
            result.rgb = clamp(blended, vec3(0.0), vec3(255.0)) / 255.0;
        } else {
            result.rgb = mod(floor(blended + 0.5), 256.0) / 255.0;
        }

    } else if (u_blend_enabled == 0) {



    }






    if (u_colclamp != 0) {

        result.rgb = clamp(result.rgb, vec3(0.0), vec3(1.0));
    } else if (u_blend_enabled == 0 || u_sw_blend == 0) {







        result.rgb = fract(result.rgb);
    }

    result.a = clamp(result.a, 0.0, 1.0);







    if (u_fba != 0) {
        int a_int = int(result.a * 255.0 + 0.5);
        a_int = a_int | 0x80;
        result.a = float(min(a_int, 255)) / 255.0;
    }




























    if (u_fb16 != 0 && (u_blend_enabled == 0 || u_sw_blend != 0)) {
        result.a = (int(result.a * 255.0 + 0.5) >= 128) ? 1.0 : 0.0;
    } else if (u_sw_blend == 0) {
        result.a = clamp(result.a * (255.0 / 128.0), 0.0, 1.0);
    }










    if (TE_CT16Q_ON && (u_blend_enabled == 0 || u_sw_blend != 0)) {
        ivec3 cq = ivec3(result.rgb * 255.0 + 0.5);
        if (u_dthe != 0) {
            int nx = te_idiv(int(gl_FragCoord.x), u_ct16q) & 3;
            int ny = te_idiv(int(gl_FragCoord.y), u_ct16q) & 3;
            int sh = ny * 16 + nx * 4;
            uint w = uint((ny < 2) ? u_dimx_lo : u_dimx_hi);
            int dm = int((w >> uint(sh & 31)) & 7u);
            if (dm >= 4) dm -= 8;
            cq = clamp(cq + ivec3(dm), ivec3(0), ivec3(255));
        }
        ivec3 q5 = cq >> 3;











        result.rgb = vec3(q5 << 3) / 255.0;
    }










    if (u_fbmsk16_en != 0) {
        ivec4 s = ivec4(result * 255.0 + 0.5);
        int src_bits = ((s.r >> 3) & 0x1F)
                     | (((s.g >> 3) & 0x1F) << 5)
                     | (((s.b >> 3) & 0x1F) << 10)
                     | ((s.a > 127 ? 1 : 0) << 15);
        vec4 dst = texelFetch(u_rt_texture, ivec2(gl_FragCoord.xy), 0);
        ivec4 d = ivec4(dst * 255.0 + 0.5);
        int dst_bits = ((d.r >> 3) & 0x1F)
                     | (((d.g >> 3) & 0x1F) << 5)
                     | (((d.b >> 3) & 0x1F) << 10)
                     | ((d.a > 127 ? 1 : 0) << 15);
        int merged = (src_bits & ~u_fbmsk16) | (dst_bits & u_fbmsk16);
        int r5 = merged & 0x1F;
        int g5 = (merged >> 5) & 0x1F;
        int b5 = (merged >> 10) & 0x1F;
        result.r = float((r5 << 3) | (r5 >> 2)) / 255.0;
        result.g = float((g5 << 3) | (g5 >> 2)) / 255.0;
        result.b = float((b5 << 3) | (b5 >> 2)) / 255.0;
        result.a = ((merged & 0x8000) != 0) ? 1.0 : 0.0;
    }

    frag_color = result;
}
)GLSL";

const char* const kScanoutVertSrc = R"GLSL(
#version 410 core

layout(location = 0) in vec2 a_position;
layout(location = 1) in vec2 a_texcoord;

out vec2 v_texcoord;

void main() {
    gl_Position = vec4(a_position, 0.0, 1.0);
    v_texcoord = a_texcoord;
}
)GLSL";

const char* const kScanoutFragSrc = R"GLSL(
#version 410 core

in vec2 v_texcoord;
uniform sampler2D u_texture;
out vec4 frag_color;

void main() {
    vec4 c = texture(u_texture, v_texcoord);


    frag_color = vec4(c.rgb, 1.0);
}
)GLSL";

const char* const kScanoutFxFragSrc = R"GLSL(
#version 410 core

in vec2 v_texcoord;
uniform sampler2D u_texture;
uniform vec4 u_fx;
uniform float u_merge_x;
uniform vec4 u_crop;
out vec4 frag_color;

vec3 te_tap(vec2 uv) {
    return textureLod(u_texture, clamp(uv, u_crop.xy, u_crop.zw), 0.0).rgb;
}

float te_luma(vec3 c) {
    return dot(c, vec3(0.299, 0.587, 0.114));
}

vec3 te_pix(vec2 uv) {
    vec3 c = te_tap(uv + vec2(u_merge_x, 0.0));
    if (u_fx.z > 0.0 || u_merge_x > 0.0) c = 0.5 * (c + te_tap(uv - vec2(u_merge_x, u_fx.z)));
    return c;
}

void main() {
    vec2 posM = v_texcoord;
    if (u_fx.w <= 0.5) {
        frag_color = vec4(te_pix(posM), 1.0);
        return;
    }
    vec2 px = u_fx.xy;
    vec3 rgbM = te_tap(posM);
    float lumaM = te_luma(rgbM);
    float lumaN = te_luma(te_tap(posM + vec2(0.0, -px.y)));
    float lumaS = te_luma(te_tap(posM + vec2(0.0, px.y)));
    float lumaW = te_luma(te_tap(posM + vec2(-px.x, 0.0)));
    float lumaE = te_luma(te_tap(posM + vec2(px.x, 0.0)));
    float rangeMax = max(max(lumaN, lumaS), max(max(lumaW, lumaE), lumaM));
    float rangeMin = min(min(lumaN, lumaS), min(min(lumaW, lumaE), lumaM));
    float range = rangeMax - rangeMin;
    if (range < max(0.0625, rangeMax * 0.125)) {
        frag_color = vec4(te_pix(posM), 1.0);
        return;
    }
    float lumaNW = te_luma(te_tap(posM + vec2(-px.x, -px.y)));
    float lumaNE = te_luma(te_tap(posM + vec2(px.x, -px.y)));
    float lumaSW = te_luma(te_tap(posM + vec2(-px.x, px.y)));
    float lumaSE = te_luma(te_tap(posM + vec2(px.x, px.y)));
    float lumaNS = lumaN + lumaS;
    float lumaWE = lumaW + lumaE;
    float subpixNSWE = lumaNS + lumaWE;
    float edgeHorz1 = -2.0 * lumaM + lumaNS;
    float edgeVert1 = -2.0 * lumaM + lumaWE;
    float lumaNESE = lumaNE + lumaSE;
    float lumaNWNE = lumaNW + lumaNE;
    float lumaNWSW = lumaNW + lumaSW;
    float lumaSWSE = lumaSW + lumaSE;
    float edgeHorz2 = -2.0 * lumaE + lumaNESE;
    float edgeVert2 = -2.0 * lumaN + lumaNWNE;
    float edgeHorz3 = -2.0 * lumaW + lumaNWSW;
    float edgeVert3 = -2.0 * lumaS + lumaSWSE;
    float edgeHorz = abs(edgeHorz3) + (abs(edgeHorz1) * 2.0 + abs(edgeHorz2));
    float edgeVert = abs(edgeVert3) + (abs(edgeVert1) * 2.0 + abs(edgeVert2));
    float subpixNWSWNESE = lumaNWSW + lumaNESE;
    bool horzSpan = edgeHorz >= edgeVert;
    float lengthSign = horzSpan ? px.y : px.x;
    float subpixA = subpixNSWE * 2.0 + subpixNWSWNESE;
    if (!horzSpan) { lumaN = lumaW; lumaS = lumaE; }
    float subpixB = subpixA * (1.0 / 12.0) - lumaM;
    float gradientN = lumaN - lumaM;
    float gradientS = lumaS - lumaM;
    float lumaNN = lumaN + lumaM;
    float lumaSS = lumaS + lumaM;
    bool pairN = abs(gradientN) >= abs(gradientS);
    float gradient = max(abs(gradientN), abs(gradientS));
    if (pairN) lengthSign = -lengthSign;
    float subpixC = clamp(abs(subpixB) / range, 0.0, 1.0);
    vec2 posB = posM;
    vec2 offNP = horzSpan ? vec2(px.x, 0.0) : vec2(0.0, px.y);
    if (!horzSpan) posB.x += lengthSign * 0.5;
    if (horzSpan) posB.y += lengthSign * 0.5;
    vec2 posN = posB - offNP;
    vec2 posP = posB + offNP;
    float subpixD = -2.0 * subpixC + 3.0;
    float lumaEndN = te_luma(te_tap(posN));
    float subpixE = subpixC * subpixC;
    float lumaEndP = te_luma(te_tap(posP));
    if (!pairN) lumaNN = lumaSS;
    float gradientScaled = gradient * 0.25;
    float lumaMM = lumaM - lumaNN * 0.5;
    float subpixF = subpixD * subpixE;
    bool lumaMLTZero = lumaMM < 0.0;
    lumaEndN -= lumaNN * 0.5;
    lumaEndP -= lumaNN * 0.5;
    bool doneN = abs(lumaEndN) >= gradientScaled;
    bool doneP = abs(lumaEndP) >= gradientScaled;
    for (int i = 0; i < 7; i++) {
        if (doneN && doneP) break;
        float stp = i == 0 ? 1.5 : (i < 5 ? 2.0 : (i == 5 ? 4.0 : 8.0));
        if (!doneN) posN -= offNP * stp;
        if (!doneP) posP += offNP * stp;
        if (!doneN) lumaEndN = te_luma(te_tap(posN)) - lumaNN * 0.5;
        if (!doneP) lumaEndP = te_luma(te_tap(posP)) - lumaNN * 0.5;
        doneN = abs(lumaEndN) >= gradientScaled;
        doneP = abs(lumaEndP) >= gradientScaled;
    }
    float dstN = horzSpan ? posM.x - posN.x : posM.y - posN.y;
    float dstP = horzSpan ? posP.x - posM.x : posP.y - posM.y;
    bool goodSpanN = (lumaEndN < 0.0) != lumaMLTZero;
    float spanLength = dstP + dstN;
    bool goodSpanP = (lumaEndP < 0.0) != lumaMLTZero;
    bool directionN = dstN < dstP;
    float dst = min(dstN, dstP);
    bool goodSpan = directionN ? goodSpanN : goodSpanP;
    float subpixG = subpixF * subpixF;
    float pixelOffset = dst * (-1.0 / spanLength) + 0.5;
    float subpixH = subpixG * 0.75;
    float pixelOffsetSubpix = max(goodSpan ? pixelOffset : 0.0, subpixH);
    if (!horzSpan) posM.x += pixelOffsetSubpix * lengthSign;
    if (horzSpan) posM.y += pixelOffsetSubpix * lengthSign;
    frag_color = vec4(te_pix(posM), 1.0);
}
)GLSL";

const char* const kShadowCountFragSrc = R"GLSL(
#version 410 core
void main() {}
)GLSL";

const char* const kStencilClearFragSrc = R"(
#version 410 core

noperspective in vec3 v_texcoord;
in vec4 v_color;
in float v_fog;

uniform int u_fb16;

out vec4 frag_color;

void main() {

    vec4 result = v_color;
    result = clamp(result, 0.0, 1.0);

    result.rgb = clamp(result.rgb, vec3(0.0), vec3(1.0));

    result.a = clamp(result.a, 0.0, 1.0);


    if (u_fb16 != 0) {
        result.a = (int(result.a * 255.0 + 0.5) >= 128) ? 1.0 : 0.0;
    }
    frag_color = result;
}
)";

const char* const kStencilStripFragSrc = R"(
#version 410 core

noperspective in vec3 v_texcoord;
in vec4 v_color;
in float v_fog;

uniform sampler2D u_rt_texture;
uniform sampler2D u_zsrc_tex;
uniform int u_zsrc_en;
uniform int u_datm;






uniform int u_zsrc_num;
uniform int u_zsrc_den;


#ifndef TE_INT_DIVIDE_IN_FLOAT
ivec2 te_idiv(ivec2 a, int b) { return a / b; }
#else
ivec2 te_idiv(ivec2 a, int b) {
    return ivec2(int(floor((float(a.x) + 0.5) / float(b))), int(floor((float(a.y) + 0.5) / float(b))));
}
#endif

void main() {

    {
        vec4 dst = texelFetch(u_rt_texture, ivec2(gl_FragCoord.xy), 0);
        int dst_alpha_int = int(dst.a * 255.0 + 0.5);
        bool dst_msb = (dst_alpha_int & 0x80) != 0;
        if (u_datm == 0) {
            if (dst_msb) discard;
        } else {
            if (!dst_msb) discard;
        }
    }

    if (u_zsrc_en != 0) {
        ivec2 zdim = textureSize(u_zsrc_tex, 0);
        int zn = max(u_zsrc_num, 1), zd = max(u_zsrc_den, 1);
        ivec2 zc = clamp(te_idiv(ivec2(gl_FragCoord.xy) * zn, zd), ivec2(0),
                         zdim - ivec2(1));
        vec4 zpix = texelFetch(u_zsrc_tex, zc, 0);
        vec4 zb = floor(zpix * 255.0 + 0.5);
        float dstz = (zb.a * 16777216.0 + zb.b * 65536.0 +
                      zb.g * 256.0 + zb.r) / 4294967296.0;
        if (u_zsrc_en == 3) {
            if (gl_FragCoord.z <= dstz) discard;
        } else {
            if (gl_FragCoord.z < dstz) discard;
        }
    }

}
)";

const char* const kStencilInvertFragSrc = R"(
#version 410 core

noperspective in vec3 v_texcoord;
in vec4 v_color;
in float v_fog;

uniform sampler2D u_rt_texture;
uniform float u_fix_alpha;

out vec4 frag_color;

void main() {
    float inv_q = (v_texcoord.z != 0.0) ? (1.0 / v_texcoord.z) : 1.0;
    vec2  rtc = v_texcoord.xy * inv_q;
    ivec2 tdim = textureSize(u_rt_texture, 0);
    ivec2 tc = clamp(ivec2(floor(rtc)), ivec2(0), tdim - 1);
    vec4 src = texelFetch(u_rt_texture, tc, 0);

    uint a_gpu = uint(floor(src.a * 255.0 + 0.5));
    uint idx = (a_gpu >= 255u) ? 128u : (a_gpu >> 1u);







    uint w32 = idx & 31u;
    uint sidx = (w32 >= 8u && w32 < 16u) ? (idx + 8u)
              : ((w32 >= 16u && w32 < 24u) ? (idx - 8u) : idx);
    vec4 result = vec4(1.0, 1.0, 1.0, (255.0 - float(sidx)) / 255.0);
    result = clamp(result, 0.0, 1.0);


    {
        vec4 RT = texelFetch(u_rt_texture, ivec2(gl_FragCoord.xy), 0);
        vec3 Cs = result.rgb * 255.0;
        vec3 Cd = RT.rgb * 255.0;
        vec3 A_val = Cs;
        vec3 B_val = Cd;
        float C_val = u_fix_alpha;
        vec3 D_val = vec3(0.0);
        vec3 blended = (A_val - B_val) * C_val + D_val;

        result.rgb = clamp(blended, vec3(0.0), vec3(255.0)) / 255.0;
    }
    result.rgb = clamp(result.rgb, vec3(0.0), vec3(1.0));
    result.a = clamp(result.a, 0.0, 1.0);


    frag_color = result;
}
)";

const char* const kConstVertSrc = R"(
#version 410 core
layout(location = 0) in vec2 a_position;
void main() { gl_Position = vec4(a_position, 0.0, 1.0); }
)";
const char* const kConstFragSrc = R"(
#version 410 core
uniform vec4 u_color;
out vec4 frag_color;
void main() { frag_color = u_color; }
)";

const char* const kOpaqueMaskFragSrc = R"(
#version 410 core
uniform sampler2D u_frame;
void main() {
    vec4 px = texelFetch(u_frame, ivec2(gl_FragCoord.xy), 0);
    if (int(px.a * 255.0 + 0.5) < 255) discard;
}
)";

const char* const kYuvFragSrc = R"(
#version 410 core
uniform sampler2D u_yuv_y;
uniform sampler2D u_yuv_u;
uniform sampler2D u_yuv_v;
uniform sampler2D u_yuv_rb;
uniform sampler2D u_yuv_g;
out vec4 frag_color;
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    int y = int(texelFetch(u_yuv_y, p, 0).r * 255.0 + 0.5);
    int u = int(texelFetch(u_yuv_u, p >> 1, 0).r * 255.0 + 0.5);
    int v = int(texelFetch(u_yuv_v, p >> 1, 0).r * 255.0 + 0.5);
    float r = texelFetch(u_yuv_rb, ivec2(y, v), 0).r;
    float b = texelFetch(u_yuv_rb, ivec2(y, 256 + u), 0).r;
    float g = texelFetch(u_yuv_g, ivec2(y + ((u & 15) << 8), (v << 4) | (u >> 4)), 0).r;
    frag_color = vec4(r, g, b, 1.0);
}
)";

const char* const kHazeFragSrc = R"(
#version 410 core
uniform sampler2D u_depth;
uniform sampler2D u_pal;
uniform int u_haze_last;
out vec4 frag_color;
void main() {
    float d = texelFetch(u_depth, ivec2(gl_FragCoord.xy), 0).r;
    int k = int(floor(d * 16777215.0 + 0.5));
    if (k > u_haze_last) discard;
    vec4 p = texelFetch(u_pal, ivec2(k, 0), 0);
    float as = min(p.a * (255.0 / 128.0), 1.0);
    frag_color = vec4(p.rgb, 1.0 - as);
}
)";
