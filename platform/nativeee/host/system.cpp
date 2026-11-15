#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "engine/cdvd/iso_fs.h"
#include "games/slus_20909_crash_twinsanity/hle/input/input.h"
#include "games/slus_20909_crash_twinsanity/hle/saves/save_storage.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/files.h"
#include "ps2e_seam.h"
#include "te_input.h"
#include "te_language.h"
#include "te_services.h"

namespace te {

namespace {
Settings g_settings;
Services* g_services = nullptr;
}

const Settings& GetSettings() { return g_settings; }
void SetSettings(const Settings& settings) { g_settings = settings; }

Services& Services::Get() {
    if (!g_services) g_services = new Services;
    return *g_services;
}

Clock& Services::clock() {
    if (!clock_) clock_.reset(new Clock(g_settings));
    return *clock_;
}
Disc& Services::disc() {
    clock();
    if (!disc_) disc_.reset(new Disc(g_settings));
    return *disc_;
}
Files& Services::files() {
    disc();
    if (!files_) files_.reset(new Files(*disc_));
    return *files_;
}
Pads& Services::pads() {
    files();
    if (!pads_) pads_.reset(new Pads(*clock_, g_settings));
    return *pads_;
}
Saves& Services::saves() {
    pads();
    if (!saves_) saves_.reset(new Saves(*clock_, g_settings));
    return *saves_;
}
void Services::Initialise() {
    if (initialised_) return;

    clock();
    Disc& d = disc();
    d.Initialise();
    d.SetMedia(2);
    files().Reset();
    const int32_t ready = d.WaitReady();
    if (ready != 2)
        std::fprintf(stderr, "[reNsanity] system: disc NOT READY (%s)\n", d.IsOpen() ? d.Iso().VolumeId().c_str() : d.Error().c_str());
    initialised_ = true;
}

void Services::StartServices() {
    if (started_) return;
    Initialise();

    pads().Initialise();
    input::Install(pads(), g_settings);
    saves();
    ps2e_sound_initialise_remote();
    started_ = true;
}

}

extern "C" {

void ps2e_system_initialise(void) { te::Services::Get().Initialise(); }

void ps2e_system_start_services(void) { te::Services::Get().StartServices(); }

void ps2e_system_kputs(ps2e_u32 text) {
    (void)text;
}

void ps2e_cache_prefetch(ps2e_u32 address) {
    if (const uint8_t* p = te::GuestPointer(address, 1)) __builtin_prefetch(p);
}

void ps2e_system_exit(ps2e_s32 status) {
    te::RequestExit(status);
}

ps2e_s32 ps2e_system_language(void) { return te::language::ConsoleLanguage(); }

ps2e_s32 ps2e_system_language_text_bank(void) { return te::language::TextBank(); }

ps2e_s32 ps2e_system_language_count(void) { return static_cast<ps2e_s32>(te::language::Count()); }

ps2e_s32 ps2e_system_language_selected(void) { return te::language::Selected(); }

void ps2e_system_language_label(ps2e_s32 language, ps2e_u32 out, ps2e_s32 size) {
    if (size <= 0) return;
    uint8_t* p = te::GuestPointer(out, static_cast<size_t>(size));
    if (!p) return;
    std::string label = language >= 0 ? te::language::Label(static_cast<uint32_t>(language)) : "";
    if (label.empty()) label = "language " + std::to_string(language);
    const size_t n = std::min(label.size(), static_cast<size_t>(size) - 1);
    std::memcpy(p, label.data(), n);
    p[n] = 0;
}

void ps2e_system_set_language(ps2e_s32 language) {
    if (language >= 0) te::language::Select(static_cast<uint32_t>(language));
}

void ps2e_system_local_time(ps2e_u32 out) {
    const te::Clock::Civil t = te::Services::Get().clock().LocalTime();
    uint8_t* p = te::GuestPointer(out, 8);
    if (!p) return;
    const uint16_t year = static_cast<uint16_t>(t.year);
    std::memcpy(p, &year, 2);
    p[2] = static_cast<uint8_t>(t.month);
    p[3] = static_cast<uint8_t>(t.day);
    p[4] = static_cast<uint8_t>(t.hour);
    p[5] = static_cast<uint8_t>(t.minute);
    p[6] = static_cast<uint8_t>(t.second);
    p[7] = 0;
}

}
