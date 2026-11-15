#pragma once

#include <cstdint>

namespace ps2r {
namespace gs {

enum class Psm : uint8_t {
    CT32  = 0x00,
    CT24  = 0x01,
    CT16  = 0x02,
    CT16S = 0x0A,
    T8    = 0x13,
    T4    = 0x14,
    T8H   = 0x1B,
    T4HL  = 0x24,
    T4HH  = 0x2C,
    Z32   = 0x30,
    Z24   = 0x31,
    Z16   = 0x32,
    Z16S  = 0x3A,
};

constexpr int kPsmCount = 13;

enum class PsmClass : uint8_t {
    Bits32,
    Bits16,
    Bits8,
    Bits4,
};

struct PsmDescriptor {
    Psm psm;
    const char* name;
    PsmClass cls;
    uint8_t  storage_bits;
    uint8_t  index_bits;
    uint8_t  index_shift;
    uint16_t page_w, page_h;
    uint8_t  block_w, block_h;
    uint8_t  column_w, column_h;
    bool     is_z;
    bool     is_indexed;
    bool     shares_ct32_word;
};

}
}
