#include "games/slus_20909_crash_twinsanity/hle/stream/files.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "engine/cdvd/iso_fs.h"
#include "engine/ipu/hle/guest_memory.h"

namespace title {

Files::Files(ps2r::ipu::GuestMemory& memory, const IsoFs& disc) : memory_(memory), disc_(disc) {}

bool Files::DiscPath(const std::string& path) {
    if (path.empty() || path.size() >= kMaxPath || path.find('\0') != std::string::npos) return false;
    const auto colon = path.find(':');

    return colon == std::string::npos || path.compare(0, colon + 1, "cdrom0:") == 0;
}

Files::Slot* Files::Find(int32_t handle) {
    if (handle < 0 || static_cast<size_t>(handle) >= slots_.size()) return nullptr;
    auto& slot = slots_[handle];
    return slot.generation ? &slot : nullptr;
}

int32_t Files::Open(const std::string& path, int32_t flags) {
    if (!DiscPath(path)) return -19;
    if (flags != 1) return (flags & 2) || (flags & 0x700) ? -30 : -22;
    IsoFs::Entry entry;
    if (!disc_.Find(path, entry) || entry.directory) return -2;
    const uint64_t end = uint64_t(entry.lba) * IsoFs::kSectorSize + entry.size;
    if (end > disc_.ImageBytes() || entry.size > uint32_t(std::numeric_limits<int32_t>::max()))
        return -5;
    for (uint32_t handle = 0; handle < slots_.size(); ++handle) {
        auto& slot = slots_[handle];
        if (slot.generation) continue;
        if (next_generation_ == std::numeric_limits<uint64_t>::max()) return -24;
        slot.file = {entry.lba, entry.size, 0};
        slot.generation = next_generation_++;
        return static_cast<int32_t>(handle);
    }
    return -24;
}

int32_t Files::Close(int32_t handle) {
    auto* slot = Find(handle);
    if (!slot) return -9;
    *slot = {};
    return 0;
}

int32_t Files::Read(int32_t handle, uint32_t destination, int32_t bytes) {
    auto* slot = Find(handle);
    if (!slot) return -9;
    if (bytes < 0) return -22;
    auto& file = slot->file;
    const uint32_t want = std::min(static_cast<uint32_t>(bytes), file.size - file.position);
    if (!want) return 0;
    auto* output = destination ? memory_.Translate(destination, want) : nullptr;
    if (!output) return -14;

    std::vector<uint8_t> data(want);
    const uint64_t offset = uint64_t(file.location) * IsoFs::kSectorSize + file.position;
    if (disc_.ReadBytes(offset, want, data.data()) != want) return -5;
    std::memcpy(output, data.data(), want);
    file.position += want;
    return static_cast<int32_t>(want);
}

int32_t Files::Seek(int32_t handle, int32_t offset, uint32_t whence) {
    auto* slot = Find(handle);
    if (!slot) return -9;
    const auto& file = slot->file;
    if (whence > 2) return -22;
    const int64_t base = whence == 0 ? 0 : whence == 1 ? file.position : file.size;
    const int64_t position = base + offset;
    if (position < 0 || position > file.size) return -22;
    slot->file.position = static_cast<uint32_t>(position);
    return static_cast<int32_t>(position);
}

int32_t Files::Write(int32_t handle) {
    return Find(handle) ? -30 : -9;
}

void Files::Reset() {
    for (auto& slot : slots_) slot = {};
}

}
