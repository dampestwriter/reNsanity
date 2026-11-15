#include "engine/render/texture_mips.h"

namespace ps2r {
namespace render {

void BoxHalve(const std::vector<uint8_t>& src, int cw, int ch,
                 std::vector<uint8_t>& dst, int* nw, int* nh) {
    const int w = cw > 1 ? cw >> 1 : 1;
    const int h = ch > 1 ? ch >> 1 : 1;
    const int sx = cw > 1 ? 2 : 1;
    const int sy = ch > 1 ? 2 : 1;
    dst.assign(static_cast<size_t>(w) * h * 4, 0);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            for (int c = 0; c < 4; c++) {
                unsigned s = 0, cnt = 0;
                for (int dy = 0; dy < sy; dy++)
                    for (int dx = 0; dx < sx; dx++) {
                        const int px = x * sx + dx, py = y * sy + dy;
                        s += src[(static_cast<size_t>(py) * cw + px) * 4 + c];
                        cnt++;
                    }
                dst[(static_cast<size_t>(y) * w + x) * 4 + c] = static_cast<uint8_t>((s + cnt / 2) / cnt);
            }
        }
    }
    *nw = w;
    *nh = h;
}

}
}
