#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

class IsoFs;
namespace ps2r::ipu { class GuestMemory; }

namespace title {

class Files {
public:
    static constexpr size_t kMaxFiles = 32, kMaxPath = 1024;

    Files(ps2r::ipu::GuestMemory& memory, const IsoFs& disc);
    int32_t Open(const std::string& path, int32_t flags);
    int32_t Close(int32_t handle);
    int32_t Read(int32_t handle, uint32_t destination, int32_t bytes);
    int32_t Seek(int32_t handle, int32_t offset, uint32_t whence);
    int32_t Write(int32_t handle);
    void Reset();

private:
    struct File { uint32_t location = 0, size = 0, position = 0; };
    struct Slot { File file; uint64_t generation = 0; };
    Slot* Find(int32_t handle);
    static bool DiscPath(const std::string& path);
    ps2r::ipu::GuestMemory& memory_;
    const IsoFs& disc_;
    std::array<Slot, kMaxFiles> slots_{};
    uint64_t next_generation_ = 1;
};

}
