#ifndef PS2E_DRAW_H
#define PS2E_DRAW_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps2e_draw ps2e_draw;

typedef struct ps2e_draw_config {
    int frame_width;
    int frame_height;
} ps2e_draw_config;

ps2e_draw* ps2e_draw_create(const ps2e_draw_config* config);
void ps2e_draw_destroy(ps2e_draw* device);

int ps2e_draw_pump(ps2e_draw* device);

int ps2e_draw_bind_thread(ps2e_draw* device, int bind);
#define PS2E_DRAW_HAS_BIND_THREAD 1

int ps2e_draw_resize_frame(ps2e_draw* device, int frame_width, int frame_height);
void ps2e_draw_frame_size(const ps2e_draw* device, int* frame_width, int* frame_height);

uint32_t ps2e_draw_texture_create(ps2e_draw* device, int width, int height, const void* rgba8);
int ps2e_draw_texture_upload(ps2e_draw* device, uint32_t texture, int x, int y, int width, int height,
                             const void* rgba8, size_t stride_bytes);

uint32_t ps2e_draw_texture_create_gpu_alpha(ps2e_draw* device, int width, int height, const void* rgba8);
void ps2e_draw_texture_destroy(ps2e_draw* device, uint32_t texture);

int ps2e_draw_yuv420_supported(ps2e_draw* device);
int ps2e_draw_yuv_tables(ps2e_draw* device, uint32_t serial, const uint8_t* rb, const uint8_t* g);
uint32_t ps2e_draw_texture_create_yuv420(ps2e_draw* device, int width, int height);
int ps2e_draw_texture_upload_yuv420(ps2e_draw* device, uint32_t texture, const uint8_t* yuv420);
int ps2e_draw_texture_readback(ps2e_draw* device, uint32_t texture, void* rgba8);

uint32_t ps2e_draw_texture_reserve(ps2e_draw* device);
int ps2e_draw_texture_rename(ps2e_draw* device, uint32_t from, uint32_t to);

int ps2e_draw_texture_set_mips(ps2e_draw* device, uint32_t texture, int count, const void* const* levels);

#define PS2E_DRAW_HAS_REPLACEMENT 1
uint32_t ps2e_draw_texture_create_replacement(ps2e_draw* device, int width, int height, int scale, int format, int levels,
                                              const void* const* data, const size_t* sizes);

int ps2e_draw_texture_census(const ps2e_draw* device, uint32_t texture, uint64_t out[3]);

typedef struct ps2e_draw_clear {
    uint8_t red, green, blue;
    int clear_colour;
    int clear_depth;
} ps2e_draw_clear;
void ps2e_draw_begin_frame(ps2e_draw* device, const ps2e_draw_clear* clear);

typedef struct ps2e_draw_reuse_stamp { uint64_t resources, target; } ps2e_draw_reuse_stamp;
ps2e_draw_reuse_stamp ps2e_draw_get_reuse_stamp(const ps2e_draw* device);

int ps2e_draw_backdrop(ps2e_draw* device, int restore);

typedef struct ps2e_draw_vertex {
    float x, y;
    float z;
    float u, v;
    uint8_t r, g, b, a;
    float q;
} ps2e_draw_vertex;

#define PS2E_DRAW_HAS_VERTEX_Q 1

enum ps2e_draw_depth_test { PS2E_DRAW_DEPTH_NEVER = 0, PS2E_DRAW_DEPTH_ALWAYS = 1, PS2E_DRAW_DEPTH_GEQUAL = 2,
                            PS2E_DRAW_DEPTH_GREATER = 3 };
enum ps2e_draw_alpha_test { PS2E_DRAW_ATST_NEVER = 0, PS2E_DRAW_ATST_ALWAYS = 1, PS2E_DRAW_ATST_LESS = 2,
                            PS2E_DRAW_ATST_LEQUAL = 3, PS2E_DRAW_ATST_EQUAL = 4, PS2E_DRAW_ATST_GEQUAL = 5,
                            PS2E_DRAW_ATST_GREATER = 6, PS2E_DRAW_ATST_NOTEQUAL = 7 };
enum ps2e_draw_wrap { PS2E_DRAW_WRAP_REPEAT = 0, PS2E_DRAW_WRAP_CLAMP = 1 };
enum ps2e_draw_tfx { PS2E_DRAW_TFX_MODULATE = 0, PS2E_DRAW_TFX_DECAL = 1, PS2E_DRAW_TFX_HIGHLIGHT = 2,
                     PS2E_DRAW_TFX_HIGHLIGHT2 = 3 };

typedef struct ps2e_draw_state {
    uint32_t texture;
    int gouraud;
    int depth_test;
    int depth_write;
    int blend;
    uint8_t blend_a, blend_b, blend_c, blend_d, blend_fix;
    int alpha_test;
    int alpha_method;
    uint8_t alpha_reference;
    int alpha_fail;
    int linear;
    int wrap_u, wrap_v;
    int tfx;
    int tcc;
    int scissor_x0, scissor_y0, scissor_x1, scissor_y1;
    int no_colour_write;
    int stencil;
    int fba;
    int dest_alpha_test;

    int mip_max_level;
    int mip_k;
    int mip_l;
    int mip_minify;

    int haze;
    uint32_t haze_flat;
} ps2e_draw_state;

#define PS2E_DRAW_HAS_HAZE 1

