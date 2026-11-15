#include "engine/platform/host/platform.h"

namespace ps2r::platform::host {

void PinCurrentThreadInteractive(const char* name, ThreadRole role, int worker_index) {
    ps2r::host::SetCurrentThreadName(name);
    ps2r::host::PinCurrentThreadFixed(role, worker_index);
}

}
