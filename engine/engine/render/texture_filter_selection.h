#pragma once

#include <atomic>
#include <cstdint>

namespace ps2r {
namespace render {

enum class TextureFilter : uint8_t { Title = 0, Point, Bilinear, Trilinear };

class TextureFilterSelection {
public:
    static constexpr int kMaxAnisotropy = 16;

    explicit TextureFilterSelection(TextureFilter filter = TextureFilter::Title, int anisotropy = 1)
        : filter_(filter), anisotropy_(static_cast<uint8_t>(ClampAnisotropy(anisotropy))) {}

    TextureFilter Filter() const { return filter_.load(std::memory_order_relaxed); }
    int Anisotropy() const { return anisotropy_.load(std::memory_order_relaxed); }

    void SetFilter(TextureFilter filter) { filter_.store(filter, std::memory_order_relaxed); }
    void SetAnisotropy(int anisotropy) {
        anisotropy_.store(static_cast<uint8_t>(ClampAnisotropy(anisotropy)), std::memory_order_relaxed);
    }

    bool Enhances() const { return Filter() != TextureFilter::Title || Anisotropy() > 1; }

    static int ClampAnisotropy(int requested) {
        return requested >= 16 ? 16 : requested >= 8 ? 8 : requested >= 4 ? 4 : requested >= 2 ? 2 : 1;
    }

    static const char* Name(TextureFilter filter) {
        return static_cast<uint8_t>(filter) < 4 ? kNames[static_cast<uint8_t>(filter)] : kNames[0];
    }

private:
    static constexpr const char* kNames[4] = {"title", "point", "bilinear", "trilinear"};
    std::atomic<TextureFilter> filter_;
    std::atomic<uint8_t> anisotropy_;
};

}
}
