#pragma once

#include <array>
#include <cstdint>

namespace title {

class DecalMath {
public:
    using Vector = std::array<float, 4>;
    static constexpr uint32_t kSameChunk = 0x1C0;
    static constexpr uint32_t kOtherChunk = 0x360;
    static constexpr uint32_t kDataQuadwords = 256;

    struct Decal {
        Vector place{};
        std::array<int32_t, 4> frame{};
        std::array<int32_t, 4> more{};
        uint16_t variant = 0;
    };
    struct Look {
        std::array<uint32_t, 4> place{};
        std::array<uint32_t, 4> colour{};
        std::array<uint32_t, 4> offsets{};
        std::array<uint32_t, 4> more{};
    };

    static Look Age(const Vector* data, uint32_t program, const Decal& decal);
};

}
