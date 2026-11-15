#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { TE_GFX_MATERIAL_MAGIC = 0x544D4554u , TE_GFX_SHADER_MAGIC = 0x48534554u ,
       TE_GFX_MAX_SHADERS = 4 };

enum te_gfx_material_kind {
    TE_GFX_MATERIAL_RESOURCE = 0,
    TE_GFX_MATERIAL_FLAT = 1,
    TE_GFX_MATERIAL_PARTICLE = 2,
    TE_GFX_MATERIAL_DISTORTION = 3,
    TE_GFX_MATERIAL_SKID = 4,
};

typedef struct te_gfx_shader {
    uint32_t magic;
    uint32_t type;
    uint64_t settings;
    uint32_t textureId;
    uint32_t texture;
    uint8_t colour[4];
    uint8_t fontPage;
    uint8_t pad0[3];
    int16_t lodK, lodL;
    float scroll[2];

    float leftover[2];
    float shaderColour[4];
    float scrollPhases[2];
    float scrollSpeeds[2];
    uint32_t animation;
    float extra[6];
    int32_t mode;
} te_gfx_shader;
#ifdef __cplusplus
static_assert(sizeof(te_gfx_shader) == 120, "te_gfx_shader is a game-heap allocation: its size class is part of the frozen heap layout");
#endif

typedef struct te_gfx_material {
    uint32_t magic;
    uint32_t kind;
    uint32_t bucket;
    uint32_t shaderCount;
    uint32_t shaders[TE_GFX_MAX_SHADERS];
    uint64_t activatedShaders;
    uint32_t ownsShaders;
    uint32_t reserved[13];
} te_gfx_material;

typedef struct te_gfx_texture_header {
    uint16_t unused00;
    uint16_t unused02;
    uint16_t widthPower;
    uint16_t heightPower;
    uint8_t levels;
    uint8_t pixelFormat;
    uint8_t uploadFormat;
    uint8_t hasAlpha;
    uint8_t unused0C;
    uint8_t function;
    uint8_t unused0E[2];
    uint32_t levelOffsets[7];
    uint32_t levelWidths[7];
    uint32_t paletteOffset;
    uint8_t unused4C[0x54 - 0x4C];
    int32_t transfer;
    uint32_t hostHandle;
    uint16_t unused5C;
    uint16_t secondContext;
} te_gfx_texture_header;

typedef struct te_gfx_texture {
    uint32_t headerBits;
    uint32_t id;
    uint32_t size;
    te_gfx_texture_header texture;
} te_gfx_texture;

typedef struct te_gfx_model {
    uint32_t headerBits, id;
    uint32_t subModelCount;
    uint32_t vertexCounts;
    uint32_t sizes;
    uint32_t extraSizes;
    uint32_t subModels;
} te_gfx_model;

typedef struct te_gfx_rigid_model {
    uint32_t headerBits, id;
    uint32_t materials;
    uint32_t model;
    uint32_t instanceBlock;
    uint8_t needsEye;
    uint8_t billboard;
    uint8_t pad[2];
    uint32_t count;
    uint32_t modelId;
} te_gfx_rigid_model;

typedef struct te_gfx_skin {
    uint32_t headerBits, id;
    uint32_t materials;
    uint32_t vertexCounts;
    uint32_t subModels;
    uint32_t count;
    uint32_t sizes;
    uint32_t vertexTotal;
    uint32_t unused20;
} te_gfx_skin;

typedef struct te_gfx_blend_part {
    uint32_t shapeAddresses;
    uint32_t shapeSizes;
    uint32_t shapeCounts;
    uint32_t packet;
    uint8_t shapeCount;
    uint8_t pad[3];
    float shapeFactors[3];
    uint32_t unused20;
    uint32_t packetSize;
} te_gfx_blend_part;

typedef struct te_gfx_blend_sub_model {
    uint32_t parts;
    uint32_t count;
} te_gfx_blend_sub_model;

typedef struct te_gfx_blend_skin {
    uint32_t headerBits, id;
    uint32_t materials;
    uint32_t subModels;
    uint32_t count;
    uint32_t shapeCount;
} te_gfx_blend_skin;

enum { TE_GFX_LOD_SWITCHES = 3 };
typedef struct te_gfx_lod {
    uint32_t headerBits, id;
    uint32_t meshes;
    uint8_t count;
    uint8_t pad[3];
    uint32_t nearest;
    uint32_t farthest;
    uint32_t switchDistances[TE_GFX_LOD_SWITCHES];
} te_gfx_lod;

