
#include "engine/render/backends/deko/dk_device.h"
#include "engine/render/mesh_indices.h"
#include "engine/render/backends/deko/dk_binding_plan.h"

#include <switch.h>

#include <algorithm>
#include <cmath>
#include <cerrno>
#include <limits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <stdexcept>

#include "dk_shaders_gen.h"
#include "engine/render/texture_mips.h"

namespace ps2r {
namespace render {

namespace {

constexpr uint32_t Align(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

struct DkshHeader {
    uint32_t magic, header_sz, control_sz, code_sz, programs_off, num_programs;
};
struct DkshProgramHeader {
    uint32_t type, entrypoint, num_gprs, constbuf1_off, constbuf1_sz, per_warp_scratch_sz;
};

constexpr uint32_t kGlNearest = 0x2600, kGlLinear = 0x2601, kGlNearestMipNearest = 0x2700, kGlLinearMipNearest = 0x2701,
                   kGlNearestMipLinear = 0x2702, kGlLinearMipLinear = 0x2703;

int MinIndex(uint32_t gl_min) {
    switch (gl_min) {
        case kGlLinear: return 1;
        case kGlNearestMipNearest: return 2;
        case kGlLinearMipNearest: return 3;
        case kGlNearestMipLinear: return 4;
        case kGlLinearMipLinear: return 5;
        default: return 0;
    }
}

int AnisoIndex(int aniso) { return aniso >= 16 ? 4 : aniso >= 8 ? 3 : aniso >= 4 ? 2 : aniso >= 2 ? 1 : 0; }

bool ReadbackCached() { return true; }
DkMemBlock MakeReadbackBlock(DkDevice dev, uint32_t size) {
    DkMemBlockMaker mm;
    dkMemBlockMakerDefaults(&mm, dev, size);
    mm.flags = (ReadbackCached() ? DkMemBlockFlags_CpuCached : DkMemBlockFlags_CpuUncached) | DkMemBlockFlags_GpuCached;
    DkMemBlock b = dkMemBlockCreate(&mm);
    if (b && ReadbackCached()) dkMemBlockFlushCpuCache(b, 0, size);
    return b;
}

const uint8_t* ReadbackBytes(DkMemBlock b, uint32_t bytes) {
    if (ReadbackCached()) dkMemBlockFlushCpuCache(b, 0, bytes);
    return static_cast<const uint8_t*>(dkMemBlockGetCpuAddr(b));
}

}

DekoDevice::DekoDevice() = default;
DekoDevice::~DekoDevice() { Shutdown(); }

void DekoDevice::CmdAddMem(void* user, DkCmdBuf cmdbuf, size_t min_size) {

    auto* self = static_cast<DekoDevice*>(user);
    const uint32_t size = Align(uint32_t(std::max<size_t>(min_size, 512 * 1024)), DK_MEMBLOCK_ALIGNMENT);
    DkMemBlockMaker mm;
    dkMemBlockMakerDefaults(&mm, self->dev_, size);
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    DkMemBlock b = dkMemBlockCreate(&mm);
    self->slots_[self->cur_].extra_cmd.push_back(b);
    dkCmdBufAddMemory(cmdbuf, b, 0, size);
    self->rotate_pending_ = true;
}

void DekoDevice::BeginSlot() {
    Slot& s = slots_[cur_];
    if (s.armed) {
        const DkResult result = dkFenceWait(&s.fence, -1);
        if (result != DkResult_Success) {
            std::fprintf(stderr, "[deko] slot %d fence wait failed: %d\n", cur_, int(result));
            std::abort();
        }
        s.armed = false;
    }
    ReleaseRetired(s);
    dkCmdBufClear(cmd_);
    dkCmdBufAddMemory(cmd_, cmd_block_, uint32_t(cur_) * cmd_slot_bytes_, cmd_slot_bytes_);
    stream_off_ = 0;

    uniform_ring_.BeginAfterFence();
    stream_flushed_ = uniform_flushed_ = 0;
    std::fill(std::begin(applied_.ubo_addr), std::end(applied_.ubo_addr), 0);
    std::fill(std::begin(applied_.ubo_size), std::end(applied_.ubo_size), ~0u);
    rotate_pending_ = false;
    applied_.vtx_addr = 0;
}

void DekoDevice::EndSlot() {
    Slot& s = slots_[cur_];

    if (ce_pending_) {
        dkCmdBufBarrier(cmd_, DkBarrier_Full, 0);
        gr_pending_ = false;
    }
    DrainForFence();

    dkCmdBufSignalFence(cmd_, &s.fence, false);
    s.armed = true;
    Submit(false);
}

void DekoDevice::Rotate() {
    EndSlot();
    dkQueueFlush(queue_);
    cur_ = (cur_ + 1) % slots_n_;
    BeginSlot();
}

void DekoDevice::SafePoint() {
    const bool uniform_full = !uniform_ring_.Fits(DkUniformRing::kOperationReserve, uniform_slot_bytes_);
    if (rotate_pending_ || uniform_full) Rotate();
}

void DekoDevice::Submit(bool flush) {

    if (stream_off_ & 255u) stream_off_ = std::min(Align(stream_off_, 256u), stream_slot_bytes_);

    {
        if (stream_off_ > stream_flushed_)
            dkMemBlockFlushCpuCache(stream_block_, uint32_t(cur_) * stream_slot_bytes_ + stream_flushed_, stream_off_ - stream_flushed_);
        if (uniform_block_ && uniform_ring_.used > uniform_flushed_)
            dkMemBlockFlushCpuCache(uniform_block_, uint32_t(cur_) * uniform_slot_bytes_ + uniform_flushed_,
                                    uniform_ring_.used - uniform_flushed_);
        stream_flushed_ = stream_off_;
        uniform_flushed_ = uniform_ring_.used;
    }
    __asm__ __volatile__("dmb st" ::: "memory");
    DkCmdList list = dkCmdBufFinishList(cmd_);
    dkQueueSubmitCommands(queue_, list);
    if (flush) dkQueueFlush(queue_);
    draws_since_submit_ = 0;
    vertices_since_submit_ = 0;
    submit_stream_base_ = stats_.stream_bytes;
    submit_push_base_ = stats_.push_bytes;
}

void DekoDevice::MaybeSubmit(uint32_t vertices) {
    ++draws_since_submit_;
    vertices_since_submit_ += vertices;
    if (!SubmitDue(draws_since_submit_, vertices_since_submit_,
                   stats_.stream_bytes - submit_stream_base_,
                   stats_.push_bytes - submit_push_base_)) return;
    Submit(true);
}

void DekoDevice::DrainForFence() {
    if (!gr_pending_) return;
    dkCmdBufTiledCacheOp(cmd_, DkTiledCacheOp_Flush);
    dkCmdBufBarrier(cmd_, DkBarrier_Full, 0);
    gr_pending_ = false;
}

void DekoDevice::WaitIdle() {

    if (ce_pending_) {
        dkCmdBufBarrier(cmd_, DkBarrier_Full, 0);
        gr_pending_ = false;
    }
    DrainForFence();
    DkFence f{};
    dkCmdBufSignalFence(cmd_, &f, true);
    Submit(true);
    const DkResult result = dkFenceWait(&f, -1);
    if (result != DkResult_Success) {
        std::fprintf(stderr, "[deko] WaitIdle fence wait failed: %d\n", int(result));
        std::abort();
    }
}

void* DekoDevice::StreamAlloc(uint32_t size, uint32_t align, DkGpuAddr* gpu) {
    uint32_t off = Align(stream_off_, align);
    if (off + size > stream_slot_bytes_) {
        if (size > stream_slot_bytes_) return nullptr;
        Rotate();
        off = 0;
    }
    stream_off_ = off + size;
    stats_.stream_bytes += size;
    const uint32_t base = uint32_t(cur_) * stream_slot_bytes_ + off;
    *gpu = stream_gpu_ + base;
    return stream_cpu_ + base;
}

bool DekoDevice::Arena::Alloc(DkDevice dev, uint32_t size, uint32_t align, uint32_t* block, uint32_t* off) {
    size = Align(size, 64);
    align = std::max<uint32_t>(align, 64);
    for (uint32_t b = 0; b < blocks.size(); ++b) {
        auto& fl = blocks[b].free;
        for (auto it = fl.begin(); it != fl.end(); ++it) {
            const uint32_t at = Align(it->first, align);
            if (at + size > it->first + it->second) continue;
            const uint32_t r_off = it->first, r_end = it->first + it->second;
            fl.erase(it);
            if (at > r_off) fl.emplace(r_off, at - r_off);
            if (at + size < r_end) fl.emplace(at + size, r_end - (at + size));
            *block = b;
            *off = at;
            return true;
        }
    }

    Block nb;
    nb.size = std::max(block_size, Align(size, DK_MEMBLOCK_ALIGNMENT));
    DkMemBlockMaker mm;
    dkMemBlockMakerDefaults(&mm, dev, nb.size);
    mm.flags = flags;
    nb.mem = dkMemBlockCreate(&mm);
    if (!nb.mem) return false;
    if ((flags & DkMemBlockFlags_CpuAccessMask) != 0) nb.cpu = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(nb.mem));
    nb.gpu = dkMemBlockGetGpuAddr(nb.mem);
    if (nb.size > size) nb.free.emplace(size, nb.size - size);

