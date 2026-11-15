#pragma once
#include <array>
#include <cfenv>
#include <cstdlib>
#include <cstring>
#include "exact_fp.h"
#include "games/slus_20909_crash_twinsanity/hle/math/culling_math.h"
namespace te::cullincremental {
using C = title::CullingMath;
inline uint64_t FpMode() {
    uint64_t fp;asm volatile("mrs %0, fpcr" : "=r"(fp));return fp;
}

template<class T> struct Contexts {
    struct Entry { T value{}; uint64_t hash=0,id=0; };
    std::array<Entry,256> entries{};
    std::array<uint16_t,256> hints{};
    std::array<uint8_t,64> victims{};
    uint64_t serial=0,current=0;
    void Load(uint32_t a,uint32_t b,const T& value) {
        const size_t hint=((a>>4)^(b>>7)^(a>>13))&255;
        auto& direct=entries[hints[hint]];
        if(direct.id && !std::memcmp(&value,&direct.value,sizeof value)) {
            current=direct.id;return;
        }

        uint64_t hash=14695981039346656037ull;
        static_assert(sizeof(T)%8==0,"whole words");
        for(size_t at=0;at<sizeof(T);at+=8) {
            uint64_t word;std::memcpy(&word,reinterpret_cast<const uint8_t*>(&value)+at,8);
            hash=(hash^word)*1099511628211ull;
        }

        const size_t bucket=(hash^(hash>>32)^(hash>>16))&63;
        const size_t first=bucket*4;
        size_t destination=256;
        for(size_t k=0;k<4;++k) {
            auto& e=entries[first+k];
            if(e.id && e.hash==hash && !std::memcmp(&value,&e.value,sizeof value)) {
                current=e.id;hints[hint]=uint16_t(first+k);return;
            }
            if(!e.id)destination=first+k;
        }
        if(destination==256)destination=first+(victims[bucket]++&3);
        auto& e=entries[destination];
        if(++serial==0)std::abort();
        e.value=value;e.hash=hash;e.id=serial;
        current=e.id;hints[hint]=uint16_t(destination);
    }
};
inline Contexts<C::Frustum>& Frusta() {static Contexts<C::Frustum> p;return p;}
inline Contexts<C::Link>& Links() {static Contexts<C::Link> p;return p;}
inline void Frustum(uint32_t a,uint32_t b,const C::Frustum& v) {Frusta().Load(a,b,v);}
inline void Link(uint32_t a,uint32_t b,const C::Link& v) {Links().Load(a,b,v);}
template<class Output> struct Cache {
    struct Entry {
        uint64_t frustum=0,link=0,fp=0;
        C::Matrix model{};
        C::Box box{};
        Output output{};
        bool valid=false;
    };
    std::array<Entry,4096> entries{};
    template<class Compute> Output Get(uint32_t a,uint32_t b,uint64_t frustum,uint64_t link,
                                      const C::Matrix* model,const C::Box* box,Compute compute) {
        auto& e=entries[((a>>4)^(a>>15)^(b>>6))&4095];
        const uint64_t fp=FpMode();
        if(e.valid && e.frustum==frustum && e.link==link && e.fp==fp &&
           (!model||!std::memcmp(model,&e.model,sizeof *model)) &&
           (!box||!std::memcmp(box,&e.box,sizeof *box))) {
            return e.output;
        }
        e.output=compute();e.frustum=frustum;e.link=link;e.fp=fp;
        if(model)e.model=*model;
        if(box)e.box=*box;
        e.valid=true;return e.output;
    }
};
inline C::Model LoadModel(uint32_t a,const C::Link& link,const C::Matrix& model) {
    static Cache<C::Model> c;
    return c.Get(a,0,0,Links().current,&model,nullptr,[&]{return C::LoadModel(link,model);});
}
inline C::Classification TestBox(uint32_t a,const C::Frustum& frustum,const C::Box& box) {
    static Cache<C::Classification> c;
    return c.Get(a,0,Frusta().current,0,nullptr,&box,[&]{return C::TestBox(frustum,box);});
}
inline C::BoxView TestBoxAt(uint32_t a,uint32_t b,const C::Frustum& frustum,const C::Link& link,const C::Box& box,const C::Matrix& model) {
    static Cache<C::BoxView> c;
    return c.Get(a,b,Frusta().current,Links().current,&model,&box,[&]{return C::TestBoxAt(frustum,link,box,model);});
}
}
