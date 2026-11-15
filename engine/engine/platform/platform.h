#pragma once

#include <cstdint>

namespace ps2r::platform {

struct InputState  { uint16_t wire_buttons = 0xFFFF; uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80; };
struct AudioSpec   { int sample_rate = 48000; int channels = 2; int frames = 800; };

enum class ThreadRole : uint8_t { Interactive = 0, Pacer, Game, Graphics, Worker, AudioMixer, AudioCallback };

}
