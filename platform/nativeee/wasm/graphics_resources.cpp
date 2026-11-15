#include "common.h"
#include "gcc2.h"
#include "game/clock.h"
#include "game/disk.h"
#include "game/math.h"
#include "game/graphicstables.h"
#include "game/memory.h"
#include "game/particles.h"
#include "game/resources.h"
#include "game/shaderanimation.h"
#include "game/stream.h"
#include "platform/graphics.h"
#include "ps2e_seam.h"
#include "te_gfx_layout.h"
#include "te_animation_index.h"

namespace te_animation {
static bool dirty = true;
void Invalidate() { dirty = true; }
}

static_assert(sizeof(te_gfx_material) <= Platform::Graphics::MaterialStorage, "a material fits OLEG's storage");
static_assert(sizeof(te_gfx_texture) == 0x6C, "GameTexture keeps the PS2's size (the game sizes nothing by it, but the tables do)");
static_assert(sizeof(te_gfx_texture_header) == 0x60, "the file's Texture header is 0x60 bytes");

namespace
{
constexpr u32 TextureLeftoverSize = 0x20;
constexpr s32 NoHandle = -1;

constexpr u32 DeferRelease = 1;
constexpr u32 NoTexture = 0;
constexpr u32 ShaderAnimationSize = sizeof(ShaderAnimation);

te_gfx_material g_FlatMaterial;
te_gfx_shader g_FlatShader;

constexpr u32 FrameArenaSize = 0x7530 * 16;
u8* g_FrameArena;
u32 g_FrameArenaUsed;
struct FrameOverflow
{
    void* memory;
    FrameOverflow* next;
};
FrameOverflow* g_FrameOverflows;

te_gfx_material* M(Material* material) { return reinterpret_cast<te_gfx_material*>(material); }
te_gfx_shader* S(u32 shader) { return reinterpret_cast<te_gfx_shader*>(shader); }
te_gfx_texture* T(GameTexture* texture) { return reinterpret_cast<te_gfx_texture*>(texture); }

u8 ReadSettingField(Stream* stream)
{
    s8 value;
    stream->ReadS8(&value);
    return static_cast<u8>(value);
}

void ConstructShader(te_gfx_shader* shader, u32 type)
{
    te_animation::Invalidate();
    *shader = te_gfx_shader{};
    shader->magic = TE_GFX_SHADER_MAGIC;
    shader->type = type;
    shader->colour[0] = shader->colour[1] = shader->colour[2] = shader->colour[3] = 0x80;
    for (f32& value : shader->shaderColour)
    {
        value = 1.0f;
    }
}

void SetColourBytes(te_gfx_shader* shader)
{

    for (u32 channel = 0; channel < 4; channel++)
    {
        f32 value = shader->shaderColour[channel] * 128.0f;
        s32 byte = static_cast<s32>(value);
        shader->colour[channel] = static_cast<u8>(byte < 0 ? 0 : byte > 255 ? 255 : byte);
    }
}

void ReadShaderBase(te_gfx_shader* shader, Stream* stream)
{
    te_animation::Invalidate();
    u64 v = 0;
    auto field = [&](u32 shift, u32 width)
    {
        u64 value = ReadSettingField(stream) & ((1ull << width) - 1);
        v |= value << shift;
    };
    field(0, 1);
    field(1, 4);
    field(5, 1);
    field(6, 3);
    field(9, 8);
    field(17, 2);
    field(19, 1);
    field(20, 1);
    field(21, 2);
    field(23, 3);
    field(26, 1);
    field(27, 1);
    field(28, 1);
    field(29, 1);
    field(30, 1);
    field(32, 3);
    field(35, 3);
    field(38, 1);
    field(39, 2);
    field(41, 2);
    field(43, 2);
    field(45, 2);
    field(47, 8);
    field(55, 1);
    field(56, 1);
    field(57, 1);
    field(58, 1);
    field(59, 1);
    field(60, 1);
    field(61, 1);
    shader->settings = v;
    stream->ReadU16(reinterpret_cast<u16*>(&shader->lodK));
    stream->ReadU16(reinterpret_cast<u16*>(&shader->lodL));

    stream->Read(shader->scroll, sizeof(shader->scroll) + sizeof(shader->leftover), 1);
    shader->scroll[0] = shader->scroll[1] = 0.0f;
    stream->Read(shader->shaderColour, sizeof(shader->shaderColour), 1);
    stream->ReadF32(&shader->scrollPhases[0]);
    stream->ReadF32(&shader->scrollPhases[1]);
    stream->ReadF32(&shader->scrollSpeeds[0]);
    stream->ReadF32(&shader->scrollSpeeds[1]);
    stream->ReadS32(reinterpret_cast<s32*>(&shader->textureId));
    s32 type;
    stream->ReadS32(&type);
    shader->type = static_cast<u32>(type);
    if (TE_GFX_S_HAS_ANIMATION(shader->settings))
    {
        auto* animation = ConstructShaderAnimation(static_cast<ShaderAnimation*>(MemoryAllocate(ShaderAnimationSize)));
        shader->animation = reinterpret_cast<u32>(animation);
        ReadShaderAnimation(animation, stream);
    }

    u32 id = shader->textureId;
    if (id != NoTexture)
    {
        shader->texture = reinterpret_cast<u32>(g_TextureTable.Acquire(&id, nullptr));
    }

    SetColourBytes(shader);
}

void ReadShaderOfType(te_gfx_shader* shader, u32 type, Stream* stream)
{
    switch (type)
    {
    case 0x10:
    case 0x11:
    case 0x18:
        stream->ReadF32(&shader->extra[0]);
        break;
    case 0x17:
        stream->ReadS32(&shader->mode);
        stream->ReadF32(&shader->extra[0]);
        stream->ReadF32(&shader->extra[1]);
        break;
    case 0x1A:
        stream->ReadS32(&shader->mode);
        stream->ReadF32(&shader->extra[0]);
        stream->ReadF32(&shader->extra[1]);
        stream->ReadF32(&shader->extra[2]);
        stream->ReadF32(&shader->extra[3]);
        break;
    case 0x1C:
        stream->ReadF32(&shader->extra[0]);
        stream->ReadF32(&shader->extra[1]);
        break;
    default:
        break;
    }

    ReadShaderBase(shader, stream);
}

void DestroyShader(te_gfx_shader* shader)
{
    te_animation::Invalidate();
    if (shader->texture != 0)
    {
        ReleaseTexture(reinterpret_cast<GameTexture*>(shader->texture));
        shader->texture = 0;
    }

    if (shader->animation != 0)
    {

        auto* animation = reinterpret_cast<ShaderAnimation*>(shader->animation);
        if (animation->frames != nullptr)
        {
            MemoryDeallocate2_(animation->frames);
        }

        MemoryDeallocate2_(animation);
        shader->animation = 0;
    }

    shader->magic = 0;
}

void ConstructMaterialAt(te_gfx_material* material, u32 kind, u32 bucket)
{
    te_animation::Invalidate();
    *material = te_gfx_material{};
    material->magic = TE_GFX_MATERIAL_MAGIC;
    material->kind = kind;
    material->bucket = bucket;
    material->ownsShaders = 1;
}

u64 FlatSettings()
{
    u64 v = 0;
    v |= 1ull << 0;
    v |= u64{TE_GFX_PRESET_MIX} << 1;
    v |= 1ull << 21;
    v |= 1ull << 26;
    v |= 1ull << 59;
    return v;
}

te_gfx_shader* NewShader(u32 type)
{
    auto* shader = static_cast<te_gfx_shader*>(MemoryAllocate(sizeof(te_gfx_shader)));
    ConstructShader(shader, type);
    return shader;
}
}

