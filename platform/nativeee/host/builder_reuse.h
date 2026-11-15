#pragma once

#include "graphics_internal.h"
#include "exact_fp.h"
#include "engine/platform/host/platform.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cfenv>

namespace te::r3 {
enum Kind { Rigid, Skin, Blend, Shadow, Sky, Particle, Env, Kinds };
struct Key {
    std::vector<uint8_t> bytes;
    void AddBytes(const void* p, size_t n) { if (!n) return; const auto* b=static_cast<const uint8_t*>(p); bytes.insert(bytes.end(),b,b+n); }
    template<class T> void Add(const T& v) { static_assert(std::is_trivially_copyable_v<T>); AddBytes(&v,sizeof(v)); }
    template<class T> void List(const std::vector<T>& v) { Add(v.size()); AddBytes(v.data(),v.size()*sizeof(T)); }
};
struct Payload {
    std::vector<ps2e_draw_vertex> verts;
    std::vector<uint32_t> segments;
    std::vector<uint8_t> alternate;
    void Save(const gfx::DeferredBuild& b) { verts=b.verts;segments=b.segments;alternate=b.alternate; }
    void Load(gfx::DeferredBuild& b) const { b.verts=verts;b.segments=segments;b.alternate=alternate; }
    size_t Bytes() const {return verts.size()*sizeof(ps2e_draw_vertex)+segments.size()*4+alternate.size();}
};
inline std::atomic<size_t> retained{0};
constexpr size_t Budget=64*1024*1024, EntryLimit=1024*1024;
struct Memo {
    Key key;
    Payload payload;
    size_t charge=0;
    Kind kind=Rigid;
    bool valid=false;
    uint32_t misses=0, probe_attempts=0;
    ~Memo(){retained.fetch_sub(charge,std::memory_order_relaxed);}
    void Clear(){valid=false;std::vector<uint8_t>().swap(key.bytes);payload=Payload{};retained.fetch_sub(charge,std::memory_order_relaxed);charge=0;}
    void Store(Key&& k,Kind t,const gfx::DeferredBuild& b) {
        if(++misses>=4)Clear();
        const size_t need=k.bytes.size()+b.verts.size()*sizeof(ps2e_draw_vertex)+b.segments.size()*4+b.alternate.size();
        if(need>EntryLimit){Clear();return;}
        if(need>charge){const size_t extra=need-charge;size_t total=retained.load(std::memory_order_relaxed);
            do {if(total+extra>Budget){Clear();return;}}while(!retained.compare_exchange_weak(total,total+extra,std::memory_order_relaxed));
            charge+=extra;
        }
        key=std::move(k);payload.Save(b);kind=t;valid=true;
    }
};
class Probe {
    gfx::DeferredBuild& build;Kind kind;Key key;Memo* memo=nullptr;
    bool hit=false;
public:
    Probe(gfx::DeferredBuild& b,Kind k):build(b),kind(k) {}
    bool Active()const{
        if(!build.r3_memo)return true;
        auto* m=static_cast<Memo*>(build.r3_memo.get());
        return m->misses<4 || (++m->probe_attempts & 15u)==0;
    }
    bool HasHit() const {return hit;}

    bool TypedHit(){
        hit=true;
        return true;
    }
    bool Match(Key&& k){
        key=std::move(k);key.Add(int(std::fegetround()));const auto fp=te::ExactFp::Read();key.Add(fp.bytes);
        if(!fp.valid)return false;
        if(!build.r3_memo){
            auto fresh=std::make_shared<Memo>();
            fresh->probe_attempts=uint32_t(build.r3_ordinal & 15u);
            build.r3_memo=std::move(fresh);
        }
        memo=static_cast<Memo*>(build.r3_memo.get());
        hit=memo->valid&&memo->kind==kind&&memo->key.bytes==key.bytes;
        if(!hit)return false;
        memo->misses=0;memo->probe_attempts=uint32_t(build.r3_ordinal & 15u);
        memo->payload.Load(build);return true;
    }
    ~Probe(){
        if(memo&&!hit)memo->Store(std::move(key),kind,build);
    }
};
}
