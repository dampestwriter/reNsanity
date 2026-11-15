#include "alpha_trim.h"

#include <algorithm>
#include <cmath>

namespace te::alphatrim {

bool Applies(const ps2e_draw_state& s) {
    if (!s.alpha_test || s.alpha_fail != 0 || !s.tcc) return false;
    if (s.alpha_method != PS2E_DRAW_ATST_GEQUAL && s.alpha_method != PS2E_DRAW_ATST_GREATER) return false;
    return s.tfx == PS2E_DRAW_TFX_MODULATE || s.tfx == PS2E_DRAW_TFX_DECAL || s.tfx == PS2E_DRAW_TFX_HIGHLIGHT ||
           s.tfx == PS2E_DRAW_TFX_HIGHLIGHT2;
}

namespace {

uint32_t MaxAlpha(const uint32_t* texels, int w, int h, long x0, long x1, long y0, long y1, bool repeatU, bool repeatV) {
    const auto span = [](long a, long b, int n, bool repeat, long& lo, long& hi) {
        if (repeat) {
            if (b - a + 1 >= n) { lo = 0; hi = n - 1; return true; }
            lo = a; hi = b;
            return false;
        }
        lo = std::clamp<long>(a, 0, n - 1); hi = std::clamp<long>(b, 0, n - 1);
        return true;
    };
    long ux0, ux1, vy0, vy1;
    const bool directU = span(x0, x1, w, repeatU, ux0, ux1), directV = span(y0, y1, h, repeatV, vy0, vy1);
    uint32_t best = 0;
    for (long y = vy0; y <= vy1; ++y) {
        const long yy = directV ? y : ((y % h) + h) % h;
        const uint32_t* row = texels + size_t(yy) * size_t(w);
        for (long x = ux0; x <= ux1; ++x) {
            const long xx = directU ? x : ((x % w) + w) % w;
            best = std::max(best, row[xx] >> 24);
            if (best == 255) return best;
        }
    }
    return best;
}

bool Fails(const ps2e_draw_state& s, uint32_t ta, double va) {
    const double k = 255.0 / 128.0, tA = ta / 255.0, vA = va / 255.0;
    double bound;
    switch (s.tfx) {
        case PS2E_DRAW_TFX_MODULATE: bound = vA * k * tA * 128.0; break;
        case PS2E_DRAW_TFX_HIGHLIGHT: bound = (tA + vA * k) * 128.0; break;
        default: bound = tA * 128.0; break;
    }
    const double rounded = bound + 0.5 + 2.0, aref = s.alpha_reference;
    return s.alpha_method == PS2E_DRAW_ATST_GEQUAL ? rounded < aref : rounded < aref + 1.0;
}

}

namespace {

struct Counts {
    int w = 0, h = 0;
    std::vector<uint32_t> sum;
    Counts(const uint32_t* texels, int w_, int h_, uint32_t t) : w(w_), h(h_), sum(size_t(w_ + 1) * size_t(h_ + 1), 0) {
        for (int y = 0; y < h; ++y) {
            uint32_t row = 0;
            for (int x = 0; x < w; ++x) {
                row += (texels[size_t(y) * size_t(w) + size_t(x)] >> 24) >= t ? 1u : 0u;
                sum[size_t(y + 1) * size_t(w + 1) + size_t(x + 1)] = sum[size_t(y) * size_t(w + 1) + size_t(x + 1)] + row;
            }
        }
    }
    uint32_t Rect(long x0, long x1, long y0, long y1) const {
        const size_t W = size_t(w + 1);
        return sum[size_t(y1 + 1) * W + size_t(x1 + 1)] - sum[size_t(y0) * W + size_t(x1 + 1)] -
               sum[size_t(y1 + 1) * W + size_t(x0)] + sum[size_t(y0) * W + size_t(x0)];
    }

    bool Any(long x0, long x1, long y0, long y1, bool repeatU, bool repeatV) const {
        long xs[2][2], ys[2][2];
        int nx = 0, ny = 0;
        const auto pieces = [](long a, long b, int n, bool repeat, long out[2][2], int& k) {
            if (!repeat) { out[0][0] = std::clamp<long>(a, 0, n - 1); out[0][1] = std::clamp<long>(b, 0, n - 1); k = 1; return; }
            if (b - a + 1 >= n) { out[0][0] = 0; out[0][1] = n - 1; k = 1; return; }
            const long lo = ((a % n) + n) % n, len = b - a + 1;
            if (lo + len <= n) { out[0][0] = lo; out[0][1] = lo + len - 1; k = 1; return; }
            out[0][0] = lo; out[0][1] = n - 1; out[1][0] = 0; out[1][1] = lo + len - 1 - n; k = 2;
        };
        pieces(x0, x1, w, repeatU, xs, nx);
        pieces(y0, y1, h, repeatV, ys, ny);
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i)
                if (Rect(xs[i][0], xs[i][1], ys[j][0], ys[j][1])) return true;
        return false;
    }
};

}

