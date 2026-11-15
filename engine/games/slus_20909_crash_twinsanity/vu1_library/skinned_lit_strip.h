#pragma once

#include "engine/vu/hle/vu1_hle_contract.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/library_common.h"

class SkinnedLitStrip {
public:
    static bool RunScalePhase(QwPtr m, vu1lib::u32 top, vu1lib::u32* stale_w_out);

    static bool RunSkinCb0(QwPtr m, vu1lib::u32 top);

    static bool RunSkin1040(QwPtr m, vu1lib::u32 top, vu1lib::u32 entry_vf19_w);

    static bool RunClipMark0B30(QwPtr m, vu1lib::u32 top);

    static bool RunSkinnedLitBody(QwPtr m, vu1lib::u32 node, vu1lib::u32 top);
};
