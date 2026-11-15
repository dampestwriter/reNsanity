#pragma once

#include <cstddef>
#include <cstdint>

namespace ps2r::ipu {

class GuestMemory {
public:
    virtual ~GuestMemory() = default;

    virtual uint8_t* Translate(uint32_t addr, size_t len) = 0;

    virtual uint32_t Read32(uint32_t addr) = 0;
    virtual void Write32(uint32_t addr, uint32_t value) = 0;
};

}
