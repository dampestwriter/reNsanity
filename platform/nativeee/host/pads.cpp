#include <array>
#include <cstdio>
#include <cstring>

#include "games/slus_20909_crash_twinsanity/hle/input/input.h"
#include "ps2e_seam.h"
#include "te_services.h"

namespace te {

namespace {
bool Valid(int32_t port, int32_t slot) {
    return port >= 0 && port < int32_t(Pads::kPorts) && slot >= 0 && slot < int32_t(Pads::kSlots);
}
}

Pads::Pads(Clock& clock, const Settings& settings) : clock_(clock) {
    core_.reset(new title::Input([this](unsigned port, unsigned slot, title::Input::Motors m) {
        return motors_ ? motors_(port, slot, m.small, m.big) : true;
    }));
    const bool connected = settings.pad_connected;
    sampler_ = [connected](unsigned, unsigned) { Sample s; s.connected = connected; return s; };
    hook_ = clock_.AddBoundaryHook("pads", [this](uint64_t boundary) { SampleBoundary(boundary); });
}

Pads::~Pads() { clock_.RemoveBoundaryHook(hook_); }

void Pads::SetSampler(Sampler sampler) { if (sampler) sampler_ = std::move(sampler); }

int32_t Pads::Initialise() {

    return 1;
}

int32_t Pads::Open(int32_t port, int32_t slot, uint32_t buffer) {
    if (!Valid(port, slot) || buffer == 0) return 0;
    buffers_[unsigned(port) * kSlots + unsigned(slot)] = buffer;
    if (uint8_t* area = GuestPointer(buffer, 256)) std::memset(area, 0, 256);
    return core_->Open(unsigned(port), unsigned(slot));
}

int32_t Pads::Close(int32_t port, int32_t slot) {
    if (!Valid(port, slot)) return 0;
    buffers_[unsigned(port) * kSlots + unsigned(slot)] = 0;
    return core_->Close(unsigned(port), unsigned(slot));
}

int32_t Pads::Read(int32_t port, int32_t slot, uint32_t data) {
    if (!Valid(port, slot)) return 0;
    title::Input::Report report{};
    const int32_t n = core_->Read(unsigned(port), unsigned(slot), report);
    if (n <= 0) return 0;
    uint8_t* out = GuestPointer(data, report.size());
    if (!out) return 0;
    std::memcpy(out, report.data(), report.size());
    return n;
}

int32_t Pads::GetState(int32_t port, int32_t slot) const {
    if (!Valid(port, slot)) return 0;
    return static_cast<int32_t>(core_->GetState(unsigned(port), unsigned(slot)));
}

int32_t Pads::GetRequestState(int32_t port, int32_t slot) const {
    if (!Valid(port, slot)) return 1;
    return static_cast<int32_t>(core_->GetRequest(unsigned(port), unsigned(slot)));
}

int32_t Pads::InfoMode(int32_t port, int32_t slot, int32_t info, int32_t index) const {
    if (!Valid(port, slot)) return 0;
    return core_->InfoMode(unsigned(port), unsigned(slot), info, index);
}

int32_t Pads::SetMainMode(int32_t port, int32_t slot, int32_t mode, int32_t lock) {
    if (!Valid(port, slot)) return 0;
    return core_->SetMode(unsigned(port), unsigned(slot), mode, lock);
}

int32_t Pads::InfoActuator(int32_t port, int32_t slot, int32_t actuator, int32_t term) const {
    if (!Valid(port, slot)) return 0;
    return core_->InfoActuator(unsigned(port), unsigned(slot), actuator, term);
}

int32_t Pads::SetActuatorAlign(int32_t port, int32_t slot, uint32_t align) {
    if (!Valid(port, slot)) return 0;
    const uint8_t* p = GuestPointer(align, 6);
    if (!p) return 0;
    std::array<uint8_t, 6> a{};
    std::memcpy(a.data(), p, 6);
    return core_->SetAlignment(unsigned(port), unsigned(slot), a);
}

int32_t Pads::SetActuatorDirect(int32_t port, int32_t slot, uint32_t values) {
    if (!Valid(port, slot)) return 0;
    const uint8_t* p = GuestPointer(values, 6);
    if (!p) return 0;
    std::array<uint8_t, 6> v{};
    std::memcpy(v.data(), p, 6);
    return core_->SetMotors(unsigned(port), unsigned(slot), v);
}

int32_t Pads::InfoPressureMode(int32_t port, int32_t slot) const {

    if (!Valid(port, slot)) return 0;
    const auto state = core_->GetState(unsigned(port), unsigned(slot));
    return state == title::Input::State::Stable || state == title::Input::State::Busy ? 1 : 0;
}

int32_t Pads::EnterPressureMode(int32_t port, int32_t slot) {
    if (!Valid(port, slot)) return 0;
    return core_->EnterPressure(unsigned(port), unsigned(slot));
}

void Pads::SampleBoundary(uint64_t boundary) {
    std::array<title::Input::Sample, kPorts * kSlots> samples{};
    for (unsigned port = 0; port < kPorts; ++port) {
        for (unsigned slot = 0; slot < kSlots; ++slot) {
            if (!core_->IsOpen(port, slot)) continue;
            Sample s = sampler_(port, slot);
            title::Input::Sample& out = samples[port * kSlots + slot];
            out.connected = s.connected;
            out.buttons = s.buttons;
            out.lx = s.lx; out.ly = s.ly; out.rx = s.rx; out.ry = s.ry;
        }
    }
    core_->SampleBoundary(boundary, samples);
}

}

