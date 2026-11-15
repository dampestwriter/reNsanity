#pragma once

#include <cstdint>

class MmioHandler {
public:
    virtual ~MmioHandler() = default;
    virtual uint64_t Read(uint32_t addr, unsigned size) = 0;
    virtual void Write(uint32_t addr, unsigned size, uint64_t value) = 0;
};
