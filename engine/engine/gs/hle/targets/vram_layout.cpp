#include "engine/gs/hle/targets/vram_layout.h"

namespace ps2r {
namespace gs {

bool VramLayout::IsClutHealExempt(uint32_t cbp) const {
    return clut_heal_exempt_hi != 0 && cbp >= clut_heal_exempt_lo && cbp <= clut_heal_exempt_hi &&
           (cbp & clut_heal_exempt_mask) == clut_heal_exempt_value;
}

}
}
