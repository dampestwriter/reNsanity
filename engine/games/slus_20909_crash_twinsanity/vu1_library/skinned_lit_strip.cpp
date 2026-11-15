#include "games/slus_20909_crash_twinsanity/vu1_library/skinned_lit_strip.h"

#include "engine/vu/vu_chop_round_guard.h"
#include "engine/vu/vu_float.h"

using namespace vu1lib;
#include "games/slus_20909_crash_twinsanity/vu1_library/skin1040_lanes.h"

namespace {

struct Bone {
    V4 c0, c1, c2, tr;
};

Bone LoadBone(QwPtr m, u32 idx) {
    Bone b;
    for (int k = 0; k < 4; ++k) {
        b.c0.b[k] = m.W(idx + 13, k);
        b.c1.b[k] = m.W(idx + 14, k);
        b.c2.b[k] = m.W(idx + 15, k);
        b.tr.b[k] = m.W(idx + 16, k);
    }
    return b;
}

V4 Load(QwPtr m, u32 qw) {
    V4 v;
    for (int k = 0; k < 4; ++k) v.b[k] = m.W(qw, k);
    return v;
}

}

bool SkinnedLitStrip::RunScalePhase(QwPtr m, u32 top, u32* stale_w_out) {
    const u32 count = m.W(top + 1, 0) & 0xFFFF;
    if (count == 0 || (count & 3) != 0) return false;
    const u32 base = top + 3;
    const float fsx = CF(m.F(top + 2, 0));
    const float fsy = CF(m.F(top + 2, 1));
    for (u32 v = 0; v < count; v += 4) {
        for (int p = 0; p < 2; ++p) {
            const u32 qw = base + v + p;
            const float s = p ? fsy : fsx;
            for (int k = 0; k < 4; ++k) {
                const float it = (float)(int32_t)m.W(qw, k);
                m.SetW(qw, k, f2bits(CF(CF(it) * s)));
            }
        }
    }
    if (stale_w_out) {

        const u32 mod = count & 15;
        u32 koff;
        bool conv;
        if (mod == 4) {
            koff = count + 9;
            conv = false;
        } else if (mod == 8) {
            koff = count + 5;
            conv = true;
        } else if (mod == 12) {
            koff = count + 1;
            conv = true;
        } else {
            koff = count + 13;
            conv = false;
        }
        const u32 raw = m.W(base + koff, 3);
        *stale_w_out = conv ? f2bits((float)(int32_t)raw) : raw;
    }
    return true;
}