size_t FailingTriangles(const std::vector<ps2e_draw_mesh_vertex>& tris, const te::gfx::DecodedTexture& tex,
                        const ps2e_draw_state& s, std::vector<uint8_t>& failing) {
    failing.assign(tris.size() / 3, 0);
    if (!Applies(s) || tex.width <= 0 || tex.height <= 0 || tex.rgba.size() < size_t(tex.width) * size_t(tex.height)) return 0;
    const bool repeatU = s.wrap_u == PS2E_DRAW_WRAP_REPEAT, repeatV = s.wrap_v == PS2E_DRAW_WRAP_REPEAT;
    struct Level { const uint32_t* texels; int w, h; };
    std::vector<Level> levels{{tex.rgba.data(), tex.width, tex.height}};
    for (size_t i = 0; i < tex.mips.size(); ++i) {
        const int w = std::max(1, tex.width >> (i + 1)), h = std::max(1, tex.height >> (i + 1));
        if (tex.mips[i].size() < size_t(w) * size_t(h)) break;
        levels.push_back({tex.mips[i].data(), w, h});
    }

    std::vector<std::pair<double, int>> thresholds;
    const auto threshold = [&](double va) {
        for (const auto& [v, t] : thresholds) if (v == va) return t;
        int t = 256;
        for (int ta = 0; ta < 256; ++ta) if (!Fails(s, uint32_t(ta), va)) { t = ta; break; }
        for (int ta = t; ta < 256; ++ta) if (Fails(s, uint32_t(ta), va)) { t = -1; break; }
        thresholds.emplace_back(va, t);
        return t;
    };
    std::vector<std::pair<int, std::vector<Counts>>> tables;
    const auto tablesFor = [&](int t) -> const std::vector<Counts>& {
        for (const auto& [k, v] : tables) if (k == t) return v;
        std::vector<Counts> v;
        v.reserve(levels.size());
        for (const Level& L : levels) v.emplace_back(L.texels, L.w, L.h, uint32_t(t));
        tables.emplace_back(t, std::move(v));
        return tables.back().second;
    };
    size_t flagged = 0;
    for (size_t t = 0; t + 2 < tris.size(); t += 3) {
        double umin = 1e300, umax = -1e300, vmin = 1e300, vmax = -1e300, va = 0;
        bool ok = true;
        for (int c = 0; c < 3; ++c) {
            const auto& v = tris[t + size_t(c)];
            if (!(v.q > 0.f) || !std::isfinite(v.s) || !std::isfinite(v.t)) { ok = false; break; }
            const double u = double(v.s) / v.q, w = double(v.t) / v.q;
            umin = std::min(umin, u); umax = std::max(umax, u); vmin = std::min(vmin, w); vmax = std::max(vmax, w);
            va = std::max({va, double(v.a), double(v.la)});
        }
        if (!ok || umax - umin > 64.0 || vmax - vmin > 64.0) continue;
        const int thr = threshold(va);
        bool fails;
        if (thr < 0) {
            uint32_t ta = 0;
            for (const Level& L : levels) {
                const long x0 = long(std::floor(umin * L.w - 0.5)) - 1, x1 = long(std::floor(umax * L.w - 0.5)) + 2;
                const long y0 = long(std::floor(vmin * L.h - 0.5)) - 1, y1 = long(std::floor(vmax * L.h - 0.5)) + 2;
                ta = std::max(ta, MaxAlpha(L.texels, L.w, L.h, x0, x1, y0, y1, repeatU, repeatV));
                if (ta == 255) break;
            }
            fails = Fails(s, ta, va);
        } else if (thr > 255) {
            fails = true;
        } else {
            const std::vector<Counts>& counts = tablesFor(thr);
            fails = true;
            for (size_t l = 0; l < levels.size() && fails; ++l) {
                const Level& L = levels[l];
                const long x0 = long(std::floor(umin * L.w - 0.5)) - 1, x1 = long(std::floor(umax * L.w - 0.5)) + 2;
                const long y0 = long(std::floor(vmin * L.h - 0.5)) - 1, y1 = long(std::floor(vmax * L.h - 0.5)) + 2;
                if (counts[l].Any(x0, x1, y0, y1, repeatU, repeatV)) fails = false;
            }
        }
        if (fails) { failing[t / 3] = 1; ++flagged; }
    }
    return flagged;
}

}