    for (uint32_t b = 0; b < blocks.size(); ++b)
        if (!blocks[b].mem) {
            blocks[b] = std::move(nb);
            *block = b;
            *off = 0;
            return true;
        }
    blocks.push_back(std::move(nb));
    *block = uint32_t(blocks.size() - 1);
    *off = 0;
    return true;
}

void DekoDevice::Arena::Free(uint32_t block, uint32_t off, uint32_t size) {
    if (block >= blocks.size() || !blocks[block].mem) return;
    Block& bk = blocks[block];
    auto& fl = bk.free;
    auto it = fl.emplace(off, Align(size, 64)).first;
    if (it != fl.begin()) {
        auto prev = std::prev(it);
        if (prev->first + prev->second == it->first) {
            prev->second += it->second;
            fl.erase(it);
            it = prev;
        }
    }
    auto next = std::next(it);
    if (next != fl.end() && it->first + it->second == next->first) {
        it->second += next->second;
        fl.erase(next);
    }

    if (bk.size > block_size && fl.size() == 1 && fl.begin()->first == 0 && fl.begin()->second >= bk.size) {
        dkMemBlockDestroy(bk.mem);
        bk = Block{};
    }
}

void DekoDevice::Arena::Destroy() {
    for (Block& b : blocks)
        if (b.mem) dkMemBlockDestroy(b.mem);
    blocks.clear();
}

bool DekoDevice::AllocImage(DkSurface& s, const DkImageLayoutMaker& lm) {
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &lm);
    const uint32_t size = uint32_t(dkImageLayoutGetSize(&layout));
    if (!image_arena_.Alloc(dev_, size, dkImageLayoutGetAlignment(&layout), &s.mem_block, &s.mem_off)) return false;
    s.mem_size = size;
    dkImageInitialize(&s.image, &layout, image_arena_.blocks[s.mem_block].mem, s.mem_off);
    return true;
}

void DekoDevice::ReleaseRetired(Slot& s) {
    for (DkMemBlock b : s.extra_cmd) dkMemBlockDestroy(b);
    s.extra_cmd.clear();
    const auto release = [this](std::vector<Retired>& list) {
        for (const Retired& r : list) {
            if (r.arena == 0 && r.size) image_arena_.Free(r.block, r.off, r.size);
            if (r.arena == 1 && r.size) mesh_arena_.Free(r.block, r.off, r.size);
            if (r.desc_id) free_desc_.push_back(r.desc_id);
        }
        list.clear();
    };
    release(s.limbo);

    s.limbo.swap(s.graveyard);
}


uint32_t DekoDevice::AllocDescriptor() {
    if (!free_desc_.empty()) {
        const uint32_t id = free_desc_.back();
        free_desc_.pop_back();
        return id;
    }
    if (next_desc_ >= kMaxImages) return 0;
    return next_desc_++;
}

void DekoDevice::WriteImageDescriptor(DkSurface& s) {
    DkImageView v;
    dkImageViewDefaults(&v, &s.image);
    if (s.format == DkImageFormat_R8_Unorm || s.format == DkImageFormat_ZF32 || s.format == DkImageFormat_Z24S8 ||
        s.format == DkImageFormat_ZF32_X24S8) {

        v.swizzle[0] = DkImageSwizzle_Red;
        v.swizzle[1] = DkImageSwizzle_Zero;
        v.swizzle[2] = DkImageSwizzle_Zero;
        v.swizzle[3] = DkImageSwizzle_One;
    }
    const int levels = s.levels > 0 ? s.levels + s.synth_levels + 1 : 1;
    v.mipLevelCount = uint8_t(std::min<int>(levels, s.mip_capacity));
    DkImageDescriptor d;
    dkImageDescriptorInitialize(&d, &v, false, false);
    dkCmdBufPushData(cmd_, image_desc_gpu_ + DkGpuAddr(s.desc_id) * sizeof(DkImageDescriptor), &d, sizeof d);
    descriptors_dirty_ = true;
}

void DekoDevice::Barrier3D() {

    dkCmdBufBarrier(cmd_, DkBarrier_Fragments, DkInvalidateFlags_Image);
    epoch_++;
}

void DekoDevice::BeforeCopyEngine() {

    if (gr_pending_) {

        dkCmdBufTiledCacheOp(cmd_, DkTiledCacheOp_Flush);
        dkCmdBufBarrier(cmd_, DkBarrier_Full, 0);
        gr_pending_ = false;
        epoch_++;
    }
}

void DekoDevice::DrawHazards(uint32_t read_mask) {

    bool need = false;
    for (unsigned i = 0; i < 5; ++i)
        if ((read_mask & (1u << i)) && want_.tex_surface[i] && want_.tex_surface[i]->write_epoch == epoch_) need = true;
    DkSurface* depth = want_.depth && (want_.depth_write || want_.stencil) ? want_.depth : nullptr;
    if (want_.color && want_.color->read_epoch == epoch_) need = true;
    if (depth && depth->read_epoch == epoch_) need = true;
    if (need) Barrier3D();
    for (unsigned i = 0; i < 5; ++i)
        if ((read_mask & (1u << i)) && want_.tex_surface[i]) want_.tex_surface[i]->read_epoch = epoch_;
    if (want_.color) want_.color->write_epoch = epoch_;
    if (depth) depth->write_epoch = epoch_;
}

void DekoDevice::NoteSample(DkSurface* s) {
    if (!s) return;
    if (s->write_epoch == epoch_) Barrier3D();
    s->read_epoch = epoch_;
}

void DekoDevice::NoteWrite(DkSurface* s) {
    if (!s) return;
    if (s->read_epoch == epoch_) Barrier3D();
    s->write_epoch = epoch_;
}

static void SafeSchedule(uint8_t* code, const uint8_t* dksh, uint32_t code_sz) {
    const auto* hdr = reinterpret_cast<const DkshHeader*>(dksh);
    const auto* prog = reinterpret_cast<const DkshProgramHeader*>(dksh + hdr->programs_off);
    for (uint32_t g = prog->entrypoint + 0x50; g + 32 <= code_sz; g += 32) {
        uint64_t c;
        std::memcpy(&c, code + g, 8);
        if (!c) break;
        for (int k = 0; k < 3; ++k) {
            uint64_t slot = (c >> (21 * k)) & 0x1FFFFFull;
            slot |= 0x3Full << 11;
            if ((slot & 15) < 6) slot = (slot & ~15ull) | 6;
            c = (c & ~(0x1FFFFFull << (21 * k))) | (slot << (21 * k));
        }
        std::memcpy(code + g, &c, 8);
    }
}

bool DekoDevice::Init(int window_w, int window_h) {
    Shutdown();
    window_w_ = window_w;
    window_h_ = window_h;
    DkDeviceMaker dm;
    dkDeviceMakerDefaults(&dm);

    dm.flags = DkDeviceFlags_DepthZeroToOne | DkDeviceFlags_OriginLowerLeft;
    dev_ = dkDeviceCreate(&dm);
    if (!dev_) return false;
    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, dev_);
    qm.flags = DkQueueFlags_Graphics | DkQueueFlags_MediumPrio | DkQueueFlags_EnableZcull;
    queue_ = dkQueueCreate(&qm);
    if (!queue_) return false;

    static_assert(DK_UNIFORM_BUF_ALIGNMENT == DkUniformRing::kAlignment, "uniform ring alignment");