typedef struct te_gfx_sky {
    uint32_t headerBits, id;
    uint32_t count;
    uint32_t models;
    float toScreen[4][4];
} te_gfx_sky;

#define TE_GFX_SETTING(value, shift, width) ((uint32_t)(((value) >> (shift)) & ((1ull << (width)) - 1)))
#define TE_GFX_S_BLENDS(v) TE_GFX_SETTING(v, 0, 1)
#define TE_GFX_S_PRESET(v) TE_GFX_SETTING(v, 1, 4)
#define TE_GFX_S_ALPHA_TEST(v) TE_GFX_SETTING(v, 5, 1)
#define TE_GFX_S_ALPHA_METHOD(v) TE_GFX_SETTING(v, 6, 3)
#define TE_GFX_S_ALPHA_REFERENCE(v) TE_GFX_SETTING(v, 9, 8)
#define TE_GFX_S_ALPHA_FAIL(v) TE_GFX_SETTING(v, 17, 2)
#define TE_GFX_S_DESTINATION_TEST(v) TE_GFX_SETTING(v, 19, 1)
#define TE_GFX_S_DESTINATION_MODE(v) TE_GFX_SETTING(v, 20, 1)
#define TE_GFX_S_DEPTH_TEST(v) TE_GFX_SETTING(v, 21, 2)
#define TE_GFX_S_GOURAUD(v) TE_GFX_SETTING(v, 26, 1)
#define TE_GFX_S_TEXTURED(v) TE_GFX_SETTING(v, 27, 1)
#define TE_GFX_S_STQ(v) TE_GFX_SETTING(v, 28, 1)
#define TE_GFX_S_FOG(v) TE_GFX_SETTING(v, 29, 1)
#define TE_GFX_S_SECOND_CONTEXT(v) TE_GFX_SETTING(v, 30, 1)
#define TE_GFX_S_U_SCROLL(v) TE_GFX_SETTING(v, 32, 3)
#define TE_GFX_S_V_SCROLL(v) TE_GFX_SETTING(v, 35, 3)
#define TE_GFX_S_OWN_ALPHA(v) TE_GFX_SETTING(v, 38, 1)
#define TE_GFX_S_ALPHA_A(v) TE_GFX_SETTING(v, 39, 2)
#define TE_GFX_S_ALPHA_B(v) TE_GFX_SETTING(v, 41, 2)
#define TE_GFX_S_ALPHA_C(v) TE_GFX_SETTING(v, 43, 2)
#define TE_GFX_S_ALPHA_D(v) TE_GFX_SETTING(v, 45, 2)
#define TE_GFX_S_ALPHA_FIX(v) TE_GFX_SETTING(v, 47, 8)
#define TE_GFX_S_LINEAR(v) TE_GFX_SETTING(v, 55, 1)
#define TE_GFX_S_NO_FBA(v) TE_GFX_SETTING(v, 56, 1)
#define TE_GFX_S_ANTI_ALIASED(v) TE_GFX_SETTING(v, 58, 1)
#define TE_GFX_S_NO_DEPTH_WRITES(v) TE_GFX_SETTING(v, 59, 1)
#define TE_GFX_S_ANIMATED_COLOUR(v) TE_GFX_SETTING(v, 60, 1)
#define TE_GFX_S_HAS_ANIMATION(v) TE_GFX_SETTING(v, 61, 1)

enum te_gfx_alpha_preset { TE_GFX_PRESET_MIX = 0, TE_GFX_PRESET_ADD = 1, TE_GFX_PRESET_SUBTRACT = 2,
                           TE_GFX_PRESET_BRIGHTEN = 3, TE_GFX_PRESET_DARKEN = 4, TE_GFX_PRESET_SCALE = 5 };

enum te_gfx_bucket { TE_GFX_BUCKET_SKY = 0, TE_GFX_BUCKET_OPAQUE = 2, TE_GFX_BUCKET_UI = 24, TE_GFX_BUCKET_FONTS = 26,
                     TE_GFX_BUCKET_MOVIES = 27, TE_GFX_BUCKET_COUNT = 28 };

typedef struct te_gfx_text_quad {
    float x0, y0, x1, y1;
    float u0, v0, u1, v1;
    uint32_t colour;
} te_gfx_text_quad;

typedef struct te_platform_text {
    float x, y;
    float scale;
    uint32_t colour;
    uint32_t alignment;
    char text[64];
} te_platform_text;

#ifdef __cplusplus
}
#endif
