#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <memory>
#include <utility>
#include <vector>

#include "engine/ee/timing/boundary_protocol.h"
#include "engine/platform/host/platform.h"
#include "engine/ee/timing/derived_clocks.h"
#include "engine/ee/timing/emu_domain.h"
#include "engine/ee/timing/field_pacer.h"
#include "engine/ee/timing/tick_source.h"
#include "engine/ee/timing/tick_sync.h"
#include "ps2e_seam.h"
#include "te_services.h"

namespace te {

struct Clock::Impl {
    EmuDomain domain;
    DerivedClocks clocks{domain};
    TickSource source;
    TickSync ticks;
    BoundaryProtocol protocol{source, ticks};
    uint64_t fields = 0;

    uint64_t rtc_epoch = DerivedClocks::kRtcFixedEpochS;

    std::unique_ptr<FieldPacer> pacer;

    using Steady = std::chrono::steady_clock;
    Steady::time_point released{};
    bool have_release = false;

    void StartPacer() {
        if (!pacer) pacer.reset(new FieldPacer(ticks, protocol, 60u, [] {

            ps2r::platform::host::PinCurrentThreadInteractive("te-pacer", ps2r::platform::host::ThreadRole::Pacer);
        }));
        if (!pacer->Running()) pacer->Start();
    }
    void StopPacer() {
        if (pacer) pacer->Stop();
    }
};

Clock::Clock(const Settings&) : impl_(new Impl) {
    Impl& i = *impl_;
    i.source.SetPolicy(TickSource::Policy::Play);
    i.source.SetMode(TickSource::Mode::Realtime);

    i.domain.SetRegimePin(1);
    i.domain.SetPacerRelease(true);
    i.clocks.SetRtcFixed(true);
    {

        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);

        const int y = local.tm_year + 1900, m = local.tm_mon + 1, d = local.tm_mday;
        auto days_from_civil = [](int64_t y, unsigned m, unsigned d) -> int64_t {
            y -= m <= 2;
            const int64_t era = (y >= 0 ? y : y - 399) / 400;
            const unsigned yoe = static_cast<unsigned>(y - era * 400);
            const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
            const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
            return era * 146097 + static_cast<int64_t>(doe) - 719468;
        };
        i.rtc_epoch = static_cast<uint64_t>(days_from_civil(y, m, d)) * 86400u + local.tm_hour * 3600u +
                      local.tm_min * 60u + local.tm_sec;
    }

    i.ticks.SetBody([this](TickSync::Cause cause) -> uint64_t {
        Impl& im = *impl_;
        const uint64_t raw = im.source.Tick();
        if (cause == TickSync::Cause::Deferred) im.domain.OnDeferredTick(raw);
        else im.domain.OnRawTick(raw);
        ++im.fields;
        return raw;
    });
    i.domain.ReAnchor(0);
}

Clock::~Clock() {
    impl_->StopPacer();
}
void Clock::StopPacer() { impl_->StopPacer(); }

void Clock::Start() {
    if (started_) return;
    started_ = true;
    start_counts_ = T0Counts();
    impl_->StartPacer();
}

bool Clock::LatestFieldEdge(int64_t* edge_ns, uint64_t* field) const {
    const Impl& i = *impl_;
    if (!i.pacer || i.fields == 0) return false;
    if (!i.pacer->EdgeOfTickNs(i.fields, edge_ns)) return false;
    if (field) *field = i.fields;
    return true;
}

uint64_t Clock::Fields() const { return impl_->fields; }

uint64_t Clock::T0Counts() const { return impl_->clocks.T0Counts(); }
uint64_t Clock::Ticks() const { return T0Counts() - start_counts_ + in_frame_; }

void Clock::ChargeInFrame(uint64_t ticks) {

    in_frame_ = std::min(kChargeCapTicks, in_frame_ + ticks);
}

void Clock::Boundary() {
    Impl& i = *impl_;
    in_frame_ = 0;
    if (!(i.pacer && i.pacer->Running())) i.StartPacer();
    i.domain.SetBypass(i.source.Bypass());
    i.domain.NoteArrival(i.source.RawCount());
    i.protocol.ParkForPacerTick();
    const int fired = i.ticks.FirePending();
    if (fired == 0) i.ticks.FireInline(TickSync::Cause::Boundary);
    i.domain.OnBoundary(i.source.RawCount());
    const uint64_t boundary = ++boundaries_;
    {
        const auto hooks = hooks_;
        for (const Entry& e : hooks) e.hook(boundary);
    }
    i.released = Impl::Steady::now();
    i.have_release = true;
}

int Clock::AddBoundaryHook(const std::string& name, Hook hook) {
    const int id = next_hook_++;
    hooks_.push_back({id, name, std::move(hook)});
    return id;
}

void Clock::RemoveBoundaryHook(int id) {
    for (size_t k = 0; k < hooks_.size(); ++k) {
        if (hooks_[k].id == id) { hooks_.erase(hooks_.begin() + static_cast<long>(k)); return; }
    }
}

Clock::Civil Clock::LocalTime() const {
    const Impl& i = *impl_;

    const uint64_t seconds = i.rtc_epoch + i.domain.DomainTick() / 60u;
    const DerivedClocks::RtcTime t = DerivedClocks::EpochToCivil(seconds);
    return Civil{t.year, t.month, t.day, t.hour, t.minute, t.second};
}

}

extern "C" {

void ps2e_time_initialise(void) { te::Services::Get().clock().Start(); }

ps2e_u64 ps2e_time_ticks(void) { return te::Services::Get().clock().Ticks(); }

}