    cmd_slot_bytes_ = Align(2048u * 1024u, DK_MEMBLOCK_ALIGNMENT);
    stream_slot_bytes_ = Align(8192u * 1024u, DK_MEMBLOCK_ALIGNMENT);
    register_memo_valid_ = false;
    const uint32_t write_cpu = DkMemBlockFlags_CpuCached;
    DkMemBlockMaker mm;
    dkMemBlockMakerDefaults(&mm, dev_, cmd_slot_bytes_ * uint32_t(slots_n_));
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    cmd_block_ = dkMemBlockCreate(&mm);
    dkMemBlockMakerDefaults(&mm, dev_, stream_slot_bytes_ * uint32_t(slots_n_));
    mm.flags = write_cpu | DkMemBlockFlags_GpuCached;
    stream_block_ = dkMemBlockCreate(&mm);
    if (!cmd_block_ || !stream_block_) return false;
    stream_cpu_ = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(stream_block_));
    stream_gpu_ = dkMemBlockGetGpuAddr(stream_block_);
    {

        uniform_slot_bytes_ = Align(2048u * 1024u, DK_MEMBLOCK_ALIGNMENT);
        dkMemBlockMakerDefaults(&mm, dev_, uniform_slot_bytes_ * uint32_t(slots_n_));
        mm.flags = write_cpu | DkMemBlockFlags_GpuCached;
        uniform_block_ = dkMemBlockCreate(&mm);
        if (!uniform_block_) return false;
        uniform_cpu_ = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(uniform_block_));
        uniform_gpu_ = dkMemBlockGetGpuAddr(uniform_block_);
    }

    image_arena_.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    image_arena_.block_size = 32768u * 1024u;
    mesh_arena_.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    mesh_arena_.block_size = 8192u * 1024u;

    DkCmdBufMaker cm;
    dkCmdBufMakerDefaults(&cm, dev_);
    cm.userData = this;
    cm.cbAddMem = &DekoDevice::CmdAddMem;
    cmd_ = dkCmdBufCreate(&cm);
    if (!cmd_) return false;
    {

        draw_packets_ = std::make_unique<DkDrawPacketCache>();
        DkCmdBufMaker pm;
        dkCmdBufMakerDefaults(&pm, dev_);
        packet_cmd_ = dkCmdBufCreate(&pm);
        if (!packet_cmd_) return false;
    }

    cur_ = 0;
    BeginSlot();

    shadow_count_loaded_ = false;

    ps2_vs_variants_.clear();
    const dkgen::FsVariant* const secondary_sets[3] = {dkgen::kPs2VariantsAlphaFirst, dkgen::kPs2VariantsAtestFast,
                                                       dkgen::kPs2VariantsLean};
    uint32_t code_bytes = 0;
    for (int i = 0; i < dkgen::kProgramCount; ++i)
        for (const dkgen::Blob* b : {&dkgen::kPrograms[i].vs, &dkgen::kPrograms[i].fs}) {
            const auto* h = reinterpret_cast<const DkshHeader*>(b->data);
            code_bytes = Align(code_bytes, DK_SHADER_CODE_ALIGNMENT) + h->code_sz;
        }
    for (int i = 0; i < dkgen::kPs2VariantCount; ++i) {
        const auto* h = reinterpret_cast<const DkshHeader*>(dkgen::kPs2Variants[i].fs.data);
        code_bytes = Align(code_bytes, DK_SHADER_CODE_ALIGNMENT) + h->code_sz;
    }
    for (const dkgen::FsVariant* set : secondary_sets)
        for (int i = 0; i < dkgen::kPs2VariantCount; ++i) {
            if (set[i].fs.data == dkgen::kPs2Variants[i].fs.data) continue;
            const auto* h = reinterpret_cast<const DkshHeader*>(set[i].fs.data);
            code_bytes = Align(code_bytes, DK_SHADER_CODE_ALIGNMENT) + h->code_sz;
        }
    for (const auto& b : dkgen::kPs2VsVariants) {
        const auto* h = reinterpret_cast<const DkshHeader*>(b.data);
        code_bytes = Align(code_bytes, DK_SHADER_CODE_ALIGNMENT) + h->code_sz;
    }
    code_bytes = Align(code_bytes + DK_SHADER_CODE_UNUSABLE_SIZE, DK_MEMBLOCK_ALIGNMENT);
    dkMemBlockMakerDefaults(&mm, dev_, code_bytes);
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code;
    code_block_ = dkMemBlockCreate(&mm);

    uint32_t ubo_bytes = 0;
    for (int i = 0; i < dkgen::kProgramCount; ++i) ubo_bytes += Align(dkgen::kPrograms[i].ubo_size, DK_UNIFORM_BUF_ALIGNMENT);
    const uint32_t img_desc_off = Align(ubo_bytes, DK_IMAGE_DESCRIPTOR_ALIGNMENT);
    const uint32_t smp_desc_off = img_desc_off + kMaxImages * uint32_t(sizeof(DkImageDescriptor));
    const uint32_t data_bytes = Align(smp_desc_off + kMaxSamplers * uint32_t(sizeof(DkSamplerDescriptor)), DK_MEMBLOCK_ALIGNMENT);
    dkMemBlockMakerDefaults(&mm, dev_, data_bytes);
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_ZeroFillInit;
    data_block_ = dkMemBlockCreate(&mm);
    if (!code_block_ || !data_block_) return false;
    auto* code_cpu = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(code_block_));
    auto* data_cpu = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(data_block_));
    const DkGpuAddr data_gpu = dkMemBlockGetGpuAddr(data_block_);
    uint32_t code_off = 0, ubo_off = 0;

    for (int i = 0; i < dkgen::kProgramCount; ++i) {
        Program& p = programs_[i];
        const dkgen::Program& g = dkgen::kPrograms[i];
        for (int stage = 0; stage < 2; ++stage) {
            const dkgen::Blob& b = stage == 0 ? g.vs : g.fs;
            const auto* h = reinterpret_cast<const DkshHeader*>(b.data);
            code_off = Align(code_off, DK_SHADER_CODE_ALIGNMENT);
            std::memcpy(code_cpu + code_off, b.data + h->control_sz, h->code_sz);
            if (stage == 0) SafeSchedule(code_cpu + code_off, b.data, h->code_sz);
            DkShaderMaker sm;
            dkShaderMakerDefaults(&sm, code_block_, code_off);
            sm.control = b.data;
            dkShaderInitialize(stage == 0 ? &p.vs : &p.fs, &sm);
            if (!dkShaderIsValid(stage == 0 ? &p.vs : &p.fs)) {
                std::fprintf(stderr, "[deko] shader %s/%s is not valid\n", g.name, stage == 0 ? "vs" : "fs");
                return false;
            }
            code_off += h->code_sz;
        }
        p.size = g.ubo_size;
        p.ubo_size = Align(g.ubo_size, DK_UNIFORM_BUF_ALIGNMENT);
        p.ubo = data_gpu + ubo_off;
        ubo_off += p.ubo_size;
        if (p.ubo_size > DkUniformRing::kMaxBinding) return false;
        p.snapshot = DkUniformSnapshot{};
        p.shadow.assign(p.size, 0);
        p.dirty = 0;
        p.key_dirty = ~0u;
        if (i == dkgen::k_shadow_count) shadow_count_loaded_ = true;
    }

    const auto load_fragment = [&](const dkgen::Blob& b, DkShader* out) {
        const auto* h = reinterpret_cast<const DkshHeader*>(b.data);
        code_off = Align(code_off, DK_SHADER_CODE_ALIGNMENT);
        std::memcpy(code_cpu + code_off, b.data + h->control_sz, h->code_sz);
        DkShaderMaker sm;
        dkShaderMakerDefaults(&sm, code_block_, code_off);
        sm.control = b.data;
        dkShaderInitialize(out, &sm);
        code_off += h->code_sz;
        return dkShaderIsValid(out);
    };
    ps2_fs_variants_.clear();
    ps2_variant_keys_.clear();
    ps2_sampler_masks_.clear();
    for (int i = 0; i < dkgen::kPs2VariantCount; ++i) {
        const dkgen::FsVariant& v = dkgen::kPs2Variants[i];
        DkShader shader;
        if (!load_fragment(v.fs, &shader)) {
            std::fprintf(stderr, "[deko] ps2 variant %d is not valid\n", i);
            return false;
        }
        ps2_fs_variants_.push_back(shader);
        GsSpecialize::Key key;
        key.lo = v.key_lo;
        key.hi = v.key_hi;
        ps2_variant_keys_.push_back(key);
        ps2_sampler_masks_.push_back(uint8_t(DkPs2SamplerMask(key)));
    }
    ps2_variant_memo_valid_ = false;
    std::vector<DkShader>* const secondary_fs[3] = {&alpha_first_fs_, &atest_fast_fs_, &lean_fs_};
    for (int k = 0; k < 3; ++k) {
        secondary_fs[k]->clear();
        for (int i = 0; i < dkgen::kPs2VariantCount; ++i) {
            const dkgen::Blob& b = secondary_sets[k][i].fs;
            if (b.data == dkgen::kPs2Variants[i].fs.data) {
                secondary_fs[k]->push_back(ps2_fs_variants_[size_t(i)]);
                continue;
            }
            DkShader shader;
            if (!load_fragment(b, &shader)) {
                std::fprintf(stderr, "[deko] ps2 fragment variant %d is not valid\n", i);
                return false;
            }
            secondary_fs[k]->push_back(shader);
        }
    }
    ps2_vs_variants_.clear();
    {
        bool all_valid = true;
        for (const auto& b : dkgen::kPs2VsVariants) {
            const auto* h = reinterpret_cast<const DkshHeader*>(b.data);
            code_off = Align(code_off, DK_SHADER_CODE_ALIGNMENT);
            std::memcpy(code_cpu + code_off, b.data + h->control_sz, h->code_sz);
            SafeSchedule(code_cpu + code_off, b.data, h->code_sz);
            DkShaderMaker sm;
            dkShaderMakerDefaults(&sm, code_block_, code_off);
            sm.control = b.data;
            DkShader shader;
            dkShaderInitialize(&shader, &sm);
            all_valid = all_valid && dkShaderIsValid(&shader);
            ps2_vs_variants_.push_back(shader);
            code_off += h->code_sz;
        }
        if (!all_valid) ps2_vs_variants_.clear();
    }
    ps2_ = dkgen::k_ps2;
    strip_ = dkgen::k_strip;
    invert_ = dkgen::k_invert;
    constq_ = dkgen::k_constq;
    opaque_ = dkgen::k_opaque;
    scanout_ = dkgen::k_scanout;
    scanout_lean_ = dkgen::k_scanout_lean;
    scanout_fx_ = dkgen::k_scanout_fx;
    for (int i = 0; i < slots_n_; ++i) {
        present_desc_[i] = AllocDescriptor();
        if (!present_desc_[i]) throw std::runtime_error("present descriptor allocation");
    }
    copy_ = dkgen::k_copy;
    yuv_ = dkgen::k_yuv;
    haze_ = dkgen::k_haze;
    image_desc_gpu_ = data_gpu + img_desc_off;
    sampler_desc_gpu_ = data_gpu + smp_desc_off;

    {
        auto* sd = reinterpret_cast<DkSamplerDescriptor*>(data_cpu + smp_desc_off);
        for (uint32_t mi = 0; mi < 6; ++mi)
            for (uint32_t mag = 0; mag < 2; ++mag)
                for (uint32_t ws = 0; ws < 2; ++ws)
                    for (uint32_t wt = 0; wt < 2; ++wt)
                        for (uint32_t an = 0; an < 5; ++an) {
                            DkSampler s;
                            dkSamplerDefaults(&s);
                            s.minFilter = (mi == 1 || mi == 3 || mi == 5) ? DkFilter_Linear : DkFilter_Nearest;
                            s.mipFilter = mi < 2 ? DkMipFilter_None : (mi < 4 ? DkMipFilter_Nearest : DkMipFilter_Linear);
                            s.magFilter = mag ? DkFilter_Linear : DkFilter_Nearest;
                            s.wrapMode[0] = ws ? DkWrapMode_Repeat : DkWrapMode_ClampToEdge;
                            s.wrapMode[1] = wt ? DkWrapMode_Repeat : DkWrapMode_ClampToEdge;
                            s.wrapMode[2] = DkWrapMode_ClampToEdge;
                            s.maxAnisotropy = float(1u << an);
                            const uint32_t idx = ((((mi * 2 + mag) * 2 + ws) * 2 + wt) * 5) + an;
                            dkSamplerDescriptorInitialize(&sd[idx], &s);
                        }
    }

    dkCmdBufBindImageDescriptorSet(cmd_, image_desc_gpu_, kMaxImages);
    dkCmdBufBindSamplerDescriptorSet(cmd_, sampler_desc_gpu_, kMaxSamplers);
    dkCmdBufTiledCacheOp(cmd_, DkTiledCacheOp_Enable);
    DkRasterizerState rs;
    dkRasterizerStateDefaults(&rs);
    rs.cullMode = DkFace_None;
    dkCmdBufBindRasterizerState(cmd_, &rs);
    DkMultisampleState ms;
    dkMultisampleStateDefaults(&ms);
    dkCmdBufBindMultisampleState(cmd_, &ms);
    applied_ = Applied{};
    want_ = Want{};

    {
        dummy_ = std::make_unique<DkSurface>();
        DkSurface& d = *dummy_;
        d.desc.width = d.desc.height = 1;
        d.format = DkImageFormat_RGBA8_Unorm;
        DkImageLayoutMaker lm;
        dkImageLayoutMakerDefaults(&lm, dev_);
        lm.flags = DkImageFlags_UsageRender;
        lm.format = d.format;
        lm.dimensions[0] = lm.dimensions[1] = 1;
        if (!AllocImage(d, lm)) return false;
        d.desc_id = 0;
        WriteImageDescriptor(d);
        const uint8_t white[4] = {255, 255, 255, 255};
        DkGpuAddr g = 0;
        void* p = StreamAlloc(4, 256, &g);
        std::memcpy(p, white, 4);
        DkImageView v;
        dkImageViewDefaults(&v, &d.image);
        DkCopyBuf src{g, 0, 0};
        DkImageRect r{0, 0, 0, 1, 1, 1};
        dkCmdBufCopyBufferToImage(cmd_, &src, &v, &r, 0);
        AfterCopyEngine();
    }
    for (int i = 0; i < 5; ++i) want_.tex[i] = Handle(nullptr, 0);

    if (!MakeSwapchain(window_w > 0 ? window_w : 1280, window_h > 0 ? window_h : 720)) return false;
    Submit(true);
    WaitIdle();
    initialized_ = true;
    return true;
}

