#include "record_lane.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "engine/ee/cpu/cpu_state.h"
#include "engine/vu/hle/vu1_hle_contract.h"
#include "games/slus_20909_crash_twinsanity/hle/math/decal_math.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/zero_kick.h"
#include "graphics_internal.h"
#include "ps2e_seam.h"
#include "te_gfx_layout.h"
#include "te_host.h"

namespace {
using te::gfx::Draw;
using te::gfx::MaterialView;

constexpr uint32_t kBlockCount = 32, kBlockDecals = 32, kKeyCount = 4, kMostTypes = 16, kMostVariants = 15;
constexpr uint32_t kBlockBytes = 0x6F0, kTypeBytes = 0x890, kVariantBytes = 0x80, kUvPacketBytes = 0x410;

constexpr uint32_t kBlockDrawNext = 0x0C, kBlockPlacesUnpack = 0x230, kBlockPlaces = 0x240, kBlockSizesUnpack = 0x440,
                   kBlockSizes = 0x450, kBlockColoursUnpack = 0x650, kBlockColours = 0x660, kBlockStart = 0x6E0;

constexpr uint32_t kDataPage = 0xDE00, kDataUvPacket = 0xDE20, kDataTypeCount = 0xE230, kDataDrawLists = 0xE304,
                   kDataChunk = 0xE314, kDataCount = 0xE318, kDataBytes = 0xE320;
constexpr uint32_t kPageMaterials = 0x0C;

constexpr uint32_t kTypeVariants = 0x10, kTypeVariantCount = 0x790;

constexpr uint32_t kRenderViewPointer = 0x00309BE0, kViewToScreen = 0xC0;

constexpr uint32_t kViewPlanes = 0x003B4900, kFarViewPlanes = 0x003B4960;
constexpr int kNearPlane = 0, kFirstSidePlane = 1, kFarPlane = 5;

constexpr uint32_t kVuMatrixAddress = 0x4F, kVuHeadAddress = 0x53;
constexpr uint32_t kVu0VariantsAddress = 0x10, kVariantQuadwords = kVariantBytes / 16;
constexpr uint32_t kSameChunkProgram = title::DecalMath::kSameChunk, kOtherChunkProgram = title::DecalMath::kOtherChunk;

constexpr uint32_t kPlacesUnpack = 0x6C208000, kSizesUnpack = 0x6D408020, kColoursUnpack = 0x6E208060, kStartProgram = 0x17000000;
constexpr uint64_t kUvGifTag = 0xC002400000008008ull;
constexpr uint32_t kUvRegisters = 0x12512512, kUvMoreRegisters = 0x5125;

constexpr uint32_t kNode = 0x20, kTop = 0x100, kPacketBase = 0x180, kPacketStride = 0x68;
constexpr uint32_t kEmitterPc = 0x1720, kClassATpc = 0x1750;
constexpr uint32_t kQwCount = 1024;

template <typename T>
const T* Guest(uint32_t address) { return reinterpret_cast<const T*>(te::GuestPointer(address, sizeof(T))); }
uint32_t ReadU32(uint32_t address) { const uint32_t* p = Guest<uint32_t>(address); return p ? *p : 0; }

struct DecalVu0 {
    std::array<title::DecalMath::Vector, title::DecalMath::kDataQuadwords> data{};
    uint32_t program = kSameChunkProgram;
    bool viewLoaded = false;
    void Write(uint32_t byteAddress, const void* bytes, size_t size) {
        if (byteAddress + size <= sizeof(data)) std::memcpy(reinterpret_cast<uint8_t*>(data.data()) + byteAddress, bytes, size);
    }
} g_vu0;

void TransformPlane(const float plane[4], const float (*m)[4], float out[4]) {
    for (int c = 0; c < 3; ++c) out[c] = plane[0] * m[0][c] + plane[1] * m[1][c] + plane[2] * m[2][c];
    out[3] = plane[3] - (out[0] * m[3][0] + out[1] * m[3][1] + out[2] * m[3][2]);
}

void Transpose(float q[4][4]) {
    for (int i = 0; i < 4; ++i)
        for (int j = i + 1; j < 4; ++j) std::swap(q[i][j], q[j][i]);
}

struct Image {
    std::vector<uint8_t> memory;
    Image() : memory(kQwCount * 16) {}
    void Set(uint32_t qw, unsigned lane, uint32_t value) { std::memcpy(&memory[qw * 16 + lane * 4], &value, 4); }
    void SetQw(uint32_t qw, const void* bytes16) { std::memcpy(&memory[qw * 16], bytes16, 16); }
};

void UnpackBlock(Image& img, const uint8_t* block) {
    for (uint32_t i = 0; i < kBlockDecals; ++i) img.SetQw(kTop + i, block + kBlockPlaces + i * 16);
    for (uint32_t i = 0; i < kBlockDecals * 2; ++i) {
        int16_t h[4];
        std::memcpy(h, block + kBlockSizes + i * 8, 8);
        for (unsigned k = 0; k < 4; ++k) img.Set(kTop + 32 + i, k, uint32_t(int32_t(h[k])));
    }
    for (uint32_t i = 0; i < kBlockDecals; ++i) {
        int8_t b[4];
        std::memcpy(b, block + kBlockColours + i * 4, 4);
        for (unsigned k = 0; k < 4; ++k) img.Set(kTop + 96 + i, k, uint32_t(int32_t(b[k])));
    }
}

bool RunBlock(const uint8_t* block, const uint8_t* uvPacket, uint32_t uvQuadwords, const float (*matrix)[4], std::vector<std::vector<uint8_t>>& kicks) {

    uint64_t uvTag;
    uint32_t uvRegs, uvMore;
    std::memcpy(&uvTag, uvPacket, 8);
    std::memcpy(&uvRegs, uvPacket + 8, 4);
    std::memcpy(&uvMore, uvPacket + 12, 4);
    const bool canonicalUv = uvTag == kUvGifTag && uvRegs == kUvRegisters && uvMore == kUvMoreRegisters;
    if (canonicalUv) {
        bool empty = true;
        for (uint32_t i = 0; i < kBlockDecals; ++i) {
            uint16_t flag;
            std::memcpy(&flag, block + kBlockSizes + i * 8 + 6, 2);
            if (flag == 0) { empty = false; break; }
        }
        if (empty) { kicks.clear(); return true; }
    }
    Image img;
    for (uint32_t r = 0; r < 4; ++r) img.SetQw(kVuMatrixAddress + r, matrix[r]);
    for (uint32_t q = 0; q < uvQuadwords; ++q) img.SetQw(kVuHeadAddress + q, uvPacket + q * 16);
    UnpackBlock(img, block);

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
    if (!ZeroKick::RunA(in, stub) || !stub.has_regs || stub.regs.TPC != kClassATpc) return false;
    for (const auto& w : stub.data_writes)
        if (w.addr + w.bytes.size() <= img.memory.size()) std::memcpy(&img.memory[w.addr], w.bytes.data(), w.bytes.size());
    VUregs entry = stub.regs;
    in.regs = &entry;
    in.start_pc = kClassATpc;
    Vu1HleOutput run;
    if (!ZeroKick::RunAContForDraw(in, run)) return false;
    kicks = std::move(run.kicks);
    return true;
}
}

