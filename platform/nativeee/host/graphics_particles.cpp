#include "record_lane.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "engine/ee/cpu/cpu_state.h"
#include "engine/vu/hle/vu1_hle_contract.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/zero_kick.h"
#include "graphics_internal.h"
#include "builder_reuse.h"
#include "ps2e_seam.h"
#include "te_gfx_layout.h"
#include "te_host.h"

namespace {
using te::gfx::Draw;
using te::gfx::MaterialView;

constexpr uint32_t kVuMatrixAddress = 0x4F, kVuHeadAddress = 0x53, kVuStepsAddress = 0x55;
constexpr uint32_t kParticleBlockBytes = 0x430, kHexagonBlockBytes = 0x1B0, kRenderTableBytes = 0x890;
constexpr uint32_t kRenderSteps = 64;
constexpr uint32_t kParticlesPerBlock = 32;
constexpr float kParticleNearClip = 0.5f;

constexpr uint32_t kTagId = 0x70000000, kCountTag = 0x10000000, kNextTag = 0x20000000, kReturnTag = 0x60000000;
constexpr uint32_t kVifFlush = 0x11000000, kVifFlushE = 0x10000000, kVifMscnt = 0x17000000;
constexpr uint32_t kVifUnpackV4Count = 0x6C000000, kVifTops = 0x8000;
constexpr uint32_t VifUnpackTo(uint32_t code, uint32_t address, uint32_t count) { return code | (count << 16) | address; }

constexpr uint32_t kGifRgbaq = 1, kGifSt = 2, kGifUv = 3, kGifXyz2 = 5;
constexpr uint32_t kGsPrimTriStrip = 4;

constexpr uint32_t kNode = 0x20, kTop = 0x100, kPacketBase = 0x180, kPacketStride = 0x50;
constexpr uint32_t kEmitterPc = 0x1720, kClassCTpc = 0x1820;
constexpr uint32_t kQwCount = 1024;

struct GuestLook {
    float gravity;
    int32_t texture[4];
    uint8_t hexagons, pad[3];
    float hexagonCorners[12];
    struct Step { float shape[6]; uint8_t colour[4]; } steps[kRenderSteps];
};
static_assert(sizeof(GuestLook) == 0x18 + 0x30 + 64 * 0x1C, "ParticleLook layout");

struct GuestTable {
    uint32_t tag[4];
    float nearClip, gravity, unused18, time;
    float distortion[4];
    uint32_t stepsTag[4];
    int32_t corners[12];
    struct Step { float shape[6]; uint32_t unused18[2]; } steps[kRenderSteps];
    uint32_t end[4];
};
static_assert(offsetof(GuestTable, steps) == 0x70 && offsetof(GuestTable, end) == 0x870, "ParticleRenderTable layout");
static_assert(sizeof(GuestTable) <= kRenderTableBytes, "ParticleRenderTable fits");

template <typename T>
T* GuestMutable(uint32_t address) { return reinterpret_cast<T*>(te::GuestPointer(address, sizeof(T))); }
template <typename T>
const T* Guest(uint32_t address) { return reinterpret_cast<const T*>(te::GuestPointer(address, sizeof(T))); }

uint64_t GifDescriptors(std::initializer_list<uint32_t> regs) {
    uint64_t value = 0;
    unsigned shift = 0;
    for (uint32_t r : regs) { value |= uint64_t(r & 0xF) << shift; shift += 4; }
    return value;
}

}