bool DekoDevice::MakeSwapchain(int w, int h) {
    if (swapchain_ || swap_block_) {
        WaitIdle();
        if (swapchain_) { dkSwapchainDestroy(swapchain_); swapchain_ = nullptr; }
        if (swap_block_) { dkMemBlockDestroy(swap_block_); swap_block_ = nullptr; }
    }
    applied_.have_target = false;
    swap_w_ = w;
    swap_h_ = h;
    DkImageLayoutMaker lm;
    dkImageLayoutMakerDefaults(&lm, dev_);
    lm.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent;
    lm.format = DkImageFormat_RGBA8_Unorm;
    lm.dimensions[0] = uint32_t(swap_w_);
    lm.dimensions[1] = uint32_t(swap_h_);
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &lm);
    const uint32_t size = Align(uint32_t(dkImageLayoutGetSize(&layout)), dkImageLayoutGetAlignment(&layout));
    DkMemBlockMaker mm;
    dkMemBlockMakerDefaults(&mm, dev_, Align(size * uint32_t(swap_images_n_), DK_MEMBLOCK_ALIGNMENT));
    mm.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    swap_block_ = dkMemBlockCreate(&mm);
    if (!swap_block_) return false;
    DkImage const* images[kSwapImages];
    for (int i = 0; i < swap_images_n_; ++i) {
        dkImageInitialize(&swap_images_[i], &layout, swap_block_, uint32_t(i) * size);
        images[i] = &swap_images_[i];
    }
    DkSwapchainMaker sm;
    NWindow* window = nwindowGetDefault();

    if (window->slots_configured) throw std::runtime_error("present window buffers were not released");
    window->format = PIXEL_FORMAT_RGBA_8888;
    dkSwapchainMakerDefaults(&sm, dev_, window, images, uint32_t(swap_images_n_));
    swapchain_ = dkSwapchainCreate(&sm);
    if (!swapchain_) return false;
    dkSwapchainSetSwapInterval(swapchain_, uint32_t(swap_interval_));
    return true;
}

void DekoDevice::Shutdown() {
    if (!dev_) return;
    if (queue_) dkQueueWaitIdle(queue_);
    for (Slot& s : slots_) {
        s.armed = false;
        ReleaseRetired(s);
    }
    surfaces_.clear();
    surface_index_.clear();
    dummy_.reset();
    image_arena_.Destroy();
    mesh_arena_.Destroy();
    meshes_.clear();
    mesh_index_.clear();
    if (swapchain_) { dkSwapchainDestroy(swapchain_); swapchain_ = nullptr; }
    if (swap_block_) { dkMemBlockDestroy(swap_block_); swap_block_ = nullptr; }
    if (readback_block_) { dkMemBlockDestroy(readback_block_); readback_block_ = nullptr; readback_size_ = 0; }
    if (packet_cmd_) { dkCmdBufDestroy(packet_cmd_); packet_cmd_ = nullptr; }
    draw_packets_.reset();
    if (cmd_) { dkCmdBufDestroy(cmd_); cmd_ = nullptr; }
    for (DkMemBlock* b : {&cmd_block_, &stream_block_, &code_block_, &data_block_, &uniform_block_})
        if (*b) { dkMemBlockDestroy(*b); *b = nullptr; }
    if (queue_) { dkQueueDestroy(queue_); queue_ = nullptr; }
    dkDeviceDestroy(dev_);
    dev_ = nullptr;
    uniform_cpu_ = nullptr;
    uniform_gpu_ = 0;
    uniform_slot_bytes_ = 0;
    uniform_ring_ = DkUniformRing{};
    applied_ = Applied{};
    free_desc_.clear();
    next_desc_ = 1;
    initialized_ = false;
}

void DekoDevice::SetSwapInterval(int interval) {
    interval = interval > 0 ? 1 : 0;
    if (interval == swap_interval_ || !swapchain_) return;
    swap_interval_ = interval;
    dkSwapchainSetSwapInterval(swapchain_, uint32_t(interval));
}

Caps DekoDevice::QueryCaps() const {
    Caps c;
    c.max_texture_size = 16384;
    c.staging_readback = false;
    c.reverse_subtract = true;
    c.depth_sample = true;
    c.texture_barrier = false;
    c.compute = false;
    c.yuv420_convert = true;
    c.depth_haze = true;
    return c;
}

static bool Depth32F() { return true; }
static bool DepthStencilFormat(DkImageFormat f) { return f == DkImageFormat_Z24S8 || f == DkImageFormat_ZF32_X24S8; }

SurfaceId DekoDevice::CreateSurface(const SurfaceDesc& desc) {
    if (desc.width == 0 || desc.height == 0 || !dev_) return SurfaceId{};
    SafePoint();
    ops_++;
    auto s = std::make_unique<DkSurface>();
    s->desc = desc;
    if (s->desc.scale == 0) s->desc.scale = 1;
    uint32_t flags = 0;
    switch (desc.format) {
        case SurfaceFormat::DepthStencil24_8:
            s->format = Depth32F() ? DkImageFormat_ZF32_X24S8 : DkImageFormat_Z24S8;
            s->depth = s->stencil = true;
            flags = DkImageFlags_UsageRender;
            break;
        case SurfaceFormat::Depth32f:
            s->format = DkImageFormat_ZF32;
            s->depth = true;
            flags = DkImageFlags_UsageRender;
            break;
        case SurfaceFormat::R8:
            s->format = DkImageFormat_R8_Unorm;
            flags = DkImageFlags_UsageRender;
            break;
        default:
            s->format = DkImageFormat_RGBA8_Unorm;

            flags = DkImageFlags_UsageRender;
            break;
    }

    s->mip_capacity = 1;
    if (!desc.renderable && (desc.format == SurfaceFormat::Rgba8 || desc.format == SurfaceFormat::R8)) {
        uint32_t m = std::max(desc.width, desc.height);
        uint8_t n = 1;
        while (m > 1) { m >>= 1; ++n; }
        s->mip_capacity = n;
    }

    if (s->depth || (desc.renderable && desc.format == SurfaceFormat::Rgba8)) flags |= DkImageFlags_HwCompression;
    DkImageLayoutMaker lm;
    dkImageLayoutMakerDefaults(&lm, dev_);
    lm.flags = flags;
    lm.format = s->format;
    lm.dimensions[0] = desc.width;
    lm.dimensions[1] = desc.height;
    lm.mipLevels = s->mip_capacity;

    if (desc.height <= 8) {
        lm.flags |= DkImageFlags_CustomTileSize;
        lm.tileSize = DkTileSize_OneGob;
    }
    if (!AllocImage(*s, lm)) return SurfaceId{};
    if (desc.format != SurfaceFormat::DepthStencil24_8) {
        s->desc_id = AllocDescriptor();
        if (!s->desc_id) {
            RetireImage(*s);
            return SurfaceId{};
        }
        WriteImageDescriptor(*s);
    }
    const uint32_t id = next_surface_++;
    DkSurface* raw = s.get();
    surfaces_[id] = std::move(s);
    IndexSurface(id, raw);
    if (raw->depth) {

        Before3D();
        want_.target_image = nullptr;
        want_.color = nullptr;
        want_.depth = raw;
        ApplyTarget();
        NoteWrite(raw);
        const DkScissor full{0, 0, 0x4000, 0x4000};
        dkCmdBufSetScissors(cmd_, 0, &full, 1);
        applied_.scissor = full;
        applied_.scissor_valid = true;
        dkCmdBufClearDepthStencil(cmd_, true, 0.f, raw->stencil ? 0xFF : 0, 0);
    }
    if (desc.renderable && desc.format == SurfaceFormat::Rgba8) {

        Before3D();
        want_.target_image = nullptr;
        want_.color = raw;
        want_.depth = nullptr;
        ApplyTarget();
        NoteWrite(raw);
        const DkScissor full{0, 0, 0x4000, 0x4000};
        dkCmdBufSetScissors(cmd_, 0, &full, 1);
        applied_.scissor = full;
        applied_.scissor_valid = true;
        dkCmdBufClearColorFloat(cmd_, 0, DkColorMask_RGBA, 0.f, 0.f, 0.f, 0.f);
    }
    return SurfaceId{id};
}

