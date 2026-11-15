#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <memory>
#include <vector>
class IsoReadCache;

class IsoFs {
public:
    static constexpr uint32_t kSectorSize = 2048;

    struct Entry {
        uint32_t lba = 0;
        uint32_t size = 0;
        bool directory = false;
        uint8_t date[7] = {};
        std::string name;
    };

    IsoFs();
    ~IsoFs();
    IsoFs(const IsoFs&) = delete;
    IsoFs& operator=(const IsoFs&) = delete;

    bool Open(const std::string& path, std::string* error = nullptr);
    bool IsOpen() const { return fd_ >= 0; }
    void Close();

    bool Find(const std::string& path, Entry& out) const;

    Entry Root() const { return root_; }

    uint32_t ReadSectors(uint32_t lba, uint32_t count, uint8_t* dst) const;

    size_t ReadBytes(uint64_t offset, size_t size, uint8_t* dst) const;

    void Prefetch(uint64_t offset, size_t size) const;
    size_t ReadDeferredBytes(uint64_t offset, size_t size, uint8_t* dst) const;

    uint32_t RootLba() const { return root_.lba; }
    uint32_t RootSize() const { return root_.size; }
    uint32_t VolumeSectors() const { return volume_sectors_; }

    uint64_t ImageBytes() const { return image_bytes_; }

    bool RawSectors() const { return sector_stride_ != kSectorSize; }
    uint32_t SectorStride() const { return sector_stride_; }
    std::string VolumeId() const { return volume_id_; }

private:
    size_t ReadUncached(uint64_t offset, size_t size, uint8_t* dst) const;
    std::unique_ptr<IsoReadCache> cache_;
    bool FindInDirectory(const Entry& dir, const std::string& component, Entry& out) const;
    static bool IdentifierMatches(const std::string& recorded, const std::string& wanted);

    int fd_ = -1;
    uint64_t image_bytes_ = 0;
    uint64_t file_bytes_ = 0;
    uint32_t sector_stride_ = kSectorSize;
    uint32_t user_offset_ = 0;
    uint32_t volume_sectors_ = 0;
    Entry root_;
    std::string volume_id_;
};