bool SkinnedLitStrip::RunSkinCb0(QwPtr m, u32 top) {
    u32 stale_w = 0;
    if (!RunScalePhase(m, top, &stale_w)) return false;

    const u32 count = m.W(top + 1, 0) & 0xFFFF;
    const u32 base = top + 3;
    for (u32 v = 0; v < count; v += 4) {
        const u32 a = base + v;
        V4 pos, st, wq;
        for (int k = 0; k < 4; ++k) {
            pos.b[k] = m.W(a, k);
            st.b[k] = m.W(a + 1, k);
            wq.b[k] = m.W(a + 2, k);
        }
        const u32 flags16 = wq.b[3] & 0xFFFF;
        const u32 cnt = flags16 & 3;
        const float wx = CF(bits2f(wq.b[0]));
        const float wy = CF(bits2f(wq.b[1]));
        const float wz = CF(bits2f(wq.b[2]));

        float pw0[3], pw1[3], pw2[3];
        for (int k = 0; k < 3; ++k) {
            const float p = CF(bits2f(pos.b[k]));
            pw0[k] = CF(p * wx);
            pw1[k] = CF(p * wy);
            pw2[k] = CF(p * wz);
        }
        const Bone b0 = LoadBone(m, wq.b[0] & 0x1FF);

        const float npw = CF(bits2f(pos.b[3]));
        const float nsz = CF(bits2f(st.b[2]));
        const float nsw = CF(bits2f(st.b[3]));
        float nrm[3];
        for (int k = 0; k < 3; ++k) {
            float acc = CF(CF(bits2f(b0.c0.b[k])) * npw);
            acc = CF(VuFloat::Fma(CF(bits2f(b0.c1.b[k])), nsz, acc));
            nrm[k] = CF(VuFloat::Fma(CF(bits2f(b0.c2.b[k])), nsw, acc));
        }

        float po[3];
        if (cnt == 1) {

            for (int k = 0; k < 3; ++k) {
                float acc = CF(CF(bits2f(b0.tr.b[k])) * wx);
                acc = CF(VuFloat::Fma(CF(bits2f(b0.c0.b[k])), CF(pw0[0]), acc));
                acc = CF(VuFloat::Fma(CF(bits2f(b0.c1.b[k])), CF(pw0[1]), acc));
                po[k] = CF(VuFloat::Fma(CF(bits2f(b0.c2.b[k])), CF(pw0[2]), acc));
            }
        } else {

            const Bone b1 = LoadBone(m, wq.b[1] & 0x1FF);
            float v31[3];
            for (int k = 0; k < 3; ++k) {
                float acc = CF(CF(bits2f(b0.tr.b[k])) * wx);
                acc = CF(VuFloat::Fma(CF(bits2f(b0.c0.b[k])), CF(pw0[0]), acc));
                acc = CF(VuFloat::Fma(CF(bits2f(b0.c1.b[k])), CF(pw0[1]), acc));
                v31[k] = CF(VuFloat::Fma(CF(bits2f(b0.c2.b[k])), CF(pw0[2]), acc));
            }
            if (cnt == 2) {

                for (int k = 0; k < 3; ++k) {
                    float acc = CF(CF(bits2f(b1.tr.b[k])) * wy);
                    acc = CF(VuFloat::Fma(CF(bits2f(b1.c0.b[k])), CF(pw1[0]), acc));
                    acc = CF(VuFloat::Fma(CF(v31[k]), 1.0f, acc));
                    acc = CF(VuFloat::Fma(CF(bits2f(b1.c1.b[k])), CF(pw1[1]), acc));
                    po[k] = CF(VuFloat::Fma(CF(bits2f(b1.c2.b[k])), CF(pw1[2]), acc));
                }
            } else {
                const Bone b2 = LoadBone(m, wq.b[2] & 0x1FF);
                for (int k = 0; k < 3; ++k) {

                    float acc = CF(CF(bits2f(b1.tr.b[k])) * wy);
                    acc = CF(VuFloat::Fma(CF(bits2f(b1.c0.b[k])), CF(pw1[0]), acc));
                    acc = CF(VuFloat::Fma(CF(v31[k]), 1.0f, acc));
                    acc = CF(VuFloat::Fma(CF(bits2f(b1.c1.b[k])), CF(pw1[1]), acc));
                    const float two = CF(VuFloat::Fma(CF(bits2f(b1.c2.b[k])), CF(pw1[2]), acc));

                    acc = CF(CF(two) * 1.0f);
                    acc = CF(VuFloat::Fma(CF(bits2f(b2.tr.b[k])), wz, acc));
                    acc = CF(VuFloat::Fma(CF(bits2f(b2.c0.b[k])), CF(pw2[0]), acc));
                    acc = CF(VuFloat::Fma(CF(bits2f(b2.c1.b[k])), CF(pw2[1]), acc));
                    po[k] = CF(VuFloat::Fma(CF(bits2f(b2.c2.b[k])), CF(pw2[2]), acc));
                }
            }
        }

        for (int k = 0; k < 3; ++k) m.SetW(a, k, f2bits(po[k]));
        m.SetW(a, 3, stale_w);

        m.SetW(a + 1, 0, f2bits(CF(CF(bits2f(st.b[0])) * 1.0f)));
        m.SetW(a + 1, 1, f2bits(CF(CF(bits2f(st.b[1])) * 1.0f)));
        m.SetW(a + 1, 2, f2bits(CF(CF(0.0f) + CF(1.0f))));
        m.SetW(a + 1, 3, f2bits(CF(CF(bits2f(st.b[3])) * 1.0f)));

        for (int k = 0; k < 3; ++k) m.SetW(a + 2, k, f2bits(CF(CF(nrm[k]) + CF(0.0f))));
        const u32 adcw = (flags16 & 0x8000) ? (0xFFFF0000u | flags16) : flags16;
        m.SetW(a + 2, 3, adcw);
    }
    return true;
}

bool SkinnedLitStrip::RunSkin1040(QwPtr m, u32 top, u32 entry_vf19_w) {
    return skin1040::Run(m,top,entry_vf19_w);
}

bool SkinnedLitStrip::RunClipMark0B30(QwPtr m, u32 top) {

    const u32 head = m.W(3, 0) & 0xFFFF;
    const u32 count = m.W(top + 1, 0) & 0xFFFF;
    if (count == 0 || (count & 3) != 0) return false;
    V4 rows[5];
    for (int r = 0; r < 5; ++r)
        for (int k = 0; k < 4; ++k) rows[r].b[k] = m.W(head + 89 + r, k);
    const u32 vbase = top + 3;
    u32 h1 = 0, h2 = 0;
    for (u32 j = 0; j < count / 4; ++j) {
        const u32 vq = vbase + j * 4;
        const float vx = CF(m.F(vq, 0)), vy = CF(m.F(vq, 1)), vz = CF(m.F(vq, 2));
        const V4 t = XformClip(rows[0], rows[1], rows[2], rows[3], vx, vy, vz);
        const V4 g = GuardScale(rows[4], t);
        const u32 cur = (ClipNf(g.b[0], g.b[1], g.b[2], g.b[3]) != 0) ? 1u : 0u;
        const u32 orig = m.W(vq + 2, 3) & 0xFFFF;
        const u32 vi05 = (((cur | h1 | h2) + 0x7FFF) | orig) & 0xFFFF;
        const u32 wbits = (vi05 & 0x8000) ? (0xFFFF0000u | vi05) : vi05;
        m.SetW(vq + 2, 3, wbits);
        h2 = h1;
        h1 = cur;
    }
    return true;
}

