#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace te::gfx::texpack {

enum Format : uint32_t { kNone = 0, kRgba8 = 1, kBc1 = 2, kBc3 = 3, kBc7 = 4 };

struct Replacement {
    int width = 0, height = 0;
    Format format = kNone;
    std::vector<uint8_t> bytes;
    std::vector<size_t> level_offset, level_size;
};

bool Available();

bool Enabled();

void SetEnabled(bool on);

uint64_t Key(const uint32_t* rgba, int width, int height);
std::string Name(uint64_t key, int width, int height);

bool Find(uint64_t key, int width, int height, Replacement* out, std::string* why);

bool Has(uint64_t key, int width, int height);

void Async(std::function<void()> job);

void Hold();
void Unhold();


void Released(uint64_t bytes);


bool ParseDds(const uint8_t* data, size_t size, Replacement* out, std::string* error);

size_t LevelBytes(Format format, int w, int h);

bool SmallProcess();

}
