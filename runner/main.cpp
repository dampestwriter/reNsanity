#include <algorithm>
#include <cfenv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "engine/ee/mem/main_memory.h"
#include "engine/core/config/config_loader.h"
#include "engine/core/config/settings_file.h"
#include "engine/platform/host/platform.h"
#include "te_graphics_hooks.h"
#include "stream_service.h"
#include "te_audio.h"
#include "te_env.h"
#include "te_game.h"
#include "te_host.h"
#include "te_display.h"
#include "te_input.h"
#include "te_language.h"
#include "te_switch.h"
#include "te_services.h"
#include "te_run.h"
#include "wasm-rt.h"

extern w2c_env* te_game_env;

namespace {
w2c_env g_env;
w2c_game g_instance{};
int g_exit_status = 0;

void Usage() {
    std::fprintf(stderr, "usage: reNsanity --iso <image> [--config ps2r.ini] [--mc-dir DIR] [--language <name>|0..7] [--no-kbm] [--no-sdl-input] [--no-audio]\n");
}
}

namespace te {
uint8_t* GuestMemory() { return g_env.memory.data; }
std::size_t GuestMemorySize() { return g_env.memory.size; }
const char* GuestString(uint32_t address, std::size_t max) {
    const uint8_t* p = GuestPointer(address, 1);
    if (!p) return nullptr;
    const std::size_t room = std::min<std::size_t>(max, GuestMemorySize() - address);
    return std::memchr(p, 0, room) ? reinterpret_cast<const char*>(p) : nullptr;
}
[[noreturn]] void RequestExit(int status) {
    g_exit_status = status;
    te::game::ExitFromGame(status);
    std::exit(status);
}
}

int main(int argc, char** argv) {
    te::nx::Startup(&argc, &argv);
    std::string iso, config_path;
    bool no_audio = false;
    te::Settings settings;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](std::string& out) { if (i + 1 >= argc) { Usage(); std::exit(2); } out = argv[++i]; };
        if (a == "--iso") next(iso);
        else if (a == "--language") {
            std::string v; next(v);
            if (!v.empty() && v.find_first_not_of("0123456789") == std::string::npos) settings.language = std::atoi(v.c_str());
            else settings.language_name = v;
        }
        else if (a == "--config") next(config_path);
        else if (a == "--mc-dir") next(settings.mc_dir);
        else if (a == "--no-sdl-input") settings.sdl_input = false;
        else if (a == "--no-kbm") settings.kbm = false;
        else if (a == "--no-audio") no_audio = true;
        else { Usage(); return 2; }
    }

    if (iso.empty()) {
        std::fprintf(stderr, "[reNsanity] No disc image was chosen: start it with --iso <image> (the PAL disc, SLES-52568).\n");
        Usage();
        return 2;
    }
    settings.iso_path = iso;

    {
        if (config_path.empty()) {
            const size_t slash = settings.mc_dir.rfind('/');
            if (!settings.mc_dir.empty() && slash != std::string::npos) config_path = settings.mc_dir.substr(0, slash) + "/ps2r.ini";
        }
        te::display::Settings display;
        if (!config_path.empty()) {
            ps2r::Config cfg;
            std::string err;
            bool existed = false;
            const bool applied = ps2r::ConfigLoader::ApplyIniFile(config_path, cfg, &err, &existed);
            if (applied && existed) {
                if (settings.language_name.empty() && settings.language < 0) settings.language_name = cfg.game.language;

                display.scale = cfg.video.scale > 0 ? cfg.video.scale : 1;
                display.offset_x = cfg.video.custom_offset_x;
                display.offset_y = cfg.video.custom_offset_y;
                display.custom_textures = cfg.video.custom_textures;
            } else if (applied) {
                const std::string language = settings.language_name.empty() ? "english" : settings.language_name;
                ps2r::SettingsFile::Set(config_path, "game.language", language, &err);
            }
            settings.settings_path = config_path;
        }
        te::display::Configure(display);
    }

    te::SetSettings(settings);

    te::audio::Configure(!no_audio);

    MainMemory ram;
    te::BindMainMemory(g_env, ram);

    wasm_rt_init();
    w2c_game& instance = g_instance;
    wasm2c_game_instantiate(&instance, &g_env);
    g_env.instance = &instance;
    te_game_env = &g_env;

    std::vector<uint8_t> elf;
    std::string error;
    if (!te::ReadElfFromIso(iso, "SLES_525.68", elf, &error)) {
        std::fprintf(stderr, "[reNsanity] Crash Twinsanity: %s -- the PAL disc image (SLES-52568) is needed.\n", error.c_str());
        return 3;
    }
    te::RetailImage image;
    if (!te::LoadRetailImage(g_env, elf, image, &error)) { std::fprintf(stderr, "[reNsanity] retail image: %s\n", error.c_str()); return 4; }

    te::PatchRetailCodePointers(g_env);
    te::VtableThunkReport vt = te::PatchVtableThunks(g_env);
    for (const std::string& s : vt.pool_exhausted_signatures) std::fprintf(stderr, "[reNsanity] vtable thunks: pool exhausted for %s\n", s.c_str());

    std::string fiber_error;
    if (!te::game::Prepare(&instance, &fiber_error)) { std::fprintf(stderr, "[reNsanity] %s\n", fiber_error.c_str()); return 5; }

    ps2r::platform::host::PinCurrentThreadInteractive("te-game", ps2r::platform::host::ThreadRole::Game);

    std::fesetround(FE_TOWARDZERO);
    for (;;) {
        const te::game::Reason reason = te::game::Resume();
        const te::game::Fiber& f = te::game::Get();
        if (reason == te::game::Reason::Exit) {
            g_exit_status = f.code;
        } else if (reason == te::game::Reason::Returned) {
            std::fprintf(stderr, "[reNsanity] entry returned (unexpected)\n");
            g_exit_status = 1;
        } else {
            std::fprintf(stderr, "[reNsanity] wasm trap: %s\n", te_trap_name(f.code));
            g_exit_status = 70;
        }
        break;
    }
    te_graphics_shutdown();
    te::Services::Get().clock().StopPacer();
    wasm2c_game_free(&instance);
    return g_exit_status;
}
