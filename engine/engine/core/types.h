#pragma once

#include <cstdint>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s8 = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;

struct u128 {
    u64 lo;
    u64 hi;
};

struct s128 {
    s64 lo;
    s64 hi;
};

static_assert(sizeof(u128) == 16 && sizeof(s128) == 16, "128-bit halves");
