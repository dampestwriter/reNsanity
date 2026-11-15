#pragma once

#include <cstdint>

namespace ps2r {
namespace gs {

enum class JobKind : uint32_t {
    DrawBatch = 0,
    SetScissor = 1,
    SetBlendMode = 2,
    SetDepthTest = 3,
    SetFrontBuffer = 4,
    SetWindow = 5,
    SetClearColor = 6,
    VSync = 7,
    InvalidateTextures = 8,
    UploadVRAMTexture = 9,
    SyncAllRTsToVRAM = 10,
};

constexpr uint32_t kJobKindCount = 11;

#pragma pack(push, 1)
struct JobRec {
    uint32_t type;
    uint32_t seq;
    uint32_t src_frame;
    uint32_t arg1, arg2, arg3, arg4;
    uint64_t vram_epoch;
    uint32_t payload_bytes;
    uint32_t payload_incl;
    uint64_t payload_hash;
};
#pragma pack(pop)

static_assert(sizeof(JobRec) == 52, "JobRec is 52 bytes on disk");

}
}