namespace {
struct Vertex { float x, y, z, u, v; uint8_t r, g, b, a; bool kick; };

struct StripKicks {
    Vertex ring[3];
    unsigned held = 0, oldest = 0, parity = 0;
};
void Assemble(StripKicks& strip, const Vertex& v, std::vector<ps2e_draw_vertex>& out, float tw, float th, bool textured) {
    if (strip.held < 3) {
        strip.ring[strip.held++] = v;
    } else {
        strip.ring[strip.oldest] = v;
        strip.oldest = strip.oldest == 2 ? 0 : strip.oldest + 1;
    }
    unsigned& parity = strip.parity;
    if (!v.kick) { parity = 0; return; }
    if (strip.held != 3) return;
    const unsigned o = strip.oldest;
    const Vertex* q[3] = {&strip.ring[o], &strip.ring[o == 2 ? 0 : o + 1], &strip.ring[o == 0 ? 2 : o - 1]};
    if (parity % 2) std::swap(q[0], q[1]);
    const bool degenerate = (q[0]->x == q[1]->x && q[0]->y == q[1]->y) || (q[1]->x == q[2]->x && q[1]->y == q[2]->y) ||
                            (q[0]->x == q[2]->x && q[0]->y == q[2]->y);
    if (!degenerate) {
        for (const Vertex* s : q) {
            ps2e_draw_vertex o{};
            o.x = s->x; o.y = s->y; o.z = s->z;
            if (textured) { o.u = s->u / tw; o.v = s->v / th; }
            o.r = s->r; o.g = s->g; o.b = s->b; o.a = s->a;
            out.push_back(o);
        }
    }
    ++parity;
}

}

bool te::gfx::LowerPackedPacket(const std::vector<uint8_t>& packet, float offX, float offY, float tw, float th, bool textured, std::vector<ps2e_draw_vertex>& out) {
    if (packet.size() < 16) return false;
    auto word = [&](size_t qw, unsigned lane) { uint32_t v; std::memcpy(&v, packet.data() + qw * 16 + lane * 4, 4); return v; };
    const uint32_t w0 = word(0, 0), w1 = word(0, 1);
    const uint32_t nloop = w0 & 0x7FFF;
    const uint32_t flg = (w1 >> 26) & 3;
    uint32_t nregs = (w1 >> 28) & 0xF;
    if (!nregs) nregs = 16;
    if (flg != 0) return false;
    const uint64_t regs = uint64_t(word(0, 2)) | uint64_t(word(0, 3)) << 32;
    if (packet.size() < 16 + size_t(nloop) * nregs * 16) return false;
    StripKicks strip;
    Vertex v{};
    size_t qw = 1;
    for (uint32_t loop = 0; loop < nloop; ++loop) {
        for (uint32_t r = 0; r < nregs; ++r, ++qw) {
            switch ((regs >> (4 * r)) & 0xF) {
                case kGifRgbaq:
                    v.r = uint8_t(word(qw, 0)); v.g = uint8_t(word(qw, 1)); v.b = uint8_t(word(qw, 2)); v.a = uint8_t(word(qw, 3));
                    break;
                case kGifUv:
                    v.u = float(word(qw, 0) & 0x3FFF) / 16.f;
                    v.v = float(word(qw, 1) & 0x3FFF) / 16.f;
                    break;
                case kGifSt: {
                    float s, t, q;
                    std::memcpy(&s, &packet[qw * 16], 4); std::memcpy(&t, &packet[qw * 16 + 4], 4); std::memcpy(&q, &packet[qw * 16 + 8], 4);
                    v.u = q != 0.f ? s / q * tw : 0.f;
                    v.v = q != 0.f ? t / q * th : 0.f;
                    break;
                }
                case kGifXyz2: {
                    v.x = float(word(qw, 0) & 0xFFFF) / 16.f - offX;
                    v.y = float(word(qw, 1) & 0xFFFF) / 16.f - offY;
                    v.z = float(word(qw, 2));
                    v.kick = ((word(qw, 3) >> 15) & 1) == 0;
                    Assemble(strip, v, out, tw, th, textured);
                    break;
                }
                default: break;
            }
        }
    }
    return true;
}

