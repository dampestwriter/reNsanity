#include "engine/ipu/hle/yuv_rgba.h"

#include <chrono>
#include <cstring>

extern "C" {
#include <libavutil/log.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace ps2r::ipu {

YuvToRgba::~YuvToRgba() {
    if (ctx_) sws_freeContext(ctx_);
}

bool YuvToRgba::Convert(const uint8_t* yuv, int w, int h, uint8_t* rgba) {
    if (!yuv || !rgba || w < 2 || h < 2 || (w & 1) || (h & 1)) return false;
    if (!ctx_ || w_ != w || h_ != h) {
        if (ctx_) sws_freeContext(ctx_);

        av_log_set_level(AV_LOG_QUIET);
        ctx_ = sws_getContext(w, h, AV_PIX_FMT_YUV420P, w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        w_ = ctx_ ? w : 0;
        h_ = ctx_ ? h : 0;
        if (!ctx_) return false;
    }
    const size_t luma = size_t(w) * size_t(h), chroma = size_t(w / 2) * size_t(h / 2);
    const uint8_t* src[4] = {yuv, yuv + luma, yuv + luma + chroma, nullptr};
    const int src_ls[4] = {w, w / 2, w / 2, 0};
    uint8_t* dst[4] = {rgba, nullptr, nullptr, nullptr};
    const int dst_ls[4] = {w * 4, 0, 0, 0};
    return sws_scale(ctx_, src, src_ls, 0, h, dst, dst_ls) > 0;
}

std::shared_ptr<const YuvRgbaTables> ProbeYuvRgbaTables(int w, int h, std::string* why) {
    const auto refuse = [why](const std::string& reason) {
        if (why) *why = reason;
        return std::shared_ptr<const YuvRgbaTables>();
    };
    if (w < 2 || h < 2 || (w & 1) || (h & 1) || w > 4096 || h > 4096) return refuse("picture size");
    const auto t0 = std::chrono::steady_clock::now();
    auto t = std::make_shared<YuvRgbaTables>();
    t->width = w;
    t->height = h;
    t->rb.assign(size_t(YuvRgbaTables::kRbWidth) * YuvRgbaTables::kRbHeight, 0);
    t->g.assign(size_t(YuvRgbaTables::kGWidth) * YuvRgbaTables::kGHeight, 0);
    std::vector<uint8_t> rb_seen(t->rb.size(), 0);

    const int cw = w / 2, ch = h / 2;
    constexpr uint64_t kNeed = 65536ull * 64ull;
    YuvToRgba convert;
    std::vector<uint8_t> yuv(Yuv420Bytes(w, h)), rgba(size_t(w) * size_t(h) * 4);
    uint8_t* yp = yuv.data();
    uint8_t* up = yp + size_t(w) * size_t(h);
    uint8_t* vp = up + size_t(cw) * size_t(ch);
    uint8_t* const rb = t->rb.data();
    uint8_t* const g = t->g.data();
    for (uint64_t base = 0; base < kNeed; base += uint64_t(cw) * uint64_t(ch)) {
        uint64_t k = base;
        for (int by = 0; by < ch; ++by) {
            uint8_t* r0 = yp + size_t(2 * by) * size_t(w);
            uint8_t* r1 = r0 + w;
            uint8_t* ur = up + size_t(by) * size_t(cw);
            uint8_t* vr = vp + size_t(by) * size_t(cw);
            for (int bx = 0; bx < cw; ++bx, ++k) {
                const uint64_t kk = k < kNeed ? k : k - kNeed;
                const uint32_t pair = uint32_t(kk >> 6), yq = uint32_t(kk & 63) << 2;
                r0[2 * bx] = uint8_t(yq);
                r0[2 * bx + 1] = uint8_t(yq + 1);
                r1[2 * bx] = uint8_t(yq + 2);
                r1[2 * bx + 1] = uint8_t(yq + 3);
                ur[bx] = uint8_t(pair & 255);
                vr[bx] = uint8_t(pair >> 8);
            }
        }
        if (!convert.Convert(yuv.data(), w, h, rgba.data())) return refuse("sws_getContext / sws_scale failed");
        t->pixels += uint64_t(w) * uint64_t(h);
        k = base;
        for (int by = 0; by < ch; ++by) {
            const uint8_t* o0 = rgba.data() + size_t(2 * by) * size_t(w) * 4;
            const uint8_t* o1 = o0 + size_t(w) * 4;
            for (int bx = 0; bx < cw; ++bx, ++k) {
                const bool again = k >= kNeed;
                const uint64_t kk = again ? k - kNeed : k;
                const uint32_t U = uint32_t(kk >> 6) & 255, V = uint32_t(kk >> 14), yq = uint32_t(kk & 63) << 2;
                const uint8_t* px[4] = {o0 + size_t(2 * bx) * 4, o0 + size_t(2 * bx + 1) * 4, o1 + size_t(2 * bx) * 4,
                                        o1 + size_t(2 * bx + 1) * 4};
                uint8_t* const gq = g + YuvRgbaTables::GIndex(int(yq), int(U), int(V));
                for (uint32_t q = 0; q < 4; ++q) {
                    const uint8_t* c = px[q];
                    if (c[3] != 0xFF) return refuse("alpha not opaque");
                    const size_t ri = size_t(V) * YuvRgbaTables::kRbWidth + yq + q;
                    const size_t bi = size_t(256 + U) * YuvRgbaTables::kRbWidth + yq + q;
                    if (rb_seen[ri] && rb[ri] != c[0]) return refuse("R depends on more than (Y, V)");
                    if (rb_seen[bi] && rb[bi] != c[2]) return refuse("B depends on more than (Y, U)");
                    if (again && gq[q] != c[1]) return refuse("G depends on more than (Y, U, V)");
                    rb[ri] = c[0];
                    rb[bi] = c[2];
                    rb_seen[ri] = rb_seen[bi] = 1;
                    gq[q] = c[1];
                }
            }
        }
    }
    t->probe_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return t;
}

}
