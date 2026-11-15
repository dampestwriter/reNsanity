#pragma once

#include <cstdint>
#include <functional>

struct ParkRecord {
    const char* reason = nullptr;
    uint32_t pc = 0;
    const char* detail = nullptr;
};

class GuestPark {
public:
    using Sink = std::function<void(const ParkRecord&)>;

    [[noreturn]] static void Forever(const char* reason, uint32_t pc = 0, const char* detail = nullptr);

    static void SetSink(Sink sink);
    static const ParkRecord& Last();
    static uint32_t Count();
};
