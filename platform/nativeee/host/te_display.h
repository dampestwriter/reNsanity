#pragma once

namespace te::display {

struct Settings {
    int scale = 1;
    int offset_x = 0;
    int offset_y = 0;
    bool custom_textures = false;
};

void Configure(const Settings& settings);

void MoveDisplay(float x, float y);

void Apply();

}
