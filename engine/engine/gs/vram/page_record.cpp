#include "engine/gs/vram/page_record.h"

namespace ps2r {
namespace gs {

void PageRecord::ClearBand() {
    dirty_y0 = 0;
    dirty_y1 = 0;
    stale = false;
}

}
}
