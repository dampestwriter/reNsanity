#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct SwsContext;

namespace ps2r::ipu {

inline size_t Yuv420Bytes(int w, int h) { return size_t(w) * size_t(h) + 2 * (size_t(w / 2) * size_t(h / 2)); }

class YuvToRgba {
public:
    YuvToRgba() = default;
    ~YuvToRgba();
    YuvToRgba(const YuvToRgba&) = delete;
    YuvToRgba& operator=(const YuvToRgba&) = delete;

    bool Convert(const uint8_t* yuv, int w, int h, uint8_t* rgba);

private:
    SwsContext* ctx_ = nullptr;
    int w_ = 0, h_ = 0;
};

struct YuvRgbaTables {
    static constexpr int kRbWidth = 256, kRbHeight = 512;
    static constexpr int kGWidth = 4096, kGHeight = 4096;
    int width = 0, height = 0;
    std::vector<uint8_t> rb;
    std::vector<uint8_t> g;
    double probe_ms = 0.0;
    uint64_t pixels = 0;

    static size_t GIndex(int y, int u, int v) { return size_t((v << 4) | (u >> 4)) * kGWidth + size_t(y + ((u & 15) << 8)); }

    bool SameAs(const YuvRgbaTables& o) const { return rb == o.rb && g == o.g; }
};

std::shared_ptr<const YuvRgbaTables> ProbeYuvRgbaTables(int w, int h, std::string* why);

}