bool SkinnedLitStrip::RunSkinnedLitBody(QwPtr m, u32 node, u32 top) {
    auto load = [&](u32 qw) { return Load(m, qw); };

    const V4 payload = load(node + 2);
    const u32 first = m.W(3, 0) & 0xFFFF;
    V4 obj[4], nrmR[3], litR[4];
    for (int r = 0; r < 4; ++r) obj[r] = load(first + 78 + r);
    for (int r = 0; r < 3; ++r) nrmR[r] = load(first + 82 + r);
    for (int r = 0; r < 4; ++r) litR[r] = load(first + 85 + r);

    const u32 vqw_count = m.W(top + 1, 0) & 0xFFFF;
    if ((vqw_count & 3) != 0) return false;
    const u32 nverts = vqw_count / 4;
    if (nverts < 3) return false;
    const u32 vbase = top + 3;

    const u32 out_base = m.W(1, 0) & 0xFFFF;
    for (int k = 0; k < 4; ++k) m.SetW(out_base, k, m.W(top, k));

    skin1040::Lanes obj_l[4], nrm_l[3], lit_l[4];
    for(int r=0;r<4;++r){obj_l[r]=skin1040::Load(obj[r]);lit_l[r]=skin1040::Load(litR[r]);}
    for(int r=0;r<3;++r)nrm_l[r]=skin1040::Load(nrmR[r]);
    for (u32 j = 0; j < nverts; ++j) {
        const u32 vq = vbase + j * 4;
        const V4 pos = load(vq);
        const V4 stuv = load(vq + 1);
        const V4 nrm = load(vq + 2);
        const V4 st_col = load(vq + 3);

        V4 T;
        {
            auto t=skin1040::Mul(obj_l[3],1.f);
            t=skin1040::Madd(obj_l[1],CF(bits2f(pos.b[1])),t);
            t=skin1040::Madd(obj_l[2],CF(bits2f(pos.b[2])),t);
            t=skin1040::Madd(obj_l[0],CF(bits2f(pos.b[0])),t);
            float f[4];skin1040::Store(t,f);for(int k=0;k<4;++k)T.b[k]=f2bits(f[k]);
        }
        const float Q = VuDiv(1.0f, bits2f(T.b[3]));

        const float nx = CF(bits2f(nrm.b[0])), ny = CF(bits2f(nrm.b[1])), nz = CF(bits2f(nrm.b[2]));
        float wn[3];
        {
            auto n=skin1040::Mul(nrm_l[0],nx);n=skin1040::Madd(nrm_l[1],ny,n);n=skin1040::Madd(nrm_l[2],nz,n);
            float f[4];skin1040::Store(n,f);for(int k=0;k<3;++k)wn[k]=f[k];
        }

        u32 rgba[4];
        {
            const float wcx = bits2f(Fmax_raw(f2bits(wn[0]), 0u));
            const float wcy = bits2f(Fmax_raw(f2bits(wn[1]), 0u));
            const float wcz = bits2f(Fmax_raw(f2bits(wn[2]), 0u));
            float lit[3];
            {
                auto l=skin1040::Mul(lit_l[3],1.f);l=skin1040::Madd(lit_l[0],CF(wcx),l);l=skin1040::Madd(lit_l[1],CF(wcy),l);l=skin1040::Madd(lit_l[2],CF(wcz),l);
                float f[4];skin1040::Store(l,f);for(int k=0;k<3;++k)lit[k]=f[k];
            }
            for (int k = 0; k < 3; ++k) {
                const float s = (float)(int32_t)st_col.b[k];
                const float mod = CF(CF(lit[k]) * CF(s));
                rgba[k] = Ftoi0(Fmin_raw(f2bits(mod), f2bits(255.0f)));
            }
            const float sa = (float)(int32_t)st_col.b[3];
            rgba[3] = Ftoi0(Fmin_raw(f2bits(sa), f2bits(255.0f)));
        }

        const u32 st_qw = out_base + 1 + 3 * j;
        for (int k = 0; k < 3; ++k) {
            const float stp = CF(CF(bits2f(stuv.b[k])) + CF(bits2f(payload.b[k])));
            m.SetW(st_qw, k, f2bits(CF(CF(stp) * Q)));
        }
        for (int k = 0; k < 4; ++k) m.SetW(st_qw + 1, k, rgba[k]);
        m.SetW(st_qw + 2, 0, Ftoi0(f2bits(CF(CF(bits2f(T.b[0])) * Q))));
        m.SetW(st_qw + 2, 1, Ftoi0(f2bits(CF(CF(bits2f(T.b[1])) * Q))));
        m.SetW(st_qw + 2, 2, Ftoi0(f2bits(CF(CF(bits2f(T.b[2])) * Q))));
        m.SetW(st_qw + 2, 3, nrm.b[3]);
    }
    return true;
}
