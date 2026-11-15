#pragma once
namespace title {
inline __attribute__((always_inline)) float FencedFloat(float value) {
    asm("" : "+w"(value));
    return value;
}
}
