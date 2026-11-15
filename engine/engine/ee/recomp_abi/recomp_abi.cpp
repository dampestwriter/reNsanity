#include "engine/ee/recomp_abi/recomp_abi.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <fstream>
#include <map>
#include <mutex>
#include <tuple>

#include "engine/ee/recomp_abi/host_app/cdvd_stream.h"
#include "engine/ee/recomp_abi/host_app/vu.h"

namespace {
FastMem* g_bound_memory = nullptr;
}

FastMem& RecompAbi::Memory() {
    assert(g_bound_memory && "RecompAbi::BindMemory must run before generated code");
    return *g_bound_memory;
}

std::ofstream g_logFile;

bool g_call_census_enabled = false;

std::atomic<uint64_t> g_alloc_trace_seq{0};
std::mutex g_alloc_live_mutex;
std::map<uint32_t, std::tuple<uint32_t, uint32_t, uint32_t, uint64_t>> g_alloc_live;
uint32_t g_pose_wb_count = 0;
std::atomic<uint32_t> g_ring_watch_addr{0};

Vu0Macro g_vu0;
Vu1Window g_vu1;

#include "engine/ee/recomp_abi/host_app/hle_heap.h"