SurfaceId DekoDevice::CreateCompressed(uint32_t w, uint32_t h, uint32_t format, int levels, const void* const* data,
                                       const size_t* sizes) {
    const DkImageFormat fmt = format == kReplacementRgba8 ? DkImageFormat_RGBA8_Unorm
                            : format == kCompressedBc1 ? DkImageFormat_RGBA_BC1
                            : format == kCompressedBc3 ? DkImageFormat_RGBA_BC3
                            : format == kCompressedBc7 ? DkImageFormat_RGBA_BC7_Unorm
                                                       : DkImageFormat_None;
    if (fmt == DkImageFormat_None || !w || !h || levels <= 0 || levels > 14 || !data || !sizes || !dev_) return SurfaceId{};
    SafePoint();
    ops_++;
    auto s = std::make_unique<DkSurface>();
    s->desc.width = w;
    s->desc.height = h;
    s->desc.format = SurfaceFormat::Rgba8;
    s->format = fmt;
    s->mip_capacity = uint8_t(levels);
    DkImageLayoutMaker lm;
    dkImageLayoutMakerDefaults(&lm, dev_);
    lm.format = fmt;
    lm.dimensions[0] = w;
    lm.dimensions[1] = h;
    lm.mipLevels = uint32_t(levels);

    if (h <= (format == kReplacementRgba8 ? 8u : 32u)) {
        lm.flags |= DkImageFlags_CustomTileSize;
        lm.tileSize = DkTileSize_OneGob;
    }
    if (!AllocImage(*s, lm)) return SurfaceId{};
    s->desc_id = AllocDescriptor();
    if (!s->desc_id) {
        RetireImage(*s);
        return SurfaceId{};
    }
    for (int i = 0; i < levels; ++i) {
        if (sizes[i] > stream_slot_bytes_ / 2) {
            break;
        }
        DkGpuAddr g = 0;
        auto* dst = static_cast<uint8_t*>(StreamAlloc(uint32_t(sizes[i]), 256, &g));
        if (!dst) break;
        std::memcpy(dst, data[i], sizes[i]);
        DkImageView v;
        dkImageViewDefaults(&v, &s->image);
        v.mipLevelOffset = uint8_t(i);
        BeforeCopyEngine();
        DkCopyBuf cb{g, 0, 0};
        DkImageRect ir{0, 0, 0, std::max<uint32_t>(w >> i, 1), std::max<uint32_t>(h >> i, 1), 1};
        dkCmdBufCopyBufferToImage(cmd_, &cb, &v, &ir, 0);
        AfterCopyEngine();
        s->levels = uint8_t(i);
    }
    WriteImageDescriptor(*s);
    const uint32_t id = next_surface_++;
    IndexSurface(id, s.get());
    surfaces_[id] = std::move(s);
    return SurfaceId{id};
}

void DekoDevice::IndexSurface(uint32_t id, DkSurface* s) {
    if (surface_index_.size() <= id) surface_index_.resize(size_t(id) + 1 + (id >> 1), nullptr);
    surface_index_[id] = s;
}

void DekoDevice::DestroySurface(SurfaceId id) {
    auto it = surfaces_.find(id.id);
    if (it == surfaces_.end()) return;
    ops_++;
    DkSurface* s = it->second.get();

    if (want_.color == s) want_.color = nullptr;
    if (want_.depth == s) want_.depth = nullptr;
    if (applied_.color == &s->image || applied_.depth == &s->image) applied_.have_target = false;
    for (int i = 0; i < 5; ++i)
        if (want_.tex_surface[i] == s) { want_.tex_surface[i] = nullptr; want_.tex[i] = Handle(nullptr, 0); }

    RetireImage(*s);
    if (id.id < surface_index_.size()) surface_index_[id.id] = nullptr;
    surfaces_.erase(it);
    run_valid_ = false;
}

SurfaceDesc DekoDevice::DescribeSurface(SurfaceId id) const {
    const DkSurface* s = Surf(id);
    return s ? s->desc : SurfaceDesc{};
}

DkResHandle DekoDevice::Handle(const DkSurface* s, uint32_t sampler) const {
    return dkMakeTextureHandle(s ? s->desc_id : 0, sampler);
}

uint32_t DekoDevice::SamplerIndex(uint32_t gl_mag, uint32_t gl_min, bool wrap_s_repeat, bool wrap_t_repeat, int aniso) {
    const uint32_t mi = uint32_t(MinIndex(gl_min));
    const uint32_t mag = gl_mag == kGlLinear ? 1 : 0;
    return ((((mi * 2 + mag) * 2 + (wrap_s_repeat ? 1 : 0)) * 2 + (wrap_t_repeat ? 1 : 0)) * 5) + uint32_t(AnisoIndex(aniso));
}

void DekoDevice::FullTarget(DkSurface* color, DkSurface* depth, int w, int h) {
    want_.target_image = nullptr;
    want_.color = color;
    want_.depth = depth;
    want_.viewport = DkViewport{0.f, 0.f, float(w), float(h), 0.f, 1.f};
    want_.scissor = DkScissor{0, 0, 0x4000, 0x4000};
}

void DekoDevice::Clear(SurfaceId color, SurfaceId depth, uint32_t rgba, float z, uint32_t flags) {
    DkSurface* c = Surf(color);
    DkSurface* d = Surf(depth);
    if (!c || !c->desc.renderable) return;
    SafePoint();
    ops_++;
    run_valid_ = false;
    Before3D();
    want_.target_image = nullptr;
    want_.color = c;
    want_.depth = d && d->depth ? d : nullptr;
    ApplyTarget();
    const DkScissor full{0, 0, 0x4000, 0x4000};
    dkCmdBufSetScissors(cmd_, 0, &full, 1);
    applied_.scissor = full;
    applied_.scissor_valid = true;
    if (flags & kClearColor) {
        NoteWrite(c);
        dkCmdBufClearColorFloat(cmd_, 0, DkColorMask_RGBA, ((rgba >> 0) & 0xFF) / 255.0f, ((rgba >> 8) & 0xFF) / 255.0f,
                                ((rgba >> 16) & 0xFF) / 255.0f, ((rgba >> 24) & 0xFF) / 255.0f);
    }
    if ((flags & kClearDepth) && want_.depth) {
        NoteWrite(want_.depth);
        dkCmdBufClearDepthStencil(cmd_, true, z, 0, 0);
    }
}

void DekoDevice::Upload(SurfaceId id, const Rect& rect, const void* bytes, size_t stride) {
    DkSurface* s = Surf(id);
    if (!s || !bytes || rect.w <= 0 || rect.h <= 0 || s->stencil) return;
    if (rect.x < 0 || rect.y < 0 || uint64_t(rect.x) + uint32_t(rect.w) > s->desc.width ||
        uint64_t(rect.y) + uint32_t(rect.h) > s->desc.height)
        return;
    SafePoint();
    ops_++;
    run_valid_ = false;
    const uint32_t bpp = s->format == DkImageFormat_R8_Unorm ? 1 : 4;
    const uint32_t row = uint32_t(rect.w) * bpp;
    const size_t src_stride = stride ? stride : row;

    const uint32_t max_rows = std::max<uint32_t>(1, (stream_slot_bytes_ / 4) / row);
    DkImageView v;
    dkImageViewDefaults(&v, &s->image);
    for (int y0 = 0; y0 < rect.h;) {
        const uint32_t rows = std::min<uint32_t>(max_rows, uint32_t(rect.h - y0));
        DkGpuAddr g = 0;
        auto* dst = static_cast<uint8_t*>(StreamAlloc(rows * row, 256, &g));
        if (!dst) return;
        const auto* src = static_cast<const uint8_t*>(bytes) + size_t(y0) * src_stride;
        if (src_stride == row) {
            std::memcpy(dst, src, size_t(rows) * row);
        } else {
            for (uint32_t r = 0; r < rows; ++r) std::memcpy(dst + size_t(r) * row, src + size_t(r) * src_stride, row);
        }
        BeforeCopyEngine();
        DkCopyBuf cb{g, 0, 0};
        DkImageRect ir{uint32_t(rect.x), uint32_t(rect.y + y0), 0, uint32_t(rect.w), rows, 1};
        dkCmdBufCopyBufferToImage(cmd_, &cb, &v, &ir, 0);
        AfterCopyEngine();
        if (s->depth) zcull_stale_ = true;
        y0 += int(rows);
    }
}

