#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class MainMemory;
class Scratchpad;

class AddressMap {
public:
    static constexpr uint32_t kHwWindowBase = 0x10000000u;
    static constexpr uint32_t kHwWindowEnd = 0x20000000u;
    static constexpr uint32_t kIoBase = 0x10000000u;
    static constexpr uint32_t kIoSize = 0x10000u;
    static constexpr uint32_t kBiosBase = 0x1FC00000u;
    static constexpr uint32_t kBiosSize = 0x400000u;

    struct Region {
        uint32_t base;
        uint32_t size;
        uint8_t* backing;
        const char* name;
    };

    AddressMap(MainMemory& ram, Scratchpad& spad);

    void RegisterHwWindow(uint32_t virt_base, uint32_t size, uint8_t* backing, const char* name);

    void RegisterExtraRegion(uint32_t phys_base, uint32_t size, uint8_t* backing, const char* name);

    uint8_t* Translate(uint32_t addr, size_t size);

    const std::vector<Region>& HwWindows() const { return hw_windows_; }
    const std::vector<Region>& ExtraRegions() const { return extra_regions_; }
    uint8_t* IoImage() { return io_image_.data(); }
    uint8_t* Bios() { return bios_.data(); }

private:
    static uint8_t* InRegion(const Region& r, uint32_t addr, size_t size);

    MainMemory& ram_;
    Scratchpad& spad_;
    std::vector<uint8_t> io_image_;
    std::vector<uint8_t> bios_;
    std::vector<Region> hw_windows_;
    std::vector<Region> extra_regions_;
};
