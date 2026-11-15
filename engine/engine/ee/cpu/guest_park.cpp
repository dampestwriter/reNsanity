#include "engine/ee/cpu/guest_park.h"

#include <condition_variable>
#include <mutex>

namespace {
ParkRecord g_last;
uint32_t g_count = 0;
GuestPark::Sink g_sink;
std::mutex g_mutex;
std::condition_variable g_never;
}

const ParkRecord& GuestPark::Last() { return g_last; }
uint32_t GuestPark::Count() { return g_count; }