namespace Platform::Graphics
{

GameTexture* NewTexture(u32 id)
{
    auto* texture = static_cast<te_gfx_texture*>(MemoryAllocate(sizeof(te_gfx_texture)));
    ConstructResourceHeader(texture, id);
    texture->size = 0;
    texture->texture = te_gfx_texture_header{};
    texture->texture.unused02 = 2;
    texture->texture.transfer = NoHandle;
    texture->texture.hostHandle = 0;
    return reinterpret_cast<GameTexture*>(texture);
}

void DeleteTexture(GameTexture* texture)
{
    te_gfx_texture* t = T(texture);
    ps2e_gfx2_texture_released(reinterpret_cast<ps2e_u32>(texture));
    t->texture.hostHandle = 0;
    if (t->texture.transfer >= 0)
    {
        DiskRelease(GetDiskManager(), &t->texture.transfer);
        t->texture.transfer = NoHandle;
    }

    MemoryDeallocate2_(texture);
}

void ReadTexture(GameTexture* texture, Stream* stream)
{
    te_gfx_texture* t = T(texture);
    stream->ReadU32(&t->size);
    stream->Read(&t->texture, sizeof(te_gfx_texture_header), 1);
    t->texture.secondContext = 0;
    t->texture.hostHandle = 0;
    u8 leftover[TextureLeftoverSize];
    stream->Read(leftover, TextureLeftoverSize, 1);
    u32 imageSize = t->size - (sizeof(te_gfx_texture_header) + TextureLeftoverSize);
    s32 allocated;
    DiskAllocate(&allocated, GetDiskManager(), imageSize, false, DeferRelease);
    t->texture.transfer = allocated;
    u8* image = DiskLoadedMemory(GetDiskManager(), &t->texture.transfer);
    stream->Read(image, imageSize, 1);
    t->texture.hostHandle = static_cast<u32>(ps2e_gfx2_texture_loaded(reinterpret_cast<ps2e_u32>(texture),
                                                                      reinterpret_cast<ps2e_u32>(&t->texture),
                                                                      reinterpret_cast<ps2e_u32>(image),
                                                                      static_cast<ps2e_s32>(imageSize)));
}

MaterialResource* NewMaterial(u32 id)
{
    auto* resource = static_cast<MaterialResource*>(MemoryAllocate(sizeof(MaterialResource)));
    ConstructResourceHeader(resource, id);
    resource->material = nullptr;
    resource->unused0C = 0;
    return resource;
}

void DestroyMaterial(Material* material)
{
    te_animation::Invalidate();
    te_gfx_material* m = M(material);
    if (m->magic != TE_GFX_MATERIAL_MAGIC)
    {
        return;
    }

    for (u32 index = 0; index < m->shaderCount && index < TE_GFX_MAX_SHADERS; index++)
    {
        te_gfx_shader* shader = S(m->shaders[index]);
        if (shader != nullptr && shader->magic == TE_GFX_SHADER_MAGIC)
        {
            DestroyShader(shader);
            if (m->ownsShaders != 0 && shader != &g_FlatShader)
            {
                MemoryDeallocate2_(shader);
            }
        }

        m->shaders[index] = 0;
    }

    m->shaderCount = 0;
    m->magic = 0;
}

void DeleteMaterial(MaterialResource* resource)
{
    if (resource->material != nullptr)
    {
        DestroyMaterial(resource->material);
        MemoryDeallocate2_(resource->material);
        resource->material = nullptr;
    }

    MemoryDeallocate2_(resource);
}

void ReadMaterial(MaterialResource* resource, Stream* stream)
{
    te_animation::Invalidate();
    auto* material = static_cast<te_gfx_material*>(MemoryAllocate(Platform::Graphics::MaterialStorage));
    ConstructMaterialAt(material, TE_GFX_MATERIAL_RESOURCE, TE_GFX_BUCKET_OPAQUE);
    stream->ReadS64(reinterpret_cast<s64*>(&material->activatedShaders));
    s32 bucket;
    stream->ReadS32(&bucket);
    material->bucket = static_cast<u32>(bucket);
    s32 nameLength;
    stream->ReadS32(&nameLength);
    if (nameLength != 0)
    {
        void* name = MemoryAllocate2(static_cast<u32>(nameLength));
        stream->Read(name, static_cast<u32>(nameLength), 1);
        MemoryDeallocate2_(name);
    }

    s32 count;
    stream->ReadS32(&count);
    material->shaderCount = 0;
    for (s32 index = 0; index < count; index++)
    {
        s32 type;
        stream->ReadS32(&type);
        te_gfx_shader* shader = NewShader(static_cast<u32>(type));
        ReadShaderOfType(shader, static_cast<u32>(type), stream);
        if (material->shaderCount < TE_GFX_MAX_SHADERS)
        {
            material->shaders[material->shaderCount++] = reinterpret_cast<u32>(shader);
        }
        else
        {

            DestroyShader(shader);
            MemoryDeallocate2_(shader);
        }
    }

    resource->material = reinterpret_cast<Material*>(material);
}

void ConstructMaterial(Material* material)
{
    ConstructMaterialAt(M(material), TE_GFX_MATERIAL_RESOURCE, TE_GFX_BUCKET_OPAQUE);
}

Material* MakeFlatMaterial()
{
    ConstructMaterialAt(&g_FlatMaterial, TE_GFX_MATERIAL_FLAT, TE_GFX_BUCKET_UI);
    ConstructShader(&g_FlatShader, 0x0E);
    g_FlatShader.settings = FlatSettings();
    g_FlatMaterial.shaderCount = 1;
    g_FlatMaterial.shaders[0] = reinterpret_cast<u32>(&g_FlatShader);
    g_FlatMaterial.ownsShaders = 0;
    g_FlatMaterial.activatedShaders = 1ull << 0x0E;
    return reinterpret_cast<Material*>(&g_FlatMaterial);
}

Material* FlatMaterial()
{
    return reinterpret_cast<Material*>(&g_FlatMaterial);
}

void* MakeParticleMaterial(Material* material)
{
    te_gfx_material* m = M(material);
    ConstructMaterialAt(m, TE_GFX_MATERIAL_PARTICLE, TE_GFX_BUCKET_UI);
    te_gfx_shader* shader = NewShader(0x0E);
    u64 settings = FlatSettings();
    settings = (settings & ~(0xFull << 1)) | (u64{TE_GFX_PRESET_ADD} << 1);
    settings |= 1ull << 27;
    settings |= 1ull << 28;
    shader->settings = settings;
    const auto* page = reinterpret_cast<const te_gfx_material*>(ParticlePageMaterial(0));
    const te_gfx_shader* pageShader = page != nullptr && page->magic == TE_GFX_MATERIAL_MAGIC && page->shaderCount != 0
                                          ? reinterpret_cast<const te_gfx_shader*>(page->shaders[0]) : nullptr;
    if (pageShader != nullptr && pageShader->texture != 0)
    {
        u32 id = reinterpret_cast<const te_gfx_texture*>(pageShader->texture)->id;
        shader->textureId = id;
        shader->texture = reinterpret_cast<u32>(g_TextureTable.Acquire(&id, nullptr));
        shader->lodK = pageShader->lodK;
        shader->lodL = pageShader->lodL;
    }
    m->shaders[0] = reinterpret_cast<u32>(shader);
    m->shaderCount = 1;
    return shader;
}

void MakeDistortionMaterial(Material* material)
{

    te_gfx_material* m = M(material);
    ConstructMaterialAt(m, TE_GFX_MATERIAL_DISTORTION, 23);
    te_gfx_shader* shader = NewShader(0x18);
    shader->settings = FlatSettings();
    m->shaders[0] = reinterpret_cast<u32>(shader);
    m->shaderCount = 1;
}

Material* MakeSkidMaterial(bool subtracting)
{

    auto* m = static_cast<te_gfx_material*>(MemoryAllocate(Platform::Graphics::MaterialStorage));
    ConstructMaterialAt(m, TE_GFX_MATERIAL_SKID, TE_GFX_BUCKET_OPAQUE);
    te_gfx_shader* shader = NewShader(0x0E);
    u64 settings = FlatSettings();
    settings = (settings & ~(0xFull << 1)) | (u64{subtracting ? TE_GFX_PRESET_SUBTRACT : TE_GFX_PRESET_ADD} << 1);
    settings = (settings & ~(3ull << 21)) | (2ull << 21);
    shader->settings = settings;
    m->shaders[0] = reinterpret_cast<u32>(shader);
    m->shaderCount = 1;
    return reinterpret_cast<Material*>(m);
}

void SetFontPage(Material* material, u32 page)
{
    te_gfx_material* m = M(material);
    if (m->magic == TE_GFX_MATERIAL_MAGIC && m->shaderCount != 0)
    {
        S(m->shaders[0])->fontPage = static_cast<u8>(page);
    }
}

void* AllocFrameMemory(u32 count, u32 size)
{
    u32 bytes = (count * size + 15) & ~15u;
    if (g_FrameArena == nullptr)
    {
        g_FrameArena = static_cast<u8*>(MemoryAllocate2(FrameArenaSize));
    }

    if (g_FrameArena != nullptr && g_FrameArenaUsed + bytes <= FrameArenaSize)
    {
        void* memory = g_FrameArena + g_FrameArenaUsed;
        g_FrameArenaUsed += bytes;
        return memory;
    }

    void* memory = MemoryAllocate(bytes);
    auto* entry = static_cast<FrameOverflow*>(MemoryAllocate(sizeof(FrameOverflow)));
    if (entry != nullptr)
    {
        entry->memory = memory;
        entry->next = g_FrameOverflows;
        g_FrameOverflows = entry;
    }

    return memory;
}

static void FrameMemoryStartOver()
{
    g_FrameArenaUsed = 0;
    for (FrameOverflow* entry = g_FrameOverflows; entry != nullptr;)
    {
        FrameOverflow* next = entry->next;
        if (entry->memory != nullptr)
        {
            MemoryDeallocate_(entry->memory);
        }

        MemoryDeallocate_(entry);
        entry = next;
    }

    g_FrameOverflows = nullptr;
}

void ResetBuckets(bool movie)
{
    FrameMemoryStartOver();
    ps2e_gfx2_frame_reset(movie ? 1 : 0);
}

void SubmitBuckets(bool)
{
    FrameMemoryStartOver();
}
}

