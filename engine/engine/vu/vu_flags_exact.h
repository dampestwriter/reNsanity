#pragma once

#include <cstdint>
#include <cstring>

namespace vuexact {

inline bool Plain(float v) {
    uint32_t b;
    std::memcpy(&b, &v, 4);
    const uint32_t e = b & 0x7f800000u;
    return e != 0x7f800000u && (e != 0 || (b & 0x007fffffu) == 0);
}

class Scope {
public:
    Scope() {
        asm volatile("mrs %0, fpcr" : "=r"(fpcr_) :: "memory");
        asm volatile("mrs %0, fpsr" : "=r"(fpsr_) :: "memory");
        asm volatile("msr fpcr, %0" :: "r"(fpcr_ | (uint64_t(1) << 24)) : "memory");
        asm volatile("msr fpsr, %0" :: "r"(uint64_t(0)) : "memory");
    }

    bool Clean() const {
        uint64_t s;
        asm volatile("mrs %0, fpsr" : "=r"(s) :: "memory");
        return (s & 0x8fu) == 0;
    }
    ~Scope() {
        asm volatile("msr fpcr, %0" :: "r"(fpcr_) : "memory");
        asm volatile("msr fpsr, %0" :: "r"(fpsr_) : "memory");
    }
private:
    uint64_t fpcr_ = 0, fpsr_ = 0;
};

}