#ifdef __cplusplus
#include <cstddef>
#define PS2E_DRAW_STATE_BASE_BYTES offsetof(ps2e_draw_state, haze)
#endif
void ps2e_draw_haze_palettes(ps2e_draw* device, const uint32_t* palettes);

int ps2e_draw_haze_supported(const ps2e_draw* device);

#define PS2E_DRAW_HAS_MIPS 1

#define PS2E_DRAW_HAS_FBA_DATE 1

#define PS2E_DRAW_HAS_STENCIL 1
enum ps2e_draw_stencil { PS2E_DRAW_STENCIL_OFF = 0, PS2E_DRAW_STENCIL_COUNT_UP = 1, PS2E_DRAW_STENCIL_COUNT_DOWN = 2,
                         PS2E_DRAW_STENCIL_INSIDE = 3, PS2E_DRAW_STENCIL_CLEAR = 4 };

int ps2e_draw_submit(ps2e_draw* device, const ps2e_draw_state* state, const ps2e_draw_vertex* vertices, int count);

#define PS2E_DRAW_HAS_MESH 1
typedef struct ps2e_draw_mesh_vertex {
    float x, y, z;
    float s, t, q;
    uint8_t r, g, b, a;
    uint8_t fog, pad[3];
    float nx, ny, nz;
    uint8_t lr, lg, lb, la;
} ps2e_draw_mesh_vertex;
typedef struct ps2e_draw_mesh_params {
    float screen[16];
    float offset[2];
    float scroll[2];
    int lit;
    float light_dir[3][3];
    float light_colour[3][3];
    float ambient[3];
    float box[4];
    int cull;
} ps2e_draw_mesh_params;
uint32_t ps2e_draw_mesh_create(ps2e_draw* device, const ps2e_draw_mesh_vertex* vertices, int count);
void ps2e_draw_mesh_destroy(ps2e_draw* device, uint32_t mesh);

uint32_t ps2e_draw_mesh_reserve(ps2e_draw* device);
int ps2e_draw_mesh_create_at(ps2e_draw* device, uint32_t reserved, const ps2e_draw_mesh_vertex* vertices, int count);

int ps2e_draw_submit_mesh(ps2e_draw* device, const ps2e_draw_state* state, uint32_t mesh, const ps2e_draw_mesh_params* params);

#define PS2E_DRAW_HAS_SKIN 1
typedef struct ps2e_draw_skin_binding {
    float bone[4];
    float weight[3];
    float pad;
} ps2e_draw_skin_binding;
uint32_t ps2e_draw_skin_create(ps2e_draw* device, const ps2e_draw_mesh_vertex* vertices, const ps2e_draw_skin_binding* bones,
                               int count);
int ps2e_draw_skin_create_at(ps2e_draw* device, uint32_t reserved, const ps2e_draw_mesh_vertex* vertices,
                             const ps2e_draw_skin_binding* bones, int count);

int ps2e_draw_submit_skin(ps2e_draw* device, const ps2e_draw_state* state, uint32_t skin, const ps2e_draw_mesh_params* params,
                          const float* bones, int bone_vec4);

int ps2e_draw_submit_blend_skin(ps2e_draw* device, const ps2e_draw_state* state, uint32_t skin, const ps2e_draw_mesh_params* params,
                                const float* bones, int bone_vec4, int disp_vec4);

#define PS2E_DRAW_HAS_FRAME_COPY 1
#define PS2E_DRAW_TEXTURE_FRAME_COPY 0xFFFFFFF0u
int ps2e_draw_copy_frame(ps2e_draw* device);

void ps2e_draw_prefetch(const ps2e_draw* device, uint32_t texture, uint32_t mesh);

void ps2e_draw_present(ps2e_draw* device);

typedef enum ps2e_draw_aspect {
    PS2E_DRAW_ASPECT_PIXEL = 0,
    PS2E_DRAW_ASPECT_4_3 = 1,
    PS2E_DRAW_ASPECT_16_9 = 2,
    PS2E_DRAW_ASPECT_16_10 = 3,
    PS2E_DRAW_ASPECT_21_9 = 4,
    PS2E_DRAW_ASPECT_WINDOW = 5
} ps2e_draw_aspect;
typedef enum ps2e_draw_fit { PS2E_DRAW_FIT_LETTERBOX = 0, PS2E_DRAW_FIT_FILL = 1, PS2E_DRAW_FIT_INTEGER = 2 } ps2e_draw_fit;
typedef enum ps2e_draw_window_mode { PS2E_DRAW_WINDOWED = 0, PS2E_DRAW_FULLSCREEN = 1, PS2E_DRAW_BORDERLESS = 2 } ps2e_draw_window_mode;
typedef struct ps2e_draw_presentation {
    int aspect;
    int fit;
    int offset_x;
    int offset_y;
    int nearest;
    int vsync;
    int window_mode;
    int scale;
    int window_scale;
    int texture_filter;
    int render_width;
    float render_scale;
    int flicker_merge;
    int post_aa;
    int edge_soften;
    int haze_direct;
} ps2e_draw_presentation;
#define PS2E_DRAW_HAS_PRESENTATION 1
void ps2e_draw_set_presentation(ps2e_draw* device, const ps2e_draw_presentation* presentation);

#ifdef __cplusplus
}
#endif
#endif
