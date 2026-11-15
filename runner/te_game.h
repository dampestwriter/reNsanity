#pragma once
#include <cstdint>
#include <string>

#include "te_fiber.h"

struct w2c_game;

namespace te::game {

enum class Reason : int { Running = 0, Exit, Trap, Returned };

struct Fiber {
    te_fiber_ctx host{};
    te_fiber_ctx game{};
    uint8_t* stack = nullptr;
    size_t stack_size = 0;
    bool on_game = false;
    Reason reason = Reason::Running;
    int code = 0;
    w2c_game* instance = nullptr;
};

constexpr uint64_t kStackHint = 0x310000000000ull;
constexpr size_t kStackSize = 32u << 20;

Fiber& Get();

bool Prepare(w2c_game* instance, std::string* err);

Reason Resume();

void Yield(Reason reason, int code = 0);

void ExitFromGame(int status);

}
