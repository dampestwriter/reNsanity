#pragma once
#include <arm_neon.h>
namespace te::gfx::recordmath {
using V = float32x4_t;
inline V Load(const float* p) { return vld1q_f32(p); }
inline V Splat(float x) { return vdupq_n_f32(x); }
inline V Add(V a, V b) { return vaddq_f32(a,b); }
inline V Mul(V a, V b) { return vmulq_f32(a,b); }
inline void Store(float* p,V a) { vst1q_f32(p,a); }
template<class M> inline void Corners(const float* b, const M& m, float (&out)[8][4]) {
    const V x=Load(m[0].data()), y=Load(m[1].data()), z=Load(m[2].data()), t=Load(m[3].data());
    const V tx[2]={Add(t,Mul(x,Splat(b[0]))),Add(t,Mul(x,Splat(b[3])))};
    const V yy[2]={Mul(y,Splat(b[1])),Mul(y,Splat(b[4]))};
    const V zz[2]={Mul(z,Splat(b[2])),Mul(z,Splat(b[5]))};
    for (int j=0;j<2;++j) for (int i=0;i<2;++i) {
        const V xy=Add(tx[i],yy[j]);
        Store(out[i+2*j],Add(xy,zz[0]));
        Store(out[i+2*j+4],Add(xy,zz[1]));
    }
}
}
