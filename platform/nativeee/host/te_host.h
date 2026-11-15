#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace te {

uint8_t* GuestMemory();
std::size_t GuestMemorySize();

inline uint8_t* GuestPointer(uint32_t address, std::size_t size) {
    uint8_t* mem = GuestMemory();
    if (!mem || uint64_t(address) + size > GuestMemorySize()) return nullptr;
    return mem + address;
}

const char* GuestString(uint32_t address, std::size_t max);

[[noreturn]] void RequestExit(int status);

}

extern "C" int te_call_void_callback(uint32_t index);

namespace te {

struct Settings {
    std::string iso_path;
    std::string mc_dir;

    std::string language_name;
    int32_t language = -1;
    std::string settings_path;
    bool kbm = true;
    bool sdl_input = true;
    bool pad_connected = true;
};
const Settings& GetSettings();
void SetSettings(const Settings& settings);

}