extern "C" {

ps2e_s32 ps2e_pads_open(ps2e_s32 port, ps2e_s32 slot, ps2e_u32 buffer) { return te::Services::Get().pads().Open(port, slot, buffer); }
ps2e_s32 ps2e_pads_close(ps2e_s32 port, ps2e_s32 slot) { return te::Services::Get().pads().Close(port, slot); }
ps2e_s32 ps2e_pads_read(ps2e_s32 port, ps2e_s32 slot, ps2e_u32 data) { return te::Services::Get().pads().Read(port, slot, data); }
ps2e_s32 ps2e_pads_get_state(ps2e_s32 port, ps2e_s32 slot) { return te::Services::Get().pads().GetState(port, slot); }
ps2e_s32 ps2e_pads_get_request_state(ps2e_s32 port, ps2e_s32 slot) { return te::Services::Get().pads().GetRequestState(port, slot); }
ps2e_s32 ps2e_pads_info_mode(ps2e_s32 port, ps2e_s32 slot, ps2e_s32 info, ps2e_s32 index) { return te::Services::Get().pads().InfoMode(port, slot, info, index); }
ps2e_s32 ps2e_pads_set_main_mode(ps2e_s32 port, ps2e_s32 slot, ps2e_s32 mode, ps2e_s32 lock) { return te::Services::Get().pads().SetMainMode(port, slot, mode, lock); }
ps2e_s32 ps2e_pads_info_actuator(ps2e_s32 port, ps2e_s32 slot, ps2e_s32 actuator, ps2e_s32 term) { return te::Services::Get().pads().InfoActuator(port, slot, actuator, term); }
ps2e_s32 ps2e_pads_set_actuator_align(ps2e_s32 port, ps2e_s32 slot, ps2e_u32 align) { return te::Services::Get().pads().SetActuatorAlign(port, slot, align); }
ps2e_s32 ps2e_pads_set_actuator_direct(ps2e_s32 port, ps2e_s32 slot, ps2e_u32 values) { return te::Services::Get().pads().SetActuatorDirect(port, slot, values); }
ps2e_s32 ps2e_pads_info_pressure_mode(ps2e_s32 port, ps2e_s32 slot) { return te::Services::Get().pads().InfoPressureMode(port, slot); }
ps2e_s32 ps2e_pads_enter_pressure_mode(ps2e_s32 port, ps2e_s32 slot) { return te::Services::Get().pads().EnterPressureMode(port, slot); }

}
