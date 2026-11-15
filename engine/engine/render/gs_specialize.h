#pragma once

#include <cstdint>
#include <string>

namespace ps2r {
namespace render {

class GsSpecialize {
public:
    enum Field : int {
        kUseTexture, kTfx, kTcc, kTexLodHw, kMipOn, kAte, kAtst, kAfail, kFba, kColclamp, kScanmsk, kFogEnabled,
        kBlendEnabled, kSwBlend, kBlendA, kBlendB, kBlendC, kBlendD, kDate, kDatm, kFbmsk16En, kDepthFmt, kIdxFmt,
        kUvNative, kFb16, kCt16qOn, kDthe, kZsrcEn, kClutRaw, kFlipV, kLodFixed, kFieldCount
    };
    static constexpr int kLive = 15;
    struct Key {
        uint64_t lo = 0, hi = 0;
        bool operator==(const Key& o) const { return lo == o.lo && hi == o.hi; }
    };


    static bool MakeKey(const int* state, Key* key);
    static int Value(const Key& key, int field);

    static std::string Defines(const Key& key);

    static std::string Text(const Key& key);
};

}
}
