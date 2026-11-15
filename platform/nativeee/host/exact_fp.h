#pragma once
#include <array>
#include <cfenv>
#include <cstdint>
#include <cstring>

namespace te {

struct ExactFp {
    std::array<uint8_t, 64> bytes{};
    bool valid = false;
    static ExactFp Read() {
        ExactFp f;
        uint64_t control;
        asm volatile("mrs %0, fpcr" : "=r"(control));
        std::memcpy(f.bytes.data(), &control, sizeof control);
        f.valid = true;
        return f;
    }
    bool operator==(const ExactFp& b) const { return valid && b.valid && bytes == b.bytes; }
};
}
