#pragma once
#include <cstddef>
#include <cstdint>

typedef struct ps2e_draw ps2e_draw;

extern "C" {

ps2e_draw* te_graphics_device(void);

void te_graphics_on_render(void (*fn)(void* user), void* user);

void te_graphics_post_render(void (*fn)(void* user), void* user);

void te_graphics_record_movie_quad(uint32_t texture, float x0, float y0, float x1, float y1, float u0, float v0, float u1,
                                   float v1);



void te_graphics_shutdown(void);
}
