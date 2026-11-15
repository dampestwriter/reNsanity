namespace behind_skin {
using Vec = float __attribute__((vector_size(16)));
inline Vec Load(const Qw& q, bool xyz=false) {
    using namespace vu1lib;
    return Vec{CF(bits2f(q[0])),CF(bits2f(q[1])),CF(bits2f(q[2])),xyz?0.f:CF(bits2f(q[3]))};
}
inline Vec Madd(Vec a,float b,Vec c) {
    return (Vec)vfmaq_n_f32((float32x4_t)c,(float32x4_t)a,b);
}
struct Output {
    std::vector<Vertex> verts;
    std::vector<size_t> ends;
    void Clear(){verts.clear();ends.clear();}
};
enum class Attempt { Unsupported, Dirty, Clean };
Attempt Run(const SkinGeometry::Part& part,const SkinGeometry::Inputs& in,const BatchProgram& program,
         const Draw& state,Output& out) {
    using namespace vu1lib;
    if (!state.tex_enabled || state.use_fst || state.tex.fst || state.ctx || state.fog || state.prim_type!=4 ||
        state.raster.frame.psm || state.raster.zbuf.psm || in.joints.empty() || (in.joints.size()&3) ||
        in.joints.size()/4>SkinGeometry::kMaxJoints || part.batches.empty()) return Attempt::Unsupported;

    float displaced[SkinGeometry::kMaxBatchVertices][4];
    uint32_t displacedCount=0;
    if(program.blend && !program.blend->shapes.empty() &&
       !SkinGeometry::BlendPositions(part,*program.blend,displaced,&displacedCount)) return Attempt::Unsupported;
    const auto f=[](uint32_t b){return CF(bits2f(b));};
    Vec screen[4],camera[4],normal[3],colour[4];
    for(int r=0;r<4;++r){screen[r]=Load(in.to_screen[r]);camera[r]=Load(in.to_camera[r]);}
    for(int r=0;r<3;++r){
        normal[r]=Vec{f(in.lights[0][r]),f(in.lights[1][r]),f(in.lights[2][r]),0};
        colour[r]=Load(in.colours[r],true);
    }
    colour[3]=Load(in.ambient,true);
    const Vec guard=Load(in.clip);
    const Vec scroll={CF(in.scroll[0]),CF(in.scroll[1]),0,0};

    std::array<Vec,SkinGeometry::kMaxJoints*4> joints;
    for(size_t r=0;r<in.joints.size();++r)joints[r]=Load(in.joints[r],true);
    out.Clear();
    vuexact::Scope certificate;
    for(const auto& batch:part.batches){
        const size_t n=batch.Vertices();
        if(n<3 || n>SkinGeometry::kMaxBatchVertices || batch.vertices.size()!=4*n || (batch.count[0]&0xffff)!=n*4 ||
           (displacedCount && displacedCount!=n))return Attempt::Unsupported;
        const float sx=f(batch.scale[0]),sy=f(batch.scale[1]);
        std::array<Vertex,SkinGeometry::kMaxBatchVertices*3> assembled;
        StripKicks strip(assembled.data());
        struct Pose { Vec p,st,position;bool draw=false,needed=false; };
        std::array<Pose,SkinGeometry::kMaxBatchVertices> poses{};
        unsigned h1=0,h2=0;
        for(size_t j=0;j<n;++j){
            const Qw &q0=batch.vertices[4*j],&q1=batch.vertices[4*j+1],&wq=batch.vertices[4*j+2];
            Vec p,st;
            for(int k=0;k<4;++k){p[k]=float(int32_t(q0[k]))*sx;st[k]=float(int32_t(q1[k]))*sy;}
            if(displacedCount)for(int k=0;k<3;++k)p[k]=CF(displaced[j][k]);
            const int used=(wq[3]&3)==1?1:(wq[3]&3)==2?2:3;
            Vec po{};
            for(int b=0;b<used;++b){
                const size_t at=wq[b]&0x1ff;

                if(at+3>=in.joints.size())return Attempt::Unsupported;
                const float w=f(wq[b]);
                const Vec pw=p*w;
                if(b==0){
                    po=joints[at+3]*w;po=Madd(joints[at],pw[0],po);
                }else if(program.blend){
                    po=Madd(joints[at+3],w,po);po=Madd(joints[at],pw[0],po);
                }else if(b==1){
                    Vec tmp=joints[at+3]*w;tmp=Madd(joints[at],pw[0],tmp);po=Madd(po,1.f,tmp);
                }else{
                    po=Madd(joints[at+3],w,po);po=Madd(joints[at],pw[0],po);
                }
                po=Madd(joints[at+1],pw[1],po);po=Madd(joints[at+2],pw[2],po);
            }
            Vec clip=camera[3];clip=Madd(camera[0],po[0],clip);clip=Madd(camera[1],po[1],clip);clip=Madd(camera[2],po[2],clip);
            clip=clip*guard;
            const unsigned cur=ClipNf(f2bits(clip[0]),f2bits(clip[1]),f2bits(clip[2]),f2bits(clip[3]))!=0;
            const bool adc=(wq[3]&0x8000) || (cur|h1|h2);
            h2=h1;h1=cur;
            poses[j].p=p;poses[j].st=st;poses[j].position=po;poses[j].draw=!adc;
            if(j>=2 && !adc)poses[j-2].needed=poses[j-1].needed=poses[j].needed=true;
        }

        for(size_t j=0;j<n;++j){
            Vertex v{};
            v.draw_kick=poses[j].draw;
            if(poses[j].needed){
                const Vec p=poses[j].p,st=poses[j].st,po=poses[j].position;
                const Qw &wq=batch.vertices[4*j+2],&col=batch.vertices[4*j+3];
                const size_t at=wq[0]&0x1ff;
                Vec nm=joints[at]*p[3];nm=Madd(joints[at+1],st[2],nm);nm=Madd(joints[at+2],st[3],nm);
                if(!program.blend)nm=nm+Vec{0,0,0,0};
                Vec t=screen[3];t=Madd(screen[1],po[1],t);t=Madd(screen[2],po[2],t);t=Madd(screen[0],po[0],t);
                const float reciprocal=1.f/t[3];
                Vec wn=normal[0]*nm[0];wn=Madd(normal[1],nm[1],wn);wn=Madd(normal[2],nm[2],wn);
                Vec lit=colour[3];
                for(int k=0;k<3;++k)lit=Madd(colour[k],bits2f(Fmax_raw(f2bits(wn[k]),0u)),lit);
                const Vec tex=(Vec{st[0],st[1],1,0}+scroll)*reciprocal;
                const Vec xyz=t*reciprocal;
                v.s=tex[0];v.t=tex[1];v.q=tex[2];
                v.x=float(uint16_t(Ftoi0(f2bits(xyz[0]))))/16.f-state.off_x;
                v.y=float(uint16_t(Ftoi0(f2bits(xyz[1]))))/16.f-state.off_y;
                v.z=float(Ftoi0(f2bits(xyz[2])));
                uint8_t rgba[4];
                for(int k=0;k<4;++k){
                    const float c=float(int32_t(col[k]));
                    const float mod=k==3?c:lit[k]*c;
                    rgba[k]=uint8_t(Ftoi0(Fmin_raw(f2bits(mod),f2bits(255.f))));
                }
                v.r=rgba[0];v.g=rgba[1];v.b=rgba[2];v.a=rgba[3];
            }
            strip.Kick(v,poses[j].draw);
        }
        if(out.verts.size()+strip.count>SkinGeometry::kMaxVertices*24)return Attempt::Unsupported;
        if(strip.count){out.verts.insert(out.verts.end(),assembled.data(),assembled.data()+strip.count);out.ends.push_back(out.verts.size());}
    }
    return certificate.Clean()?Attempt::Clean:Attempt::Dirty;
}
inline bool Same(const Vertex& a,const Vertex& b){
    return !std::memcmp(&a.s,&b.s,sizeof(float))&&!std::memcmp(&a.t,&b.t,sizeof(float))&&
        !std::memcmp(&a.q,&b.q,sizeof(float))&&!std::memcmp(&a.x,&b.x,sizeof(float))&&
        !std::memcmp(&a.y,&b.y,sizeof(float))&&!std::memcmp(&a.z,&b.z,sizeof(float))&&
        a.r==b.r&&a.g==b.g&&a.b==b.b&&a.a==b.a&&a.draw_kick==b.draw_kick;
}
}