namespace {

struct Image {
    std::vector<uint8_t> memory;
    Image() : memory(kQwCount * 16) {}
    void Set(uint32_t qw, unsigned lane, uint32_t value) { std::memcpy(&memory[qw * 16 + lane * 4], &value, 4); }
    void SetQw(uint32_t qw, const void* bytes16) { std::memcpy(&memory[qw * 16], bytes16, 16); }
};

bool RunBlock(const uint8_t* block, const GuestTable& table, const float (*matrix)[4], float time, std::vector<std::vector<uint8_t>>& kicks) {
    Image img;
    for (uint32_t r = 0; r < 4; ++r) img.SetQw(kVuMatrixAddress + r, matrix[r]);
    const float head[4] = {kParticleNearClip, table.gravity, table.unused18, time};
    img.SetQw(kVuHeadAddress, head);
    img.SetQw(kVuHeadAddress + 1, table.distortion);
    for (uint32_t q = 0; q < 3; ++q) img.SetQw(kVuStepsAddress + q, &table.corners[q * 4]);
    for (uint32_t s = 0; s < kRenderSteps; ++s) {
        img.SetQw(kVuStepsAddress + 3 + 2 * s, table.steps[s].shape);
        img.SetQw(kVuStepsAddress + 4 + 2 * s, &table.steps[s].shape[4]);
    }

    const uint32_t quadwords = kParticleBlockBytes / 16 - 1;
    for (uint32_t q = 0; q < quadwords - 1; ++q) img.SetQw(kTop + q, block + 16 + q * 16);

    img.Set(3, 0, kNode);
    img.Set(kNode, 0, kEmitterPc / 8);
    for (uint32_t p = 0; p < 4; ++p) img.Set(kNode + 1 + p, 0, kPacketBase + p * kPacketStride);
    VUregs regs{};
    regs.VF[0].I[3] = 0x3f800000u;
    Vu1HleInput in{};
    in.micro = nullptr;
    in.data = img.memory.data();
    in.start_pc = 0;
    in.top = in.tops = kTop;
    in.itop = in.itops = 0;
    in.regs = &regs;
    Vu1HleOutput stub;
    if (!ZeroKick::RunC(in, stub) || !stub.has_regs || stub.regs.TPC != kClassCTpc) return false;
    for (const auto& w : stub.data_writes)
        if (w.addr + w.bytes.size() <= img.memory.size()) std::memcpy(&img.memory[w.addr], w.bytes.data(), w.bytes.size());
    VUregs entry = stub.regs;
    in.regs = &entry;
    in.start_pc = kClassCTpc;
    Vu1HleOutput run;
    if (!ZeroKick::RunCContForDraw(in, run)) return false;
    kicks = std::move(run.kicks);
    return true;
}
}

