#include "engine/ee/mem/main_memory.h"

#include <algorithm>

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
__attribute__((visibility("hidden"), aligned(4096))) uint8_t te_ee_ram[MainMemory::kSize];
}

namespace {
bool g_ram_taken = false;
}

MainMemory::MainMemory() : bytes_(te_ee_ram) {
    if (g_ram_taken) {
        std::fprintf(stderr, "MainMemory: a second instance on the Switch (the RAM is one static array)\n");
        std::abort();
    }
    g_ram_taken = true;
    std::memset(bytes_, 0, kSize);
}

MainMemory::~MainMemory() { g_ram_taken = false; }
