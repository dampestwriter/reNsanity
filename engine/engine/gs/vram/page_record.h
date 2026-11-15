#pragma once

#include <cstdint>

namespace ps2r {
namespace gs {

struct PageRecord {
    enum class Author : uint8_t { Cold = 0, Cpu = 1, GpuTarget = 2 };
    enum class State : uint8_t { Clean = 0, Pending = 1, Encoding = 2, Applying = 3 };

    static constexpr uint32_t kPageBytes = 8192;
    static constexpr uint32_t kPageCount = 512;
    static constexpr uint32_t kSubPageBytes = 128;
    static constexpr uint32_t kSubPagesPerPage = kPageBytes / kSubPageBytes;

    Author author = Author::Cold;
    uint32_t author_fbp = 0;
    uint64_t upload_epoch = 0;
    uint64_t shadow_epoch = 0;
    State state = State::Clean;
    uint64_t write_bits = 0;

    uint64_t view_gen = 0;
    int32_t dirty_y0 = 0;
    int32_t dirty_y1 = 0;
    bool stale = false;
    bool reader_flag = false;

    bool BandEmpty() const { return dirty_y0 >= dirty_y1; }
    void ClearBand();
};

}
}
