#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/ee/mem/quad_word.h"

class AddressMap;
class MainMemory;
class MmioRouter;

class FastMem {
public:
    FastMem(MainMemory& ram, AddressMap& map, MmioRouter& mmio);

    template <typename T>
    T Read(uint32_t addr);
    template <typename T>
    void Write(uint32_t addr, T value);
    void ReadQuad(uint32_t addr, QuadWord& out);
    void WriteQuad(uint32_t addr, const QuadWord& value);

    template <typename T>
    T ReadSlow(uint32_t addr);
    template <typename T>
    void WriteSlow(uint32_t addr, T value);
    void ReadQuadSlow(uint32_t addr, QuadWord& out);
    void WriteQuadSlow(uint32_t addr, const QuadWord& value);

    static bool FastPath(uint32_t addr, size_t width);

    AddressMap& Map() { return map_; }
    MmioRouter& Mmio() { return mmio_; }
    MainMemory& Ram() { return ram_; }

    using WriteObserver = void (*)(void* user, uint32_t addr, unsigned width, uint64_t old_bits, uint64_t new_bits);
    static void SetWriteObserver(WriteObserver fn, void* user, uint32_t lo, uint32_t hi);
    static bool HasWriteObserver() { return observer_ != nullptr; }

private:

    void ObserveWrite(uint32_t phys, uint32_t addr, const void* value, unsigned width);

    MainMemory& ram_;
    AddressMap& map_;
    MmioRouter& mmio_;
    static WriteObserver observer_;
    static void* observer_user_;
    static uint32_t observe_lo_, observe_hi_;
};

#include "fast_mem.inl"