namespace
{
constexpr u32 ScrollNone = 0, ScrollAnimated = 1, ScrollWrapped = 2, ScrollSine = 3, ScrollCosine = 4;
constexpr f32 ShaderTwoPi = 6.2831855f;
constexpr f32 AnimationColourScale = 256.0f, AnimationAlphaScale = 127.0f;

f32 Wrapped(f32* phase, f32 speed, f32 seconds)
{
    f32 value = *phase + speed * seconds;
    *phase = value;
    s32 whole = TruncateToInt(value);
    if (value < static_cast<f32>(whole))
    {
        whole--;
    }

    *phase -= static_cast<f32>(whole);
    return *phase;
}

f32 Waved(f32* phase, f32 speed, f32 seconds, bool cosine)
{
    *phase += speed * seconds;
    s32 angle;
    AngleFrom(&angle, *phase * ShaderTwoPi, AngleRadians);
    return cosine ? CosOfAngle(&angle) : SinOfAngle(&angle);
}

void UpdateShader(te_gfx_shader* shader, f32 seconds)
{
    const u32 uScroll = static_cast<u32>(TE_GFX_S_U_SCROLL(shader->settings));
    const u32 vScroll = static_cast<u32>(TE_GFX_S_V_SCROLL(shader->settings));
    switch (uScroll)
    {
    case ScrollWrapped: shader->scroll[0] = Wrapped(&shader->scrollPhases[0], shader->scrollSpeeds[0], seconds); break;
    case ScrollSine: shader->scroll[0] = Waved(&shader->scrollPhases[0], shader->scrollSpeeds[0], seconds, false); break;
    case ScrollCosine: shader->scroll[1] = Waved(&shader->scrollPhases[0], shader->scrollSpeeds[0], seconds, true); break;
    default: break;
    }

    switch (vScroll)
    {
    case ScrollWrapped: shader->scroll[1] = Wrapped(&shader->scrollPhases[1], shader->scrollSpeeds[1], seconds); break;
    case ScrollSine: shader->scroll[1] = Waved(&shader->scrollPhases[1], shader->scrollSpeeds[1], seconds, false); break;
    case ScrollCosine: shader->scroll[1] = Waved(&shader->scrollPhases[1], shader->scrollSpeeds[1], seconds, true); break;
    default: break;
    }

    if (shader->animation == 0)
    {
        return;
    }

    auto* animation = reinterpret_cast<ShaderAnimation*>(shader->animation);
    AdvanceShaderAnimation(animation, seconds);
    if (uScroll == ScrollAnimated)
    {
        shader->scroll[0] = animation->uvOffset.x;
    }

    if (vScroll == ScrollAnimated)
    {
        shader->scroll[1] = animation->uvOffset.y;
    }

    if (TE_GFX_S_ANIMATED_COLOUR(shader->settings))
    {
        shader->shaderColour[0] = animation->colour.x * AnimationColourScale;
        shader->shaderColour[1] = animation->colour.y * AnimationColourScale;
        shader->shaderColour[2] = animation->colour.z * AnimationColourScale;
        shader->shaderColour[3] = animation->colour.w * AnimationAlphaScale;
        SetColourBytes(shader);
    }
}
}

