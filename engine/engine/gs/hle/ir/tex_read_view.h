#pragma once

#include <cstdint>

namespace ps2r {
namespace gs {

struct TexReadView {

    uint32_t tbp = 0;
    uint32_t tbw = 0;
    uint32_t psm = 0;
    uint32_t tw = 0;
    uint32_t th = 0;
    uint32_t tcc = 0;
    uint32_t tfx = 0;
    uint32_t cbp = 0;
    uint32_t cpsm = 0;
    uint32_t csm = 0;
    uint32_t csa = 0;
    uint32_t cld = 0;
    bool fst = false;

    bool mmag = false;
    uint8_t mmin = 0;
    uint8_t mxl = 0;
    bool lcm = false;
    uint8_t l = 0;
    int16_t k = 0;

    uint8_t wms = 0, wmt = 0;
    uint16_t minu = 0, maxu = 0, minv = 0, maxv = 0;

    uint8_t texa_ta0 = 0;
    uint8_t texa_ta1 = 0x80;
    bool texa_aem = false;

    uint32_t mip_tbp[6] = {0, 0, 0, 0, 0, 0};
    uint32_t mip_tbw[6] = {0, 0, 0, 0, 0, 0};
};

}
}
