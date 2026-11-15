#pragma once

#include <cstdint>
#include <cstring>

struct QwPtr {
    uint8_t* mem;

    uint32_t W(uint32_t qw, int lane) const {
        uint32_t v;
        std::memcpy(&v, mem + ((qw & 0x3FF) * 16) + lane * 4, 4);
        return v;
    }
    void SetW(uint32_t qw, int lane, uint32_t v) { std::memcpy(mem + ((qw & 0x3FF) * 16) + lane * 4, &v, 4); }
    float F(uint32_t qw, int lane) const {
        float f;
        std::memcpy(&f, mem + ((qw & 0x3FF) * 16) + lane * 4, 4);
        return f;
    }
};