namespace
{
constexpr u32 StepAhead = 3;

void PrefetchMaterialEntry(u32 index, u32 count)
{
    if (index + StepAhead * 3 < count)
    {
        __builtin_prefetch(&g_MaterialTable.entries[index + StepAhead * 3].item->material);
    }

    if (index + StepAhead * 2 < count)
    {
        MaterialResource* resource = g_MaterialTable.entries[index + StepAhead * 2].item;
        if (resource != nullptr)
        {
            __builtin_prefetch(resource->material);
        }
    }

    if (index + StepAhead < count)
    {
        MaterialResource* resource = g_MaterialTable.entries[index + StepAhead].item;
        te_gfx_material* material = resource != nullptr ? M(resource->material) : nullptr;
        if (material != nullptr && material->magic == TE_GFX_MATERIAL_MAGIC)
        {
            for (u32 shaderIndex = 0; shaderIndex < material->shaderCount && shaderIndex < TE_GFX_MAX_SHADERS; shaderIndex++)
            {
                const u32 shader = material->shaders[shaderIndex];
                __builtin_prefetch(reinterpret_cast<const void*>(shader));
                __builtin_prefetch(reinterpret_cast<const void*>(shader + offsetof(te_gfx_shader, animation)));
            }
        }
    }
}
}