extern "C" {

void ps2e_graphics_init_particle_graphics(void) {
    te::gfx::DrainRecordLane();

}

void ps2e_graphics_init_particle_block(ps2e_u32 blockAddress, ps2e_s32 hexagons) {
    te::gfx::DrainRecordLane();
    const uint32_t bytes = hexagons ? kHexagonBlockBytes : kParticleBlockBytes;
    uint8_t* block = te::GuestPointer(blockAddress, bytes);
    if (!block) return;
    const uint32_t quadwords = bytes / 16 - 1;
    auto* words = reinterpret_cast<uint32_t*>(block);
    auto* last = reinterpret_cast<uint32_t*>(block + quadwords * 16);
    for (int k = 0; k < 4; ++k) last[k] = 0;
    words[0] = kReturnTag | quadwords;
    words[1] = 0;
    words[2] = 0;
    words[3] = VifUnpackTo(kVifUnpackV4Count | kVifTops, 0, quadwords - 1);

    uint32_t* gif = words + 4;
    const uint32_t loops = hexagons ? 6 : 8, nregs = hexagons ? 13 : 9;
    gif[0] = loops | 0x8000;
    gif[1] = (1u << 14) | (kGsPrimTriStrip << 15) | (nregs << 28);
    const uint64_t regs = hexagons
        ? GifDescriptors({kGifSt, kGifRgbaq, kGifXyz2, kGifSt, kGifXyz2, kGifRgbaq, kGifSt, kGifXyz2, kGifRgbaq, kGifSt, kGifXyz2, kGifSt, kGifXyz2})
        : GifDescriptors({kGifUv, kGifRgbaq, kGifXyz2, kGifUv, kGifXyz2, kGifUv, kGifXyz2, kGifUv, kGifXyz2});
    gif[2] = uint32_t(regs);
    gif[3] = uint32_t(regs >> 32);
    if (hexagons) last[3] = kVifMscnt;
    else last[0] = kVifMscnt;
}

void ps2e_graphics_end_particle_block(ps2e_u32 blockAddress) {
    te::gfx::DrainRecordLane();
    auto* tag = GuestMutable<uint32_t[4]>(blockAddress);
    if (!tag) return;
    (*tag)[1] = 0;
    (*tag)[0] = ((*tag)[0] & ~kTagId) | kReturnTag;
}

void ps2e_graphics_chain_particle_blocks(ps2e_u32 blocksAddress, ps2e_s32 count) {
    te::gfx::DrainRecordLane();
    if (count <= 0 || count > 4096) return;
    const uint32_t* blocks = reinterpret_cast<const uint32_t*>(te::GuestPointer(blocksAddress, 4 * size_t(count)));
    if (!blocks) return;
    ps2e_graphics_end_particle_block(blocks[count - 1]);
    for (int32_t b = count - 2; b >= 0; --b) {
        auto* tag = GuestMutable<uint32_t[4]>(blocks[b]);
        if (!tag) continue;
        (*tag)[0] = ((*tag)[0] & ~kTagId) | kNextTag;
        (*tag)[1] = blocks[b + 1] & 0x7FFFFFFFu;
    }
}

void ps2e_graphics_init_particle_render_table(ps2e_u32 tableAddress) {
    te::gfx::DrainRecordLane();
    auto* t = GuestMutable<GuestTable>(tableAddress);
    if (!t) return;
    constexpr uint32_t kHeadQuadwords = 2;
    constexpr uint32_t kStepsQuadwords = (offsetof(GuestTable, end) - offsetof(GuestTable, corners)) / 16;
    t->tag[0] = kCountTag | kHeadQuadwords;
    t->tag[1] = 0;
    t->tag[2] = kVifFlush;
    t->tag[3] = VifUnpackTo(kVifUnpackV4Count, kVuHeadAddress, kHeadQuadwords);
    t->stepsTag[0] = kReturnTag | (kStepsQuadwords + 1);
    t->stepsTag[1] = 0;
    t->stepsTag[2] = kVifFlush;
    t->stepsTag[3] = VifUnpackTo(kVifUnpackV4Count, kVuStepsAddress, kStepsQuadwords);
    t->end[0] = kVifFlushE;
    t->end[1] = t->end[2] = t->end[3] = 0;
}

void ps2e_graphics_write_particle_render_table(ps2e_u32 tableAddress, ps2e_u32 lookAddress) {
    te::gfx::DrainRecordLane();
    auto* t = GuestMutable<GuestTable>(tableAddress);
    const auto* look = Guest<GuestLook>(lookAddress);
    if (!t || !look) return;
    t->gravity = look->gravity;
    int32_t* c = t->corners;
    c[0] = c[4] = look->texture[0];
    c[2] = c[6] = look->texture[2];
    c[1] = c[3] = look->texture[1];
    c[5] = c[7] = look->texture[3];
    for (uint32_t s = 0; s < kRenderSteps; ++s) {
        float* entry = t->steps[s].shape;
        for (unsigned v = 0; v < 6; ++v) entry[v] = look->steps[s].shape[v];
        auto* bytes = reinterpret_cast<uint8_t*>(entry);
        for (unsigned ch = 0; ch < 4; ++ch) bytes[ch * 4] = look->steps[s].colour[ch];
    }

    if (!look->hexagons) {
        const float zero[4] = {0.f, 0.f, 0.f, 0.f};
        std::memcpy(&t->corners[8], zero, sizeof zero);
        return;
    }
    std::memcpy(t->corners, look->hexagonCorners, 12 * sizeof(float));
}

void ps2e_graphics_draw_particle_block(ps2e_u32 blockAddress, ps2e_u32 tableAddress, ps2e_u32 matrixAddress, ps2e_u32 materialAddress, ps2e_f32 time) {
    te::gfx::DrainRecordLane();
    auto* t = GuestMutable<GuestTable>(tableAddress);
    const auto* matrix = reinterpret_cast<const float(*)[4]>(te::GuestPointer(matrixAddress, 64));
    if (!t || !matrix || !te::gfx::EnsureDevice()) return;

    t->nearClip = kParticleNearClip;
    t->time = time;
    const MaterialView m = te::gfx::ReadMaterial(materialAddress);
    ps2e_draw_state state = te::gfx::StateOf(m);
    const uint32_t bucket = m.material ? m.bucket : TE_GFX_BUCKET_UI;
    if (te::gfx::SceneSkipNow() && bucket < TE_GFX_BUCKET_UI) return;
    const bool textured = state.texture != 0 && m.texture;
    const float tw = textured ? float(1 << m.texture->texture.widthPower) : 1.f;
    const float th = textured ? float(1 << m.texture->texture.heightPower) : 1.f;
    const float offX = te::gfx::GsOriginX(), offY = te::gfx::GsOriginY();

    if (te::gfx::DeferringBuilds()) {

        std::vector<uint8_t> blocks;
        for (int guard = 0; guard < 4096 && blockAddress; ++guard) {
            const uint8_t* block = te::GuestPointer(blockAddress, kParticleBlockBytes);
            if (!block) break;
            blocks.insert(blocks.end(), block, block + kParticleBlockBytes);
            uint32_t tag, next;
            std::memcpy(&tag, block, 4);
            std::memcpy(&next, block + 4, 4);
            blockAddress = ((tag >> 28) & 7) == 2 ? next : 0;
        }
        if (blocks.empty()) return;
        std::array<float, 16> m16;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) m16[size_t(r * 4 + c)] = matrix[r][c];
        te::gfx::DeferredBuild& build = te::gfx::ReserveDeferred(bucket, state);
        te::gfx::PostBuild(build, [&build, blocks = std::move(blocks), table = *t, m16, time, offX, offY, tw, th, textured] {
            te::r3::Probe probe(build,te::r3::Particle);
            if(probe.Active()){te::r3::Key k;k.List(blocks);k.Add(table);k.Add(m16);k.Add(time);k.Add(offX);k.Add(offY);k.Add(tw);k.Add(th);k.Add(textured);
                if(probe.Match(std::move(k))) return;}
            thread_local std::vector<std::vector<uint8_t>> kicks;
            const auto* mat = reinterpret_cast<const float(*)[4]>(m16.data());
            for (size_t at = 0; at + kParticleBlockBytes <= blocks.size(); at += kParticleBlockBytes) {
                kicks.clear();
                if (!RunBlock(blocks.data() + at, table, mat, time, kicks)) break;

                const size_t base = build.verts.size();
                for (const auto& packet : kicks) te::gfx::LowerPackedPacket(packet, offX, offY, tw, th, textured, build.verts);
                const size_t n = (build.verts.size() - base) / 3 * 3;
                build.verts.resize(base + n);
                if (!n) continue;
                build.segments.push_back(uint32_t(n));
            }
        });
        return;
    }
    static std::vector<std::vector<uint8_t>> kicks;
    static std::vector<ps2e_draw_vertex> verts;
    uint32_t address = blockAddress;
    for (int guard = 0; guard < 4096 && address; ++guard) {
        const uint8_t* block = te::GuestPointer(address, kParticleBlockBytes);
        if (!block) break;
        uint32_t tag;
        std::memcpy(&tag, block, 4);
        kicks.clear();
        if (!RunBlock(block, *t, matrix, time, kicks)) break;
        verts.clear();
        for (const auto& packet : kicks) te::gfx::LowerPackedPacket(packet, offX, offY, tw, th, textured, verts);
        if (!verts.empty()) {

            Draw& d = te::gfx::OpenDraw(bucket, state, false, te::gfx::FiniteQ(verts.data(), verts.size()));
            d.verts.insert(d.verts.end(), verts.begin(), verts.end());
            te::gfx::CloseDraw(bucket);
        }
        const uint32_t id = (tag >> 28) & 7;
        uint32_t next;
        std::memcpy(&next, block + 4, 4);
        address = id == 2 ? next : 0;
    }
}

void ps2e_graphics_draw_distortion_block(ps2e_u32 block, ps2e_u32 table, ps2e_u32 matrix, ps2e_u32 material, ps2e_f32 time,
                                         ps2e_f32 distortionX, ps2e_f32 distortionY) {
    te::gfx::DrainRecordLane();
    (void)block; (void)table; (void)matrix; (void)material; (void)time; (void)distortionX; (void)distortionY;
}

}
