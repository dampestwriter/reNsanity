#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "game.h"
#include "wasm-rt.h"

class MainMemory;

struct w2c_env {
    w2c_game* instance = nullptr;
    wasm_rt_memory_t memory{};
    MainMemory* main_memory = nullptr;
};

namespace te {

uint32_t rd32(const uint8_t* p);
const char* TypeName(const char* type_id);

void BindMainMemory(w2c_env& env, MainMemory& ram);

struct RetailImage {
    uint32_t start = 0x2D9D90;
    uint32_t end = 0x3DB200;
    uint32_t bss_start = 0x30A480;
    uint32_t code_start = 0x00100000;
    uint32_t code_end = 0x002D9D90;
};

bool LoadRetailImage(w2c_env& env, const std::vector<uint8_t>& elf, const RetailImage& image, std::string* error);

bool ReadElfFromIso(const std::string& iso_path, const std::string& path_in_iso, std::vector<uint8_t>& out, std::string* error);

void PatchRetailCodePointers(w2c_env& env);

struct VtableThunkReport {
    std::vector<std::string> pool_exhausted_signatures;
};

VtableThunkReport PatchVtableThunks(w2c_env& env);

}

extern "C" {
wasm_rt_memory_t* w2c_env_memory(struct w2c_env* env);
struct w2c_game* te_env_instance(struct w2c_env* env);
wasm_rt_memory_t* te_env_memory(struct w2c_env* env);
}
