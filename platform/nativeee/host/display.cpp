#include "te_display.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "engine/core/config/settings_file.h"
#include "engine/platform/ps2e/present_pacer.h"
#include "engine/platform/ps2e/ps2e_draw.h"
#include "graphics_internal.h"
#include "ps2e_seam.h"
#include "te_host.h"
#include "texpack.h"

namespace te::display {

namespace {

Settings g_settings;

void Persist(const char* key, const std::string& value) {
    const std::string& path = GetSettings().settings_path;
    if (path.empty()) return;
    std::string err;
    ps2r::SettingsFile::Set(path, key, value, &err);
}

}

void Apply() {
    ps2e_draw_presentation p{};
    p.aspect = PS2E_DRAW_ASPECT_16_9;
    p.fit = PS2E_DRAW_FIT_LETTERBOX;
    p.offset_x = g_settings.offset_x;
    p.offset_y = g_settings.offset_y;
    p.nearest = 0;
    p.vsync = 1;
    p.window_mode = 1;
    p.scale = g_settings.scale;
    p.render_width = 0;
    p.render_scale = -1.f;
    p.window_scale = 0;
    p.texture_filter = 0;
    p.flicker_merge = 1;
    p.post_aa = 0;
    p.edge_soften = 1;
    p.haze_direct = 1;
    gfx::SetPresentation(p);
    gfx::SetFrameRate(int(ps2r::ps2e::PresentPacer::Rate::Rate60));
}

void Configure(const Settings& settings) {
    g_settings = settings;
    g_settings.scale = std::clamp(g_settings.scale, 1, 4);
    g_settings.offset_x = std::clamp(g_settings.offset_x, -256, 256);
    g_settings.offset_y = std::clamp(g_settings.offset_y, -256, 256);
    Apply();
    if (g_settings.custom_textures && gfx::texpack::Available()) gfx::texpack::SetEnabled(true);
}

void MoveDisplay(float x, float y) {
    const float cx = std::clamp(x, -1.f, 1.f), cy = std::clamp(y, -1.f, 1.f);
    const int ox = int(std::lround(cx * 64.f)), oy = int(std::lround(cy * 32.f));
    if (ox == g_settings.offset_x && oy == g_settings.offset_y) return;
    g_settings.offset_x = ox;
    g_settings.offset_y = oy;
    Apply();
    Persist("video.custom_offset_x", std::to_string(ox));
    Persist("video.custom_offset_y", std::to_string(oy));
}

}

namespace {
void CopyLabel(const char* label, ps2e_u32 out, ps2e_s32 size) {
    if (size <= 0) return;
    uint8_t* p = te::GuestPointer(out, static_cast<size_t>(size));
    if (!p) return;
    const size_t n = std::min(std::strlen(label), static_cast<size_t>(size) - 1);
    std::memcpy(p, label, n);
    p[n] = 0;
}
}

extern "C" {

ps2e_s32 ps2e_graphics_display_setting_count(void) { return te::gfx::texpack::Available() ? 1 : 0; }

ps2e_s32 ps2e_graphics_display_setting_choices(ps2e_s32 setting) { return setting == 0 ? 2 : 0; }

void ps2e_graphics_display_setting_label(ps2e_s32 setting, ps2e_u32 out, ps2e_s32 size) {
    CopyLabel(setting == 0 ? "custom textures" : "", out, size);
}

void ps2e_graphics_display_setting_choice_label(ps2e_s32 setting, ps2e_s32 choice, ps2e_u32 out, ps2e_s32 size) {
    CopyLabel(setting != 0 ? "" : choice == 0 ? "off" : "on", out, size);
}

ps2e_s32 ps2e_graphics_display_setting_value(ps2e_s32 setting) {
    return setting == 0 && te::gfx::texpack::Enabled() ? 1 : 0;
}

void ps2e_graphics_set_display_setting(ps2e_s32 setting, ps2e_s32 choice) {
    if (setting != 0) return;
    const bool on = choice != 0;
    te::gfx::texpack::SetEnabled(on);
    te::display::Persist("video.custom_textures", on ? "on" : "off");
}

}
