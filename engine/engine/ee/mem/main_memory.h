#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class MainMemory {
public:
    static constexpr uint32_t kSize = 32u * 1024u * 1024u;
    static constexpr uint32_t kPhysMask = 0x1FFFFFFFu;

    MainMemory();

    static uint32_t Phys(uint32_t addr) { return addr & kPhysMask; }

    uint8_t* Pointer(uint32_t phys, size_t size);
    const uint8_t* Pointer(uint32_t phys, size_t size) const;

    ~MainMemory();
    MainMemory(const MainMemory&) = delete;
    MainMemory& operator=(const MainMemory&) = delete;
    uint8_t* Data() { return bytes_; }
    const uint8_t* Data() const { return bytes_; }
    size_t Size() const { return kSize; }


private:
    uint8_t* bytes_;
};
