#include <cstdio>
#include <cstring>
#include <string>

#include "engine/ipu/hle/guest_memory.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/files.h"
#include "ps2e_seam.h"
#include "te_services.h"

namespace te {

struct Files::Memory final : ps2r::ipu::GuestMemory {
    uint8_t* Translate(uint32_t addr, size_t len) override { return GuestPointer(addr, len); }
    uint32_t Read32(uint32_t addr) override {
        uint32_t v = 0;
        if (uint8_t* p = GuestPointer(addr, 4)) std::memcpy(&v, p, 4);
        return v;
    }
    void Write32(uint32_t addr, uint32_t value) override {
        if (uint8_t* p = GuestPointer(addr, 4)) std::memcpy(p, &value, 4);
    }
};

namespace {

std::string DevicePath(const char* path) {
    constexpr size_t kMax = 256;
    std::string out = "cdrom0:\\";
    for (const char* c = path; c && *c != '\0' && out.size() < kMax - 3; ++c) {
        char ch = *c;
        if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
        else if (ch == '/') ch = '\\';
        out.push_back(ch);
    }
    out += ";1";
    return out;
}
}

Files::Files(Disc& disc) : memory_(new Memory) {
    disc.Initialise();
    core_.reset(new title::Files(*memory_, disc.Iso()));
}
Files::~Files() = default;

void Files::Reset() { core_->Reset(); }

int32_t Files::Open(const char* path, int32_t flags) {
    if (!path) return -22;
    return core_->Open(DevicePath(path), flags);
}

int32_t Files::Close(int32_t file) { return core_->Close(file); }

int32_t Files::Read(int32_t file, uint32_t buffer, int32_t size) { return core_->Read(file, buffer, size); }

int32_t Files::Write(int32_t file, uint32_t, int32_t) { return core_->Write(file); }

int32_t Files::Seek(int32_t file, int32_t offset, int32_t whence) {
    return core_->Seek(file, offset, static_cast<uint32_t>(whence));
}

}

extern "C" {


ps2e_s32 ps2e_files_open(ps2e_u32 path, ps2e_s32 flags) {
    const char* s = te::GuestString(path, 1024);
    if (!s) return -22;
    return te::Services::Get().files().Open(s, flags);
}

ps2e_s32 ps2e_files_close(ps2e_s32 file) { return te::Services::Get().files().Close(file); }

ps2e_s32 ps2e_files_read(ps2e_s32 file, ps2e_u32 buffer, ps2e_s32 size) {
    return te::Services::Get().files().Read(file, buffer, size);
}

ps2e_s32 ps2e_files_write(ps2e_s32 file, ps2e_u32 buffer, ps2e_s32 size) {
    return te::Services::Get().files().Write(file, buffer, size);
}

ps2e_s32 ps2e_files_seek(ps2e_s32 file, ps2e_s32 offset, ps2e_s32 whence) {
    return te::Services::Get().files().Seek(file, offset, whence);
}

}
