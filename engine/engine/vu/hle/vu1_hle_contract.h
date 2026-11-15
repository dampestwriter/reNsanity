#pragma once

#include <cstdint>
#include <vector>

#include "engine/ee/cpu/cpu_state.h"

constexpr uint32_t kVu1HleMicroSize = 16 * 1024;
constexpr uint32_t kVu1HleDataSize = 16 * 1024;

struct Vu1ImageInfo;

struct Vu1HleInput {
    const uint8_t* micro;
    const uint8_t* data;
    uint32_t start_pc;
    uint32_t top, tops, itop, itops;
    const VUregs* regs;
    uint64_t image_hash;

    const Vu1ImageInfo* info = nullptr;
};

struct Vu1HleOutput {

    std::vector<std::vector<uint8_t>> kicks;

    struct DataWrite {
        uint32_t addr;
        std::vector<uint8_t> bytes;
    };
    std::vector<DataWrite> data_writes;

    bool has_regs = false;
    VUregs regs{};
};

using Vu1HleFn = bool (*)(const Vu1HleInput&, Vu1HleOutput&);

struct Vu1LoadedAt {
    uint16_t pc;
    uint16_t program;
};

struct Vu1ImageInfo {
    uint64_t image_hash;
    uint64_t prefix_hash;
    uint64_t variant_hash;
    uint32_t extent;
    bool prefix_is_family;
    std::vector<uint16_t> dispatch_chain;
    bool dispatch_valid;

    std::vector<Vu1LoadedAt> layout;
    uint32_t loaded_end;
    uint64_t layout_hash;
};