namespace {
constexpr u32 ActiveCapacity = 4096;
te_gfx_shader* activeShaders[ActiveCapacity];
u32 activeCount;
bool activeOverflow;
bool ShaderActs(const te_gfx_shader* shader) {
    const u32 u = TE_GFX_S_U_SCROLL(shader->settings), v = TE_GFX_S_V_SCROLL(shader->settings);
    return shader->animation != 0 || (u >= ScrollWrapped && u <= ScrollCosine) ||
           (v >= ScrollWrapped && v <= ScrollCosine);
}

u32 VisitActive() {
    u32 count = 0;
    for (u32 i = 0; i < g_MaterialTable.count; ++i) {
        const MaterialResource* resource = g_MaterialTable.entries[i].item;
        const te_gfx_material* material = resource ? M(resource->material) : nullptr;
        if (!material || material->magic != TE_GFX_MATERIAL_MAGIC) continue;
        for (u32 j = 0; j < material->shaderCount && j < TE_GFX_MAX_SHADERS; ++j) {
            auto* shader = S(material->shaders[j]);
            if (!shader || shader->magic != TE_GFX_SHADER_MAGIC || !ShaderActs(shader)) continue;
            if (count < ActiveCapacity) activeShaders[count] = shader;
            ++count;
        }
    }
    return count;
}
}

