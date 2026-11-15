#pragma once

#include <utility>

#include "engine/gs/hle/ir/draw_ir.h"
#include "engine/gs/hle/ir/job_rec.h"

namespace ps2r {
namespace gs {

class DrawIrSink {
public:
    virtual ~DrawIrSink() = default;
    virtual void OnDraw(const DrawIr& draw) = 0;

    virtual void OnDrawOwned(DrawIr&& draw) { OnDraw(draw); }
    virtual void OnJob(const JobRec& job) = 0;

    virtual void OnFrame(const JobRec& vsync) = 0;
};

}
}
