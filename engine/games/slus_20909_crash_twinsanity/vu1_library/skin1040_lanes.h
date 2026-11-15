#pragma once
#include <arm_neon.h>

namespace skin1040 {
using Lanes=float32x4_t;
inline Lanes Clamp(Lanes v){
    uint32x4_t b=vreinterpretq_u32_f32(v),e=vandq_u32(b,vdupq_n_u32(0x7f800000));
    const auto sign=vandq_u32(b,vdupq_n_u32(0x80000000));
    b=vbslq_u32(vceqq_u32(e,vdupq_n_u32(0)),sign,b);
    b=vbslq_u32(vceqq_u32(e,vdupq_n_u32(0x7f800000)),vorrq_u32(sign,vdupq_n_u32(0x7f7fffff)),b);
    return vreinterpretq_f32_u32(b);
}
inline Lanes Load(const vu1lib::V4& v){return Clamp(vreinterpretq_f32_u32(vld1q_u32(v.b)));}
inline Lanes Mul(Lanes a,float b){return Clamp(vmulq_n_f32(a,b));}
inline Lanes Madd(Lanes a,float b,Lanes c){return Clamp(vfmaq_n_f32(c,a,b));}
inline void Store(Lanes v,float* p){vst1q_f32(p,v);}

inline Lanes Add(Lanes a,Lanes b){
 return Clamp(vaddq_f32(a,b));
}
inline Lanes Sub(Lanes a,Lanes b){
 return Clamp(vsubq_f32(a,b));
}
inline Lanes Product(Lanes a,Lanes b){
 return Clamp(vmulq_f32(a,b));
}
inline Lanes LoadWords(const uint32_t* p){vu1lib::V4 v;std::memcpy(v.b,p,16);return Load(v);}
inline void StoreMask(Lanes value,uint32_t* p,uint8_t mask){
 float f[4];Store(value,f);for(int k=0;k<4;++k)if(mask&(8u>>k))p[k]=vu1lib::f2bits(f[k]);
}
inline bool Run(QwPtr m,vu1lib::u32 top,vu1lib::u32 stale){
    using namespace vu1lib;
    const u32 count=m.W(top+1,0)&0xffff;if(!count||(count&3))return false;
    for(u32 v=0;v<count;v+=4){
        const u32 a=top+3+v;V4 pos,st,wq;
        for(int k=0;k<4;++k){pos.b[k]=m.W(a,k);st.b[k]=m.W(a+1,k);wq.b[k]=m.W(a+2,k);}
        const u32 cnt=wq.b[3]&3;
        const auto bone=[&](u32 idx,int row){V4 t;for(int k=0;k<4;++k)t.b[k]=m.W((idx&0x1ff)+13+row,k);return Load(t);};
        const float p[3]={CF(bits2f(pos.b[0])),CF(bits2f(pos.b[1])),CF(bits2f(pos.b[2]))};
        const float wx=CF(bits2f(wq.b[0]));
        const Lanes c0=bone(wq.b[0],0),c1=bone(wq.b[0],1),c2=bone(wq.b[0],2);
        Lanes po=Mul(bone(wq.b[0],3),wx);
        po=Madd(c0,CF(p[0]*wx),po);po=Madd(c1,CF(p[1]*wx),po);po=Madd(c2,CF(p[2]*wx),po);
        Lanes n=Mul(c0,CF(bits2f(pos.b[3])));
        n=Madd(c1,CF(bits2f(st.b[2])),n);n=Madd(c2,CF(bits2f(st.b[3])),n);
        const int used=cnt==1?1:cnt==2?2:3;
        for(int b=1;b<used;++b){const float w=CF(bits2f(wq.b[b]));
            po=Mul(po,1.f);po=Madd(bone(wq.b[b],3),w,po);
            for(int r=0;r<3;++r)po=Madd(bone(wq.b[b],r),CF(p[r]*w),po);
        }
        float pf[4],nf[4];Store(po,pf);Store(n,nf);
        for(int k=0;k<3;++k){m.SetW(a,k,f2bits(pf[k]));m.SetW(a+2,k,f2bits(nf[k]));}
        m.SetW(a,3,stale);m.SetW(a+1,0,st.b[0]);m.SetW(a+1,1,st.b[1]);m.SetW(a+1,2,0x3f800000);m.SetW(a+1,3,st.b[3]);m.SetW(a+2,3,wq.b[3]);
    }return true;
}
}