void DekoDevice::UploadLevel(SurfaceId id, int level, int w, int h, const void* rgba) {
    DkSurface* s = Surf(id);
    if (!s || level <= 0 || w <= 0 || h <= 0 || !rgba) return;
    if (level >= s->mip_capacity || (s->format != DkImageFormat_RGBA8_Unorm && s->format != DkImageFormat_R8_Unorm)) {
        return;
    }
    SafePoint();
    ops_++;
    const uint32_t bpp = s->format == DkImageFormat_R8_Unorm ? 1 : 4;
    const uint32_t row = uint32_t(w) * bpp, bytes = row * uint32_t(h);
    DkGpuAddr g = 0;
    auto* dst = static_cast<uint8_t*>(StreamAlloc(bytes, 256, &g));
    if (!dst) {
        s->first_missing = uint8_t(std::min<int>(s->first_missing, level));
        return;
    }
    std::memcpy(dst, rgba, bytes);
    s->last_level.assign(static_cast<const uint8_t*>(rgba), static_cast<const uint8_t*>(rgba) + bytes);
    s->last_level_index = level;
    s->last_level_w = w;
    s->last_level_h = h;
    DkImageView v;
    dkImageViewDefaults(&v, &s->image);
    v.mipLevelOffset = uint8_t(level);
    BeforeCopyEngine();
    DkCopyBuf cb{g, 0, 0};
    DkImageRect ir{0, 0, 0, uint32_t(w), uint32_t(h), 1};
    dkCmdBufCopyBufferToImage(cmd_, &cb, &v, &ir, 0);
    AfterCopyEngine();
}

void DekoDevice::FinishMipChain(SurfaceId id, int authored_levels, bool synthesize_tail) {
    DkSurface* s = Surf(id);
    if (!s || authored_levels <= 0) return;
    SafePoint();
    ops_++;
    int syn = 0;
    if (synthesize_tail && s->last_level_index == authored_levels && !s->last_level.empty()) {
        std::vector<uint8_t> cur = s->last_level, nxt;
        int cw = s->last_level_w, ch = s->last_level_h;
        while ((cw > 1 || ch > 1) && authored_levels + syn + 1 < s->mip_capacity) {
            int nw = 0, nh = 0;
            BoxHalve(cur, cw, ch, nxt, &nw, &nh);
            const int level = authored_levels + syn + 1;
            DkGpuAddr g = 0;
            auto* dst = static_cast<uint8_t*>(StreamAlloc(uint32_t(nxt.size()), 256, &g));
            if (!dst) break;
            std::memcpy(dst, nxt.data(), nxt.size());
            DkImageView v;
            dkImageViewDefaults(&v, &s->image);
            v.mipLevelOffset = uint8_t(level);
            BeforeCopyEngine();
            DkCopyBuf cb{g, 0, 0};
            DkImageRect ir{0, 0, 0, uint32_t(nw), uint32_t(nh), 1};
            dkCmdBufCopyBufferToImage(cmd_, &cb, &v, &ir, 0);
            AfterCopyEngine();
            syn++;
            cur.swap(nxt);
            cw = nw;
            ch = nh;
        }
    }
    s->last_level.clear();
    s->last_level.shrink_to_fit();

    const int delivered = std::min<int>(authored_levels, int(s->first_missing) - 1);
    s->levels = uint8_t(std::max(delivered, 0));
    s->synth_levels = delivered < authored_levels ? 0 : uint8_t(syn);
    s->first_missing = 255;
    WriteImageDescriptor(*s);
}

void DekoDevice::IndexMesh(Mesh& m, const MeshVertex* verts, const SkinBinding* bones, size_t count) {
    const auto indices = MeshIndices(verts, bones, count);

    bool reused = false;
    for (size_t i = 0; i < count; ++i) reused = reused || indices[i] != i;
    if (!reused) return;
    const uint64_t bytes = uint64_t(count) * sizeof(uint32_t);
    if (bytes > UINT32_MAX || !mesh_arena_.Alloc(dev_, uint32_t(bytes), 64, &m.idx_block, &m.idx_off))
        throw std::runtime_error("mesh index allocation failed");
    m.idx_size = Align(uint32_t(bytes), 64);
    std::memcpy(mesh_arena_.blocks[m.idx_block].cpu + m.idx_off, indices.data(), size_t(bytes));
}

uint32_t DekoDevice::CreateMesh(const MeshVertex* verts, size_t count) {
    const uint32_t id = CreateMeshStorage(verts, count);
    if (id) {
        Mesh& m = meshes_[id];
        IndexMesh(m, verts, nullptr, count);
    }
    return id;
}

uint32_t DekoDevice::CreateMeshStorage(const MeshVertex* verts, size_t count) {
    if (!verts || count < 3 || !dev_) return 0;
    SafePoint();
    ops_++;
    const uint32_t bytes = uint32_t(count * sizeof(MeshVertex));
    Mesh m;
    if (!mesh_arena_.Alloc(dev_, bytes, 64, &m.block, &m.off)) return 0;
    m.size = Align(bytes, 64);
    m.count = count;

    std::memcpy(mesh_arena_.blocks[m.block].cpu + m.off, verts, bytes);
    const uint32_t id = next_mesh_++;
    Mesh& made = meshes_[id];
    made = m;
    made.id = id;
    if (mesh_index_.size() <= id) mesh_index_.resize(size_t(id) + 1, nullptr);
    mesh_index_[id] = &made;
    return id;
}

uint32_t DekoDevice::CreateSkinMesh(const MeshVertex* verts, const SkinBinding* bones, size_t count) {
    if (!bones) return 0;
    const uint32_t id = CreateMeshStorage(verts, count);
    if (!id) return 0;
    Mesh& m = meshes_[id];
    const uint32_t bytes = uint32_t(count * sizeof(SkinBinding));
    if (!mesh_arena_.Alloc(dev_, bytes, 64, &m.skin_block, &m.skin_off)) { DestroyMesh(id); return 0; }
    m.skin_size = Align(bytes, 64);
    IndexMesh(m, verts, bones, count);
    std::memcpy(mesh_arena_.blocks[m.skin_block].cpu + m.skin_off, bones, bytes);
    return id;
}

void DekoDevice::DestroyMesh(uint32_t mesh) {
    auto it = meshes_.find(mesh);
    if (it == meshes_.end()) return;
    ops_++;
    if (it->second.idx_size) Retire(Retired{1, it->second.idx_block, it->second.idx_off, it->second.idx_size, 0});
    if (it->second.skin_size) Retire(Retired{1, it->second.skin_block, it->second.skin_off, it->second.skin_size, 0});
    Retire(Retired{1, it->second.block, it->second.off, it->second.size, 0});
    meshes_.erase(it);
    if (mesh < mesh_index_.size()) mesh_index_[mesh] = nullptr;
    run_valid_ = false;
}

void DekoDevice::ClearStencil(SurfaceId stencil, uint8_t value) {
    DkSurface* st = Surf(stencil);
    if (!st || !st->stencil) return;
    SafePoint();
    ops_++;
    run_valid_ = false;
    Before3D();
    want_.target_image = nullptr;
    want_.color = nullptr;
    want_.depth = st;
    ApplyTarget();
    NoteWrite(st);
    const DkScissor full{0, 0, 0x4000, 0x4000};
    dkCmdBufSetScissors(cmd_, 0, &full, 1);
    applied_.scissor = full;
    applied_.scissor_valid = true;
    dkCmdBufClearDepthStencil(cmd_, false, 0.f, 0xFF, value);
}

void DekoDevice::DrawFullscreenTri() {
    static const float kTri[6] = {-1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f};
    DkGpuAddr g = 0;
    void* p = StreamAlloc(sizeof kTri, 32, &g);
    if (!p) return;
    std::memcpy(p, kTri, sizeof kTri);
    DrawArrays(kLayoutPos2, g, sizeof kTri, 0, 3);
}

void DekoDevice::DrawConstQuad(SurfaceId color, SurfaceId stencil, const Rect& scissor, uint32_t rgba, bool write_alpha,
                               const StencilState& st) {
    DkSurface* ss = Surf(stencil);
    DkSurface* c = Surf(color);
    if (!ss || !ss->stencil || scissor.w <= 0 || scissor.h <= 0) return;
    if (color.id != 0 && !c) return;
    SafePoint();
    ops_++;
    run_valid_ = false;
    FullTarget(c, ss, int(ss->desc.width), int(ss->desc.height));
    want_.scissor = DkScissor{uint32_t(std::max(0, scissor.x)), uint32_t(std::max(0, scissor.y)), uint32_t(scissor.w),
                              uint32_t(scissor.h)};
    want_.blend = false;
    want_.depth_test = true;
    want_.depth_func = DkCompareOp_Always;
    want_.depth_write = false;
    const bool cw = c != nullptr;
    want_.color_mask = cw ? (uint32_t(DkColorMask_RGB) | (write_alpha ? uint32_t(DkColorMask_A) : 0u)) : 0u;
    want_.program = constq_;
    const float col[4] = {((rgba >> 0) & 0xFF) / 255.0f, ((rgba >> 8) & 0xFF) / 255.0f, ((rgba >> 16) & 0xFF) / 255.0f,
                          ((rgba >> 24) & 0xFF) / 255.0f};
    SetUFixed<16>(programs_[constq_], dkgen::constq::u_color, col);
    StencilAround(st);
    DrawFullscreenTri();
    want_.stencil = false;
    want_.color_mask = 0xF;
    want_.depth_write = true;
}