void Platform::Graphics::StepAnimations(const TimeClock* clock)
{
    const f32 seconds = static_cast<f32>(static_cast<s32>(clock->advance)) * g_SecondsPerClockUnit;
    const u32 options = ps2e_gfx2_tick_options();
    if (options & 1) {
        if (te_animation::dirty) {
            activeCount = VisitActive();
            activeOverflow = activeCount > ActiveCapacity;
            te_animation::dirty = false;
        }
        if (!activeOverflow) {
            for (u32 i = 0; i < activeCount; ++i) UpdateShader(activeShaders[i], seconds);
            return;
        }
    }

    for (u32 index = 0; index < g_MaterialTable.count; index++)
    {
        PrefetchMaterialEntry(index, g_MaterialTable.count);
        MaterialResource* resource = g_MaterialTable.entries[index].item;
        if (resource == nullptr || resource->material == nullptr)
        {
            continue;
        }

        te_gfx_material* material = M(resource->material);
        if (material->magic != TE_GFX_MATERIAL_MAGIC)
        {
            continue;
        }

        for (u32 shaderIndex = 0; shaderIndex < material->shaderCount && shaderIndex < TE_GFX_MAX_SHADERS; shaderIndex++)
        {
            auto* shader = reinterpret_cast<te_gfx_shader*>(material->shaders[shaderIndex]);
            if (shader != nullptr && shader->magic == TE_GFX_SHADER_MAGIC)
            {
                UpdateShader(shader, seconds);
            }
        }
    }
}
