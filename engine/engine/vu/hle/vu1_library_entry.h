#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/vu/hle/vu1_family_signature.h"
#include "engine/vu/hle/vu1_hle_contract.h"

constexpr uint32_t kVu1AnyEntry = UINT32_MAX;

struct Vu1LibraryEntry {
    uint64_t image_hash;
    uint64_t variant_hash;
    const char* name;
    uint32_t start_pc;
    Vu1HleFn fn;
    const uint32_t* oracle_case_ids;
    size_t oracle_case_count;

    uint64_t layout_hash = 0;
};

struct Vu1LoadedProgram {
    uint64_t code_hash;
    uint32_t size_bytes;
    const char* name;
};

struct Vu1ProgramLibrary {
    Vu1FamilySignature family;
    const Vu1LibraryEntry* entries;
    size_t entry_count;

    const Vu1LoadedProgram* programs = nullptr;
    size_t program_count = 0;
};