bool DekoDevice::MarkStencilWhereOpaque(SurfaceId color, SurfaceId stencil, uint8_t bits, const Rect& scissor) {
    DkSurface* frame = Surf(color);
    DkSurface* ss = Surf(stencil);
    if (!frame || frame->depth || !ss || !ss->stencil) return false;
    SafePoint();
    ops_++;
    run_valid_ = false;
    FullTarget(nullptr, ss, int(ss->desc.width), int(ss->desc.height));
    if (scissor.w > 0 && scissor.h > 0)
        want_.scissor = DkScissor{uint32_t(scissor.x), uint32_t(scissor.y), uint32_t(scissor.w), uint32_t(scissor.h)};
    want_.blend = false;
    want_.depth_test = true;
    want_.depth_func = DkCompareOp_Always;
    want_.depth_write = false;
    want_.color_mask = 0;
    want_.program = opaque_;
    want_.tex[0] = Handle(frame, 0);
    want_.tex_surface[0] = frame;
    want_.stencil = true;
    want_.stencil_func = DkCompareOp_Always;
    want_.stencil_ref = bits;
    want_.stencil_cmp_mask = 0xFF;
    want_.stencil_write_mask = bits;
    want_.stencil_pass = DkStencilOp_Replace;
    want_.stencil_zfail = DkStencilOp_Keep;
    DrawFullscreenTri();
    want_.stencil = false;
    want_.color_mask = 0xF;
    want_.depth_write = true;
    return true;
}

bool DekoDevice::DepthHaze(SurfaceId color, SurfaceId depth, const uint32_t* palette256, bool direct) {
    DkSurface* c = Surf(color);
    DkSurface* z = Surf(depth);
    if (!haze_ || !palette256 || !c || c->depth || c->format != DkImageFormat_RGBA8_Unorm || !z || !DepthStencilFormat(z->format)) return false;
    const uint32_t w = c->desc.width, h = c->desc.height;
    if (z->desc.width != w || z->desc.height != h) return false;

    int last = 255;
    while (last >= 0 && (palette256[last] >> 24) >= 128u) --last;
    if (last < 0) return true;
    if (!direct && (!haze_depth_.id || haze_w_ != w || haze_h_ != h)) {
        if (haze_depth_.id) { DestroySurface(haze_depth_); haze_depth_ = SurfaceId{}; }
        haze_depth_ = CreateSurface(z->desc);
        DkSurface* hz = Surf(haze_depth_);
        if (!hz) { haze_depth_ = SurfaceId{}; return false; }
        hz->desc_id = AllocDescriptor();
        if (!hz->desc_id) { DestroySurface(haze_depth_); haze_depth_ = SurfaceId{}; return false; }
        WriteImageDescriptor(*hz);
        haze_w_ = w;
        haze_h_ = h;
    }
    if (!haze_pal_.id) {
        SurfaceDesc pd;
        pd.width = 256;
        pd.height = 1;
        pd.format = SurfaceFormat::Rgba8;
        pd.renderable = false;
        haze_pal_ = CreateSurface(pd);
        haze_last_valid_ = false;
        if (!haze_pal_.id) return false;
    }
    if (direct && !z->desc_id) {
        z->desc_id = AllocDescriptor();
        if (!z->desc_id) return false;
        WriteImageDescriptor(*z);
    }
    DkSurface* hz = direct ? z : Surf(haze_depth_);
    DkSurface* pal = Surf(haze_pal_);
    if (!hz || !pal || !pal->desc_id) return false;
    if (!haze_last_valid_ || std::memcmp(haze_last_, palette256, sizeof haze_last_) != 0) {
        Upload(haze_pal_, Rect{0, 0, 256, 1}, palette256, 256 * 4);
        std::memcpy(haze_last_, palette256, sizeof haze_last_);
        haze_last_valid_ = true;
    }
    SafePoint();
    ops_++;
    run_valid_ = false;
    if (!direct) {
    BeforeCopyEngine();
    {
        const DkImageRect rect{0, 0, 0, w, h, 1};
        DkImageView sv, dv;
        dkImageViewDefaults(&sv, &z->image);
        dkImageViewDefaults(&dv, &hz->image);
        dkCmdBufCopyImage(cmd_, &sv, &rect, &dv, &rect, 0);
    }
    AfterCopyEngine();
    } else {

        dkCmdBufTiledCacheOp(cmd_, DkTiledCacheOp_Flush);
        NoteSample(z);
    }

    Program& hp = programs_[haze_];
    SetUFixed<4>(hp, dkgen::haze::u_haze_last, &last);
    const Want saved = want_;
    FullTarget(c, nullptr, int(w), int(h));
    want_.blend = true;
    DkBlendState& b = want_.blend_state;
    dkBlendStateDefaults(&b);
    dkBlendStateSetOps(&b, DkBlendOp_Add, DkBlendOp_Add);
    dkBlendStateSetFactors(&b, DkBlendFactor_SrcAlpha, DkBlendFactor_InvSrcAlpha, DkBlendFactor_One, DkBlendFactor_Zero);
    want_.depth_test = false;
    want_.depth_write = false;
    want_.stencil = false;
    want_.color_mask = uint32_t(DkColorMask_RGB);
    want_.program = haze_;
    want_.tex[0] = Handle(hz, 0);
    want_.tex_surface[0] = hz;
    want_.tex[1] = Handle(pal, 0);
    want_.tex_surface[1] = pal;
    for (int i = 2; i < 5; ++i) { want_.tex[i] = Handle(nullptr, 0); want_.tex_surface[i] = nullptr; }
    DrawFullscreenTri();
    want_ = saved;
    return true;
}

bool DekoDevice::ConvertYuv420(SurfaceId dst, SurfaceId y, SurfaceId u, SurfaceId v, SurfaceId rb, SurfaceId g) {
    DkSurface* d = Surf(dst);
    DkSurface* src[5] = {Surf(y), Surf(u), Surf(v), Surf(rb), Surf(g)};
    if (!d || d->depth || d->format != DkImageFormat_RGBA8_Unorm) return false;
    for (DkSurface* s : src)
        if (!s || s->format != DkImageFormat_R8_Unorm || !s->desc_id) return false;
    const uint32_t w = d->desc.width, h = d->desc.height;
    if (src[0]->desc.width != w || src[0]->desc.height != h || src[1]->desc.width * 2 != w || src[1]->desc.height * 2 != h ||
        src[2]->desc.width * 2 != w || src[2]->desc.height * 2 != h || src[3]->desc.width != 256 || src[3]->desc.height != 512 ||
        src[4]->desc.width != 4096 || src[4]->desc.height != 4096)
        return false;
    SafePoint();
    ops_++;
    run_valid_ = false;
    const Want saved = want_;
    FullTarget(d, nullptr, int(w), int(h));
    want_.blend = false;
    want_.depth_test = false;
    want_.depth_write = false;
    want_.stencil = false;
    want_.color_mask = 0xF;
    want_.program = yuv_;
    for (int i = 0; i < 5; ++i) {
        want_.tex[i] = Handle(src[i], 0);
        want_.tex_surface[i] = src[i];
    }
    DrawFullscreenTri();
    want_ = saved;
    return true;
}

void DekoDevice::CopyDraw(DkSurface* src, int sx, int sy, DkSurface* dst, int dx, int dy, int w, int h) {
    if (!src || !dst || w <= 0 || h <= 0 || dst->depth || src->depth) return;

    const Want saved = want_;
    FullTarget(dst, nullptr, int(dst->desc.width), int(dst->desc.height));
    want_.scissor = DkScissor{uint32_t(std::max(0, dx)), uint32_t(std::max(0, dy)), uint32_t(w), uint32_t(h)};
    want_.blend = false;
    want_.depth_test = false;
    want_.depth_write = false;
    want_.stencil = false;
    want_.color_mask = 0xF;
    want_.program = copy_;
    want_.tex[0] = Handle(src, 0);
    want_.tex_surface[0] = src;
    for (int i = 1; i < 5; ++i) { want_.tex[i] = Handle(nullptr, 0); want_.tex_surface[i] = nullptr; }
    const int off[2] = {sx - dx, sy - dy};
    SetUFixed<8>(programs_[copy_], dkgen::copy::u_offset, off);
    DrawFullscreenTri();
    want_ = saved;
}

bool DekoDevice::CopyAttachments(SurfaceId sc, SurfaceId sz, SurfaceId dc, SurfaceId dz) {
    DkSurface* s = Surf(sc); DkSurface* z = Surf(sz);
    DkSurface* d = Surf(dc); DkSurface* q = Surf(dz);
    if (!s || !z || !d || !q || s == d || z == q || s->format != DkImageFormat_RGBA8_Unorm ||
        d->format != s->format || !DepthStencilFormat(z->format) || q->format != z->format) return false;
    const auto w = s->desc.width, h = s->desc.height;
    for (const auto* a : {z, d, q}) if (a->desc.width != w || a->desc.height != h) return false;
    SafePoint();
    ops_++;
    run_valid_ = false;
    BeforeCopyEngine();
    const DkImageRect rect{0, 0, 0, w, h, 1};
    for (const auto pair : {std::make_pair(s, d), std::make_pair(z, q)}) {
        DkImageView sv, dv;
        dkImageViewDefaults(&sv, &pair.first->image);
        dkImageViewDefaults(&dv, &pair.second->image);

        dkCmdBufCopyImage(cmd_, &sv, &rect, &dv, &rect, 0);
    }
    AfterCopyEngine();
    zcull_stale_ = true;
    return true;
}

void DekoDevice::CopyRect(SurfaceId src, const Rect& src_rect, SurfaceId dst, const Rect& dst_rect, uint32_t transform) {
    (void)transform;
    DkSurface* s = Surf(src);
    DkSurface* d = Surf(dst);
    if (!s || !d || !s->desc.renderable || src_rect.w <= 0 || src_rect.h <= 0) return;
    SafePoint();
    ops_++;
    run_valid_ = false;
    CopyDraw(s, src_rect.x, src_rect.y, d, dst_rect.x, dst_rect.y, src_rect.w, src_rect.h);
}