extern "C" {

void ps2e_graphics_init_decal_block(ps2e_u32 blockAddress) {
    te::gfx::DrainRecordLane();
    uint8_t* block = te::GuestPointer(blockAddress, kBlockBytes);
    if (!block) return;
    auto word = [&](uint32_t at, uint32_t value) { std::memcpy(block + at, &value, 4); };
    word(kBlockStart + 12, kStartProgram);
    word(kBlockPlacesUnpack + 12, kPlacesUnpack);
    word(kBlockSizesUnpack + 12, kSizesUnpack);
    word(kBlockColoursUnpack + 12, kColoursUnpack);
}

void ps2e_graphics_init_decal_uv_packet(ps2e_u32 packetAddress) {
    te::gfx::DrainRecordLane();
    uint8_t* packet = te::GuestPointer(packetAddress, kUvPacketBytes);
    if (!packet) return;
    std::memcpy(packet, &kUvGifTag, 8);
    std::memcpy(packet + 8, &kUvRegisters, 4);
    std::memcpy(packet + 12, &kUvMoreRegisters, 4);
}

void ps2e_graphics_load_decal_view(ps2e_u32 chunkMatrices, ps2e_u32 fromChunk) {
    te::gfx::DrainRecordLane();
    const auto* chunk = reinterpret_cast<const float(*)[4]>(te::GuestPointer(chunkMatrices, 64));
    const float* far = reinterpret_cast<const float*>(te::GuestPointer(kFarViewPlanes, 6 * 16));
    const float* view = reinterpret_cast<const float*>(te::GuestPointer(kViewPlanes, 6 * 16));
    if (!chunk || !far || !view) return;
    float rows[4][4], copies[4][4];
    for (int plane = kFirstSidePlane; plane < kFarPlane; ++plane) TransformPlane(far + plane * 4, chunk, rows[plane - kFirstSidePlane]);
    float near[4];
    TransformPlane(view + kNearPlane * 4, chunk, near);
    for (auto& copy : copies) std::memcpy(copy, near, 16);
    Transpose(rows);
    Transpose(copies);
    g_vu0.Write(0, rows, 64);
    g_vu0.Write(64, copies, 64);
    g_vu0.program = kSameChunkProgram;
    if (fromChunk) {
        if (const uint8_t* from = te::GuestPointer(fromChunk, 64)) {
            g_vu0.Write(128, from, 64);
            g_vu0.program = kOtherChunkProgram;
        }
    }
    g_vu0.viewLoaded = true;
}

void ps2e_graphics_load_decal_type(ps2e_u32 typeAddress) {
    te::gfx::DrainRecordLane();
    const uint8_t* type = te::GuestPointer(typeAddress, kTypeBytes);
    if (!type) return;
    int32_t count;
    std::memcpy(&count, type + kTypeVariantCount, 4);
    if (count <= 0) return;
    const uint32_t variants = std::min<uint32_t>(uint32_t(count), kMostVariants);
    g_vu0.Write(kVu0VariantsAddress * 16, type + kTypeVariants, size_t(variants) * kVariantBytes);
}

void ps2e_graphics_age_decal(ps2e_u32 placeAddress, ps2e_u32 frameAddress, ps2e_s32 variant, ps2e_u32 lookAddress) {
    te::gfx::DrainRecordLane();
    const float* place = reinterpret_cast<const float*>(te::GuestPointer(placeAddress, 16));
    const int32_t* frame = reinterpret_cast<const int32_t*>(te::GuestPointer(frameAddress, 32));
    uint8_t* look = te::GuestPointer(lookAddress, 32);
    if (!place || !frame || !look) return;
    if (!g_vu0.viewLoaded) {

        std::memcpy(look, place, 16);
        std::memset(look + 16, 0x80, 4);
        std::memset(look + 20, 0, 12);
        return;
    }
    title::DecalMath::Decal decal;
    std::memcpy(decal.place.data(), place, 16);
    for (int k = 0; k < 4; ++k) {
        decal.frame[k] = frame[k];
        decal.more[k] = frame[4 + k];
    }
    decal.variant = uint16_t(uint32_t(std::max(variant, 0)) * kVariantQuadwords + kVu0VariantsAddress);
    const title::DecalMath::Look aged = title::DecalMath::Age(g_vu0.data.data(), g_vu0.program, decal);

    for (int k = 0; k < 4; ++k) {
        std::memcpy(look + k * 4, &aged.place[k], 4);
        look[16 + k] = uint8_t(aged.colour[k]);
        const int16_t size = int16_t(aged.offsets[k] & 0xFFFF), more = int16_t(aged.more[k] & 0xFFFF);
        std::memcpy(look + 20 + k * 2, &size, 2);
        std::memcpy(look + 28 + k * 2, &more, 2);
    }
}

void ps2e_graphics_draw_decals(ps2e_u32 decalsAddress) {
    te::gfx::DrainRecordLane();
    uint8_t* decals = te::GuestPointer(decalsAddress, kDataBytes);
    if (!decals) return;
    int32_t count;
    std::memcpy(&count, decals + kDataCount, 4);
    if (count == 0) {
        std::memset(decals + kDataChunk, 0, 4);
        return;
    }
    const uint32_t viewAddress = ReadU32(kRenderViewPointer);
    const auto* toScreen = viewAddress ? reinterpret_cast<const float(*)[4]>(te::GuestPointer(viewAddress + kViewToScreen, 64)) : nullptr;
    if (!toScreen) return;
    uint32_t typeCount;
    std::memcpy(&typeCount, decals + kDataTypeCount, 4);
    const uint32_t uvQuadwords = std::min<uint32_t>((std::min<uint32_t>(typeCount, kMostTypes) + 1) * 2 + 1, kUvPacketBytes / 16);
    const uint8_t* uvPacket = decals + kDataUvPacket;
    const float offX = te::gfx::GsOriginX(), offY = te::gfx::GsOriginY();
    static std::vector<std::vector<uint8_t>> kicks;
    static std::vector<ps2e_draw_vertex> verts;
    const bool device = te::gfx::EnsureDevice();
    for (uint32_t key = 0; key < kKeyCount; ++key) {
        uint32_t block;
        std::memcpy(&block, decals + kDataDrawLists + key * 4, 4);
        std::memset(decals + kDataDrawLists + key * 4, 0, 4);
        if (!block) continue;
        const uint32_t materialAddress = ReadU32(decalsAddress + kDataPage + kPageMaterials + key * 4);
        if (!materialAddress || !device) continue;
        const MaterialView m = te::gfx::ReadMaterial(materialAddress);
        ps2e_draw_state state = te::gfx::StateOf(m);
        const uint32_t bucket = m.material ? m.bucket : TE_GFX_BUCKET_UI;
        if (te::gfx::SceneSkipNow() && bucket < TE_GFX_BUCKET_UI) continue;
        const bool textured = state.texture != 0 && m.texture;
        const float tw = textured ? float(1 << m.texture->texture.widthPower) : 1.f;
        const float th = textured ? float(1 << m.texture->texture.heightPower) : 1.f;
        for (int guard = 0; guard < int(kBlockCount) && block; ++guard) {
            const uint8_t* bytes = te::GuestPointer(block, kBlockBytes);
            if (!bytes) break;
            kicks.clear();
            if (RunBlock(bytes, uvPacket, uvQuadwords, toScreen, kicks)) {
                verts.clear();
                for (const auto& packet : kicks) te::gfx::LowerPackedPacket(packet, offX, offY, tw, th, textured, verts);
                if (!verts.empty()) {

                    Draw& d = te::gfx::OpenDraw(bucket, state, false, te::gfx::FiniteQ(verts.data(), verts.size()));
                    d.verts.insert(d.verts.end(), verts.begin(), verts.end());
                    te::gfx::CloseDraw(bucket);
                }
            }
            std::memcpy(&block, bytes + kBlockDrawNext, 4);
        }
    }
}

}