void DekoDevice::Readback(SurfaceId id, const Rect& rect, void* out, size_t stride) {
    DkSurface* s = Surf(id);
    if (!s || !out || rect.w <= 0 || rect.h <= 0) return;
    if (rect.x < 0 || rect.y < 0 || uint64_t(rect.x) + uint32_t(rect.w) > s->desc.width ||
        uint64_t(rect.y) + uint32_t(rect.h) > s->desc.height)
        return;
    SafePoint();
    ops_++;
    run_valid_ = false;
    const uint32_t bpp = s->format == DkImageFormat_R8_Unorm ? 1 : 4;
    const uint32_t row = uint32_t(rect.w) * bpp, bytes = row * uint32_t(rect.h);
    if (readback_size_ < bytes) {
        if (readback_block_) dkMemBlockDestroy(readback_block_);
        readback_size_ = Align(bytes, DK_MEMBLOCK_ALIGNMENT);
        readback_block_ = MakeReadbackBlock(dev_, readback_size_);
        if (!readback_block_) { readback_size_ = 0; return; }
    }
    BeforeCopyEngine();
    DkImageView v;
    dkImageViewDefaults(&v, &s->image);
    DkImageRect ir{uint32_t(rect.x), uint32_t(rect.y), 0, uint32_t(rect.w), uint32_t(rect.h), 1};
    DkCopyBuf cb{dkMemBlockGetGpuAddr(readback_block_), 0, 0};
    dkCmdBufCopyImageToBuffer(cmd_, &v, &ir, &cb, 0);
    AfterCopyEngine();
    WaitIdle();

    const auto* src = ReadbackBytes(readback_block_, bytes);
    const size_t out_stride = stride ? stride : row;
    if (s->stencil) {

        for (int y = 0; y < rect.h; ++y)
            std::memcpy(static_cast<uint8_t*>(out) + size_t(y) * out_stride, src + size_t(y) * row, row);
        return;
    }
    for (int y = 0; y < rect.h; ++y)
        std::memcpy(static_cast<uint8_t*>(out) + size_t(y) * out_stride, src + size_t(y) * row, row);
}

void DekoDevice::Present(const ScanoutDesc& desc) {
    if (desc.filter != PresentationFilter::Surface && desc.filter != PresentationFilter::Nearest &&
        desc.filter != PresentationFilter::Linear)
        throw std::invalid_argument("Graphics presentation filter is unsupported");
    DkSurface* s = Surf(desc.source);
    Rect source = desc.src;
    float u0 = 0.f, u1 = 1.f, v0 = 0.f, v1 = 1.f;
    if (desc.source.id) {
        if (!s || s->depth || !s->desc.width || !s->desc.height || s->desc.width > uint32_t(INT32_MAX) ||
            s->desc.height > uint32_t(INT32_MAX))
            throw std::invalid_argument("Graphics presentation source is unavailable");
        if (!source.x && !source.y && !source.w && !source.h) source = {0, 0, int32_t(s->desc.width), int32_t(s->desc.height)};
        if (source.x < 0 || source.y < 0 || source.w <= 0 || source.h <= 0 || uint32_t(source.x) >= s->desc.width ||
            uint32_t(source.y) >= s->desc.height || uint32_t(source.w) > s->desc.width - uint32_t(source.x) ||
            uint32_t(source.h) > s->desc.height - uint32_t(source.y))
            throw std::invalid_argument("Graphics presentation crop exceeds its source");
        u0 = float(source.x) / float(s->desc.width);
        u1 = float(source.x + source.w) / float(s->desc.width);
        v0 = float(source.y) / float(s->desc.height);
        v1 = float(source.y + source.h) / float(s->desc.height);
        if (!desc.flip_y) std::swap(v0, v1);
    }
    SafePoint();
    ops_++;
    run_valid_ = false;

    const bool lean = s && s->format == DkImageFormat_RGBA8_Unorm;
    const bool fx_wanted = s && desc.flip_y && (desc.merge_line > 0.f || desc.merge_x > 0.f || desc.fxaa);

    if (dkQueueIsInErrorState(queue_)) {
        std::fprintf(stderr, "[deko] FATAL: the GPU queue is in error state (a GPU fault)\n");
        std::fflush(stderr);
        std::abort();
    }
    if (window_w_ > 0 && window_h_ > 0 && (window_w_ != swap_w_ || window_h_ != swap_h_)) {
        if (!MakeSwapchain(window_w_, window_h_)) {
            std::fprintf(stderr, "[deko] FATAL: the swapchain could not be re-made at %dx%d\n", window_w_, window_h_);
            std::fflush(stderr);
            std::abort();
        }
    }
    const int target_w = swap_w_, target_h = swap_h_;
    Submit(true);
    const int slot = dkQueueAcquireImage(queue_, swapchain_);
    DkImage const* target = &swap_images_[slot];

    Before3D();
    want_.target_image = target;
    want_.color = nullptr;
    want_.depth = nullptr;
    ApplyTarget();
    const DkScissor full{0, 0, 0x4000, 0x4000};

    int px0 = 0, py0 = 0, px1 = 0, py1 = 0;
    if (s) {
        const int vw = desc.dst.w > 0 ? desc.dst.w : window_w_;
        const int vh = desc.dst.h > 0 ? desc.dst.h : window_h_;
        px0 = std::clamp(desc.dst.x, 0, target_w);
        py0 = std::clamp(desc.dst.y, 0, target_h);
        px1 = std::clamp(desc.dst.x + vw, 0, target_w);
        py1 = std::clamp(desc.dst.y + vh, 0, target_h);
    }
    if (px1 > px0 && py1 > py0) {
        const DkScissor bars[4] = {
            {0, 0, uint32_t(target_w), uint32_t(py0)},
            {0, uint32_t(py1), uint32_t(target_w), uint32_t(target_h - py1)},
            {0, uint32_t(py0), uint32_t(px0), uint32_t(py1 - py0)},
            {uint32_t(px1), uint32_t(py0), uint32_t(target_w - px1), uint32_t(py1 - py0)},
        };
        for (const DkScissor& b : bars) {
            if (!b.width || !b.height) continue;
            dkCmdBufSetScissors(cmd_, 0, &b, 1);
            dkCmdBufClearColorFloat(cmd_, 0, DkColorMask_RGBA, 0.f, 0.f, 0.f, 1.f);
        }
        applied_.scissor_valid = false;
    } else {
        dkCmdBufSetScissors(cmd_, 0, &full, 1);
        applied_.scissor = full;
        applied_.scissor_valid = true;
        dkCmdBufClearColorFloat(cmd_, 0, DkColorMask_RGBA, 0.f, 0.f, 0.f, 1.f);
    }
    if (s) {
        const int vw = desc.dst.w > 0 ? desc.dst.w : window_w_;
        const int vh = desc.dst.h > 0 ? desc.dst.h : window_h_;
        want_.viewport = DkViewport{float(desc.dst.x), float(desc.dst.y), float(vw), float(vh), 0.f, 1.f};
        want_.scissor = full;
        want_.blend = false;
        want_.depth_test = false;
        want_.depth_write = false;
        want_.stencil = false;
        want_.color_mask = 0xF;

        const uint32_t sampler = desc.filter == PresentationFilter::Linear ? SamplerIndex(kGlLinear, kGlLinear, false, false, 1)
                                                                            : SamplerIndex(kGlNearest, kGlNearest, false, false, 1);
        want_.tex[0] = Handle(s, sampler);
        want_.tex_surface[0] = s;
        if (fx_wanted) {
            want_.program = scanout_fx_;
            Program& p = programs_[scanout_fx_];
            const float iw = 1.f / float(s->desc.width), ih = 1.f / float(s->desc.height);
            const float fx[4] = {iw, ih, desc.merge_line > 0.f ? desc.merge_line * ih : 0.f, desc.fxaa ? 1.f : 0.f};
            const float crop[4] = {std::min(u0, u1) + 0.5f * iw, std::min(v0, v1) + 0.5f * ih,
                                   std::max(u0, u1) - 0.5f * iw, std::max(v0, v1) - 0.5f * ih};
            const float merge_x = desc.merge_x * iw;
            SetUFixed<4>(p, dkgen::scanout_fx::u_merge_x, &merge_x);
            SetUFixed<16>(p, dkgen::scanout_fx::u_fx, fx);
            SetUFixed<16>(p, dkgen::scanout_fx::u_crop, crop);
        } else want_.program = lean ? scanout_lean_ : scanout_;
        const float quad[]{
            -1.f, -1.f, u0, v1, 1.f, -1.f, u1, v1, -1.f, 1.f, u0, v0,
             1.f, -1.f, u1, v1, 1.f,  1.f, u1, v0, -1.f, 1.f, u0, v0,
        };
        DkGpuAddr g = 0;
        void* p = StreamAlloc(sizeof quad, 32, &g);
        if (p) {
            std::memcpy(p, quad, sizeof quad);
            if (lean) {
                DkImageView v;
                dkImageViewDefaults(&v, &s->image);
                v.swizzle[3] = DkImageSwizzle_One;
                DkImageDescriptor image;
                dkImageDescriptorInitialize(&image, &v, false, false);
                dkCmdBufPushData(cmd_, image_desc_gpu_ + DkGpuAddr(present_desc_[cur_]) * sizeof image, &image, sizeof image);
                descriptors_dirty_ = true;
                want_.tex[0] = dkMakeTextureHandle(present_desc_[cur_], sampler);
            }
            DrawArrays(kLayoutScan, g, sizeof quad, 0, 6);
        }
        want_.depth_test = true;
        want_.depth_write = true;
    }

    EndSlot();
    dkQueuePresentImage(queue_, swapchain_, slot);
    cur_ = (cur_ + 1) % slots_n_;
    BeginSlot();
    want_.target_image = nullptr;
}

void DekoDevice::SetWindowSize(int w, int h) {
    window_w_ = w;
    window_h_ = h;
}

void DekoDevice::BeginFrame() { SafePoint(); }
void DekoDevice::EndFrame() {}

}
}
