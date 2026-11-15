#include "common.h"
#include "gcc2.h"
#include "game/disk.h"
#include "game/graphicstables.h"
#include "game/math.h"
#include "game/memory.h"
#include "game/particles.h"
#include "game/place.h"
#include "game/reference.h"
#include "game/resources.h"
#include "game/shaderanimation.h"
#include "game/stream.h"
#include "game/view.h"
#include "platform/graphics.h"
#include "ps2e_seam.h"
#include "te_gfx_layout.h"
#include "te_animation_index.h"

static_assert(sizeof(te_gfx_model) == 0x1C, "RigidModelData is 0x1C bytes (shadows.cpp ModelSize)");
static_assert(sizeof(te_gfx_rigid_model) == 0x20, "RigidModel is 0x20 bytes (shadows.cpp MeshSize)");
static_assert(sizeof(te_gfx_skin) == 0x24, "Skin is 0x24 bytes");
static_assert(sizeof(te_gfx_blend_part) == 0x28, "BlendPart is 0x28 bytes");
static_assert(sizeof(te_gfx_blend_skin) == 0x18, "BlendSkin is 0x18 bytes");
static_assert(sizeof(te_gfx_lod) == 0x24, "Lod is 0x24 bytes");
static_assert(sizeof(te_gfx_sky) == 0x50, "Sky is 0x50 bytes");

extern "C"
{

    extern Vector4 te_gfx_ViewPlanes[6] asm("D_003B4900");
    extern Vector4 te_gfx_FarViewPlanes[6] asm("D_003B4960");

    RigidModelData* te_ModelConstruct(void* memory, u32 id) asm("InitGameModel");
    void te_ModelRead(RigidModelData* model, Stream* stream) asm("ReadGameModel");
    RigidModel* te_MeshConstruct(void* memory, u32 id) asm("FUN_001c1e70");
    void te_MeshRead(RigidModel* mesh, Stream* stream) asm("ReadRigidModel2");
}

namespace
{
constexpr s32 NoHandle = -1;
constexpr u32 DeferRelease = 1;
constexpr s32 LodDistancesVersion = 0x1001;
constexpr s32 LodSquaresVersion = 0x1002;
constexpr u32 NoDistance = 0xFFFFFFFF;
constexpr u32 QuadwordShift = 4;
constexpr u32 ShapeFactorsSize = sizeof(te_gfx_blend_part::shapeFactors);

constexpr f32 FarSidesScale = 5.0f;
enum FrustumPlane : u32
{
    NearPlane = 0,
    FirstSidePlane = 1,
    FarPlane = 5,
};

te_gfx_model* MD(RigidModelData* model) { return reinterpret_cast<te_gfx_model*>(model); }
te_gfx_rigid_model* RM(RigidModel* model) { return reinterpret_cast<te_gfx_rigid_model*>(model); }
te_gfx_skin* SK(Skin* skin) { return reinterpret_cast<te_gfx_skin*>(skin); }
te_gfx_blend_skin* BS(BlendSkin* skin) { return reinterpret_cast<te_gfx_blend_skin*>(skin); }
te_gfx_lod* LD(Lod* lod) { return reinterpret_cast<te_gfx_lod*>(lod); }
te_gfx_sky* SY(Sky* sky) { return reinterpret_cast<te_gfx_sky*>(sky); }
template <typename T>
T* P(u32 address) { return reinterpret_cast<T*>(address); }
template <typename T>
u32 A(T* pointer) { return reinterpret_cast<u32>(pointer); }

s32* NewHandles(u32 count)
{
    s32* handles = NewArray<s32>(count);
    for (u32 index = 0; index < count; index++)
    {
        handles[index] = NoHandle;
    }

    return handles;
}

u8* ReadPacket(Stream* stream, s32* handle, u32 size)
{
    s32 allocated;
    DiskAllocate(&allocated, GetDiskManager(), size, false, DeferRelease);
    *handle = allocated;
    u8* memory = DiskLoadedMemory(GetDiskManager(), handle);
    stream->Read(memory, size, 1);
    return memory;
}

void ReadS32(Stream* stream, u32* value) { stream->ReadS32(reinterpret_cast<s32*>(value)); }

bool ShaderNeedsEye(u32 type)
{
    switch (type)
    {
    case 0x0B: case 0x0C: case 0x0F: case 0x10: case 0x11: case 0x16: case 0x18: case 0x1B:
        return true;
    default:
        return false;
    }
}

void AskShaders(te_gfx_rigid_model* model)
{
    model->needsEye = 0;
    model->billboard = 0;
    auto** materials = P<MaterialResource*>(model->materials);
    for (u32 index = 0; index < model->count; index++)
    {
        MaterialResource* resource = materials[index];
        auto* material = resource != nullptr ? reinterpret_cast<te_gfx_material*>(resource->material) : nullptr;
        if (material == nullptr || material->magic != TE_GFX_MATERIAL_MAGIC)
        {
            continue;
        }

        for (u32 shader = 0; shader < material->shaderCount && shader < TE_GFX_MAX_SHADERS; shader++)
        {
            auto* s = P<te_gfx_shader>(material->shaders[shader]);
            if (s == nullptr || s->magic != TE_GFX_SHADER_MAGIC)
            {
                continue;
            }

            if (ShaderNeedsEye(s->type))
            {
                model->needsEye = 1;
            }

            if (s->type == 0x1B)
            {
                model->billboard = 1;
            }
        }
    }
}

void ReadRigidModelData(te_gfx_rigid_model* model, Stream* stream)
{
    ReadS32(stream, &model->count);
    auto** materials = static_cast<MaterialResource**>(MemoryAllocate2(model->count * sizeof(MaterialResource*)));
    model->materials = A(materials);
    for (u32 index = 0; index < model->count; index++)
    {
        u32 id;
        ReadS32(stream, &id);
        materials[index] = g_MaterialTable.Acquire(&id, nullptr);
    }

    ReadS32(stream, &model->modelId);
    model->model = A(g_ModelTable.Acquire(&model->modelId, nullptr));
    model->instanceBlock = 0;
    AskShaders(model);
}

RigidModelData* ModelConstruct(void* memory, u32 id)
{
    auto* model = static_cast<te_gfx_model*>(memory);
    ConstructResourceHeader(model, id);
    model->subModelCount = 0;
    model->subModels = 0;
    model->vertexCounts = 0;
    model->sizes = 0;
    model->extraSizes = 0;
    return reinterpret_cast<RigidModelData*>(model);
}

void ModelDestroy(te_gfx_model* model, u32 flags)
{
    ps2e_gfx2_model_released(A(model));
    auto* handles = P<s32>(model->subModels);
    for (u32 index = 0; index < model->subModelCount; index++)
    {
        DiskRelease(GetDiskManager(), &handles[index]);
    }

    if (handles != nullptr)
    {
        DeleteArray(handles);
    }

    if (model->vertexCounts != 0)
    {
        MemoryDeallocate_(P<void>(model->vertexCounts));
    }

    if (model->sizes != 0)
    {
        MemoryDeallocate_(P<void>(model->sizes));
    }

    if (model->extraSizes != 0)
    {
        MemoryDeallocate_(P<void>(model->extraSizes));
    }

    if ((flags & FreeAfterDestroy) != 0)
    {
        MemoryDeallocate2_(model);
    }
}

void ModelRead(te_gfx_model* model, Stream* stream)
{
    ReadS32(stream, &model->subModelCount);
    s32* handles = NewHandles(model->subModelCount);
    model->subModels = A(handles);
    auto* vertexCounts = static_cast<u32*>(MemoryAllocate2(model->subModelCount * sizeof(u32)));
    auto* sizes = static_cast<u32*>(MemoryAllocate2(model->subModelCount * sizeof(u32)));
    auto* extraSizes = static_cast<u32*>(MemoryAllocate2(model->subModelCount * sizeof(u32)));
    model->vertexCounts = A(vertexCounts);
    model->sizes = A(sizes);
    model->extraSizes = A(extraSizes);
    for (u32 index = 0; index < model->subModelCount; index++)
    {
        ReadS32(stream, &vertexCounts[index]);
        ReadS32(stream, &sizes[index]);
        u8* packet = ReadPacket(stream, &handles[index], sizes[index]);
        ps2e_gfx2_model_loaded(A(model), static_cast<ps2e_s32>(index), static_cast<ps2e_s32>(model->subModelCount), A(packet),
                               static_cast<ps2e_s32>(sizes[index]), static_cast<ps2e_s32>(vertexCounts[index]));
        ReadS32(stream, &extraSizes[index]);
        if (extraSizes[index] != 0)
        {
            void* extra = MemoryAllocate2(extraSizes[index]);
            stream->Read(extra, extraSizes[index], 1);
            if (extra != nullptr)
            {
                MemoryDeallocate_(extra);
            }
        }
    }
}

RigidModel* RigidModelConstruct(void* memory, u32 id)
{
    auto* model = static_cast<te_gfx_rigid_model*>(memory);
    ConstructResourceHeader(model, id);
    model->materials = 0;
    model->model = 0;
    model->instanceBlock = 0;
    model->needsEye = 0;
    model->billboard = 0;
    model->pad[0] = model->pad[1] = 0;
    model->count = 0;
    model->modelId = 0;
    return reinterpret_cast<RigidModel*>(model);
}

void RigidModelDestroy(te_gfx_rigid_model* model, u32 flags)
{
    auto** materials = P<MaterialResource*>(model->materials);
    for (u32 index = 0; index < model->count; index++)
    {
        ReleaseMaterial(materials[index]);
    }

    ReleaseModel(P<RigidModelData>(model->model));
    if (materials != nullptr)
    {
        MemoryDeallocate_(materials);
    }

    if ((flags & FreeAfterDestroy) != 0)
    {
        MemoryDeallocate2_(model);
    }
}

void RigidModelRead(te_gfx_rigid_model* model, Stream* stream)
{
    s32 version = 0;
    stream->ReadS32(&version);
    ReadRigidModelData(model, stream);
}

Skin* SkinConstruct(void* memory, u32 id)
{
    auto* skin = static_cast<te_gfx_skin*>(memory);
    ConstructResourceHeader(skin, id);
    skin->materials = 0;
    skin->vertexCounts = 0;
    skin->subModels = 0;
    skin->count = 0;
    skin->sizes = 0;

    return reinterpret_cast<Skin*>(skin);
}

void SkinDestroy(te_gfx_skin* skin, u32 flags)
{
    auto** materials = P<MaterialResource*>(skin->materials);
    auto* handles = P<s32>(skin->subModels);
    for (u32 index = 0; index < skin->count; index++)
    {
        ReleaseMaterial(materials[index]);
        if (handles[index] >= 0)
        {
            DiskRelease(GetDiskManager(), &handles[index]);
        }
    }

    ps2e_gfx2_skin_released(A(skin));
    if (materials != nullptr)
    {
        MemoryDeallocate_(materials);
    }

    if (skin->vertexCounts != 0)
    {
        MemoryDeallocate_(P<void>(skin->vertexCounts));
    }

    if (handles != nullptr)
    {
        DeleteArray(handles);
    }

    if (skin->sizes != 0)
    {
        MemoryDeallocate_(P<void>(skin->sizes));
    }

    if ((flags & FreeAfterDestroy) != 0)
    {
        MemoryDeallocate2_(skin);
    }
}

void SkinRead(te_gfx_skin* skin, Stream* stream)
{
    ReadS32(stream, &skin->count);
    auto** materials = static_cast<MaterialResource**>(MemoryAllocate2(skin->count * sizeof(MaterialResource*)));
    skin->materials = A(materials);
    s32* handles = NewHandles(skin->count);
    skin->subModels = A(handles);
    auto* sizes = static_cast<u32*>(MemoryAllocate2(skin->count * sizeof(u32)));
    skin->sizes = A(sizes);
    auto* vertexCounts = static_cast<u32*>(MemoryAllocate2(skin->count * sizeof(u32)));
    skin->vertexCounts = A(vertexCounts);
    for (u32 index = 0; index < skin->count; index++)
    {
        u32 id;
        ReadS32(stream, &id);
        materials[index] = g_MaterialTable.Acquire(&id, nullptr);
        ReadS32(stream, &sizes[index]);
        ReadS32(stream, &vertexCounts[index]);
        u8* packet = ReadPacket(stream, &handles[index], sizes[index]);

        ps2e_gfx2_skin_loaded(A(skin), static_cast<ps2e_s32>(index), static_cast<ps2e_s32>(skin->count), A(packet),
                              static_cast<ps2e_s32>(sizes[index]), static_cast<ps2e_s32>(vertexCounts[index]));
    }
}

te_gfx_blend_part* BlendPartConstruct(te_gfx_blend_part* part)
{
    part->shapeAddresses = 0;
    part->shapeSizes = 0;
    part->shapeCounts = 0;
    part->packet = 0;
    part->shapeCount = 0;
    return part;
}

void BlendPartDestroy(te_gfx_blend_part* part, u32 flags)
{
    if (part->packet != 0)
    {
        MemoryDeallocate_(P<void>(part->packet));
    }

    auto* addresses = P<u32>(part->shapeAddresses);
    for (s32 shape = 0; shape < part->shapeCount; shape++)
    {
        MemoryDeallocate2_(P<void>(addresses[shape]));
    }

    if (addresses != nullptr)
    {
        MemoryDeallocate_(addresses);
    }

    if (part->shapeSizes != 0)
    {
        MemoryDeallocate_(P<void>(part->shapeSizes));
    }

    if (part->shapeCounts != 0)
    {
        MemoryDeallocate_(P<void>(part->shapeCounts));
    }

    if ((flags & FreeAfterDestroy) != 0)
    {
        MemoryDeallocate2_(part);
    }
}

void ReadBlendPart(te_gfx_blend_part* part, Stream* stream, u32 shapeCount)
{
    ReadS32(stream, &part->packetSize);
    ReadS32(stream, &part->unused20);
    void* packet = MemoryAllocate2(part->packetSize);
    part->packet = A(packet);
    stream->Read(packet, part->packetSize, 1);
    stream->Read(part->shapeFactors, ShapeFactorsSize, 1);
    part->shapeCount = static_cast<u8>(shapeCount);
    auto* addresses = static_cast<u32*>(MemoryAllocate2(part->shapeCount * sizeof(u32)));
    auto* sizes = static_cast<u32*>(MemoryAllocate2(part->shapeCount * sizeof(u32)));
    auto* counts = static_cast<u32*>(MemoryAllocate2(part->shapeCount * sizeof(u32)));
    part->shapeAddresses = A(addresses);
    part->shapeSizes = A(sizes);
    part->shapeCounts = A(counts);
    for (u32 shape = 0; shape < part->shapeCount; shape++)
    {
        ReadS32(stream, &sizes[shape]);
        ReadS32(stream, &counts[shape]);
        u32 size = sizes[shape] << QuadwordShift;
        void* memory = MemoryAllocate2(size);
        addresses[shape] = A(memory);
        stream->Read(memory, size, 1);
    }
}

void BlendSubModelDestroy(te_gfx_blend_sub_model* subModel, u32 flags)
{
    if (subModel->count != 0)
    {
        auto** parts = P<te_gfx_blend_part*>(subModel->parts);
        for (u32 index = 0; index < subModel->count; index++)
        {
            if (parts[index] != nullptr)
            {
                BlendPartDestroy(parts[index], DestroyAndFree);
            }
        }

        if (parts != nullptr)
        {
            MemoryDeallocate_(parts);
        }
    }

    subModel->parts = 0;
    subModel->count = 0;
    if ((flags & FreeAfterDestroy) != 0)
    {
        MemoryDeallocate2_(subModel);
    }
}

BlendSkin* BlendSkinConstruct(void* memory, u32 id)
{
    auto* skin = static_cast<te_gfx_blend_skin*>(memory);
    ConstructResourceHeader(skin, id);
    skin->materials = 0;
    skin->subModels = 0;
    skin->count = 0;
    return reinterpret_cast<BlendSkin*>(skin);
}

void BlendSkinDestroy(te_gfx_blend_skin* skin, u32 flags)
{
    auto** materials = P<MaterialResource*>(skin->materials);
    auto** subModels = P<te_gfx_blend_sub_model*>(skin->subModels);
    for (u32 index = 0; index < skin->count; index++)
    {
        ReleaseMaterial(materials[index]);
        if (subModels[index] != nullptr)
        {
            BlendSubModelDestroy(subModels[index], DestroyAndFree);
        }
    }

    ps2e_gfx2_blend_skin_released(A(skin));
    if (materials != nullptr)
    {
        MemoryDeallocate_(materials);
    }

    if (subModels != nullptr)
    {
        MemoryDeallocate_(subModels);
    }

    if ((flags & FreeAfterDestroy) != 0)
    {
        MemoryDeallocate2_(skin);
    }
}

void BlendSkinRead(te_gfx_blend_skin* skin, Stream* stream)
{
    ReadS32(stream, &skin->count);
    ReadS32(stream, &skin->shapeCount);
    auto** materials = static_cast<MaterialResource**>(MemoryAllocate2(skin->count * sizeof(MaterialResource*)));
    auto** subModels = static_cast<te_gfx_blend_sub_model**>(MemoryAllocate2(skin->count * sizeof(te_gfx_blend_sub_model*)));
    skin->materials = A(materials);
    skin->subModels = A(subModels);
    for (u32 index = 0; index < skin->count; index++)
    {
        auto* subModel = static_cast<te_gfx_blend_sub_model*>(MemoryAllocate(sizeof(te_gfx_blend_sub_model)));
        subModel->parts = 0;
        subModel->count = 0;
        u32 partCount;
        ReadS32(stream, &partCount);
        u32 id;
        ReadS32(stream, &id);
        materials[index] = g_MaterialTable.Acquire(&id, nullptr);
        subModel->count = partCount;
        auto** parts = static_cast<te_gfx_blend_part**>(MemoryAllocate2(partCount * sizeof(te_gfx_blend_part*)));
        subModel->parts = A(parts);
        for (u32 part = 0; part < partCount; part++)
        {
            parts[part] = BlendPartConstruct(static_cast<te_gfx_blend_part*>(MemoryAllocate(sizeof(te_gfx_blend_part))));
            ReadBlendPart(parts[part], stream, skin->shapeCount);
        }

        subModels[index] = subModel;
    }
}

void ReadMeshes(RigidModel** meshes, u32 count, Stream* stream)
{
    for (u32 index = 0; index < count; index++)
    {
        u32 id;
        ReadS32(stream, &id);
        meshes[index] = g_MeshTable.Acquire(&id, nullptr);
    }
}

Lod* LodConstruct(void* memory, u32 id)
{
    auto* lod = static_cast<te_gfx_lod*>(memory);
    ConstructResourceHeader(lod, id);
    lod->meshes = 0;
    lod->count = 0;
    return reinterpret_cast<Lod*>(lod);
}

void LodDestroy(te_gfx_lod* lod, u32 flags)
{
    auto** meshes = P<RigidModel*>(lod->meshes);
    for (s32 index = 0; index < lod->count; index++)
    {
        ReleaseMesh(meshes[index]);
    }

    if (meshes != nullptr)
    {
        MemoryDeallocate_(meshes);
    }

    if ((flags & FreeAfterDestroy) != 0)
    {
        MemoryDeallocate2_(lod);
    }
}

void LodRead(te_gfx_lod* lod, Stream* stream)
{
    s32 version;
    stream->ReadS32(&version);
    if (version == LodDistancesVersion)
    {
        u32 count;
        stream->ReadU32(&count);
        lod->count = static_cast<u8>(count);
        auto** meshes = static_cast<RigidModel**>(MemoryAllocate2(lod->count * sizeof(RigidModel*)));
        lod->meshes = A(meshes);
        ReadS32(stream, &lod->nearest);
        ReadS32(stream, &lod->farthest);
        lod->nearest = lod->nearest * lod->nearest;
        if (lod->farthest != NoDistance)
        {
            lod->farthest = lod->farthest * lod->farthest;
        }

        for (u32 distance = 0; distance < TE_GFX_LOD_SWITCHES; distance++)
        {
            u32& value = lod->switchDistances[distance];
            ReadS32(stream, &value);
            value = value * value;
        }

        s32 unused;
        stream->ReadS32(&unused);
        ReadMeshes(meshes, lod->count, stream);
        return;
    }

    if (version == LodSquaresVersion)
    {
        stream->ReadS8(reinterpret_cast<s8*>(&lod->count));
        auto** meshes = static_cast<RigidModel**>(MemoryAllocate2(lod->count * sizeof(RigidModel*)));
        lod->meshes = A(meshes);
        ReadS32(stream, &lod->nearest);
        ReadS32(stream, &lod->farthest);
        stream->Read(lod->switchDistances, sizeof(lod->switchDistances), 1);
        ReadMeshes(meshes, lod->count, stream);
    }
}

Sky* SkyConstruct(void* memory, u32 id)
{
    auto* sky = static_cast<te_gfx_sky*>(memory);
    ConstructResourceHeader(sky, id);
    sky->count = 0;
    sky->models = 0;
    return reinterpret_cast<Sky*>(sky);
}

void SkyDestroy(te_gfx_sky* sky, u32 flags)
{
    auto** models = P<RigidModel*>(sky->models);
    for (u32 index = 0; index < sky->count; index++)
    {
        ReleaseMesh(models[index]);
    }

    if (models != nullptr)
    {
        MemoryDeallocate_(models);
    }

    if ((flags & FreeAfterDestroy) != 0)
    {
        MemoryDeallocate2_(sky);
    }
}

void SkyRead(te_gfx_sky* sky, Stream* stream)
{
    u32 unused;
    stream->ReadU32(&unused);
    ReadS32(stream, &sky->count);
    auto** models = static_cast<RigidModel**>(MemoryAllocate2(sky->count * sizeof(RigidModel*)));
    sky->models = A(models);
    ReadMeshes(models, sky->count, stream);
}

void RestartMaterialAnimations(const MaterialResource* resource)
{
    auto* material = resource != nullptr ? reinterpret_cast<te_gfx_material*>(resource->material) : nullptr;
    if (material == nullptr || material->magic != TE_GFX_MATERIAL_MAGIC)
    {
        return;
    }

    for (u32 index = 0; index < material->shaderCount && index < TE_GFX_MAX_SHADERS; index++)
    {
        auto* shader = P<te_gfx_shader>(material->shaders[index]);
        if (shader != nullptr && shader->magic == TE_GFX_SHADER_MAGIC && shader->animation != 0)
        {
            RestartShaderAnimation(P<ShaderAnimation>(shader->animation));
        }
    }
}

static_assert(BlendAdditive == 0 && BlendSubtractive == 1 && BlendPageMaterial == 2 && BlendCutout == 3, "ParticleBlendMode slots");
constexpr u32 BucketParticles = 22;
constexpr u64 AlphaThreshold = 5;
constexpr u32 GsAlphaGequal = 6;
constexpr u32 GsDepthGequal = 2;

u64 SetField(u64 settings, u32 shift, u32 width, u64 value)
{
    u64 mask = ((1ull << width) - 1) << shift;
    return (settings & ~mask) | ((value << shift) & mask);
}

u64 PageAlphaTest(u64 s)
{
    s = SetField(s, 5, 1, 1);
    s = SetField(s, 6, 3, GsAlphaGequal);
    s = SetField(s, 9, 8, AlphaThreshold);
    s = SetField(s, 17, 2, 0);
    return s;
}

u64 LeaveOutExtras(u64 s)
{
    s = SetField(s, 57, 1, 0);
    s = SetField(s, 19, 1, 0);
    s = SetField(s, 32, 3, 0);
    s = SetField(s, 35, 3, 0);
    s = SetField(s, 23, 3, 0);
    s = SetField(s, 26, 1, 0);
    s = SetField(s, 29, 1, 0);
    s = SetField(s, 30, 1, 0);
    return s;
}

te_gfx_shader* NewPageShader(bool decals)
{
    auto* shader = static_cast<te_gfx_shader*>(MemoryAllocate(sizeof(te_gfx_shader)));
    *shader = te_gfx_shader{};
    shader->magic = TE_GFX_SHADER_MAGIC;
    shader->type = decals ? 0x13 : 0x12;
    shader->settings = SetField(0, 28, 1, decals ? 1 : 0);

    shader->shaderColour[0] = shader->shaderColour[1] = shader->shaderColour[2] = 0.5f;
    shader->shaderColour[3] = 1.0f;
    shader->colour[0] = shader->colour[1] = shader->colour[2] = 0x40;
    shader->colour[3] = 0x80;
    return shader;
}

te_gfx_material* MakeModeMaterial(ParticlePage* page, ParticleBlendMode mode, u64 activated)
{
    auto* material = static_cast<te_gfx_material*>(MemoryAllocate(Platform::Graphics::MaterialStorage));
    *material = te_gfx_material{};
    material->magic = TE_GFX_MATERIAL_MAGIC;
    material->kind = TE_GFX_MATERIAL_PARTICLE;
    material->bucket = BucketParticles;
    material->ownsShaders = 1;
    material->activatedShaders = activated;
    page->materials[mode] = reinterpret_cast<Material*>(material);
    return material;
}

void AddShader(te_gfx_material* material, te_gfx_shader* shader)
{
    if (material->shaderCount < TE_GFX_MAX_SHADERS)
    {
        material->shaders[material->shaderCount++] = A(shader);
    }
}

void FinishModeShader(te_gfx_shader* shader, const te_gfx_shader* pageShader)
{
    if (pageShader->texture != 0)
    {
        u32 id = P<te_gfx_texture>(pageShader->texture)->id;
        shader->textureId = id;
        shader->texture = A(g_TextureTable.Acquire(&id, nullptr));
    }

    shader->lodK = pageShader->lodK;
    shader->lodL = pageShader->lodL;
    shader->settings = SetField(shader->settings, 27, 1, 1);
}

u64 BlendedShaderSettings(u64 s, u32 preset)
{
    s = SetField(s, 0, 1, 1);
    s = SetField(s, 59, 1, 1);
    s = PageAlphaTest(s);
    s = SetField(s, 21, 2, GsDepthGequal);
    s = LeaveOutExtras(s);
    s = SetField(s, 38, 1, 0);
    s = SetField(s, 1, 4, preset);
    return s;
}

void ReadParticlePageData(ParticlePage* page, Stream* stream, bool fromStream, bool decals)
{
    te_animation::Invalidate();
    ReadS32(stream, &page->textureId);
    ReadS32(stream, &page->materialId);
    if (fromStream)
    {
        TextureFromStream(page->textureId, stream);
        page->material = MaterialFromStream(page->materialId, stream);
    }
    else
    {
        page->material = g_MaterialTable.Acquire(&page->materialId, nullptr);
    }

    auto* own = page->material != nullptr ? reinterpret_cast<te_gfx_material*>(page->material->material) : nullptr;
    page->materials[BlendPageMaterial] = reinterpret_cast<Material*>(own);
    if (own == nullptr || own->magic != TE_GFX_MATERIAL_MAGIC || own->shaderCount == 0)
    {
        page->materials[BlendAdditive] = page->materials[BlendSubtractive] = page->materials[BlendCutout] = nullptr;
        return;
    }

    own->bucket = BucketParticles;
    u64 activated = own->activatedShaders;
    auto* pageShader = P<te_gfx_shader>(own->shaders[0]);
    u64 s = pageShader->settings;
    s = SetField(s, 0, 1, 1);
    s = SetField(s, 59, 1, 1);
    s = PageAlphaTest(s);
    s = SetField(s, 21, 2, GsDepthGequal);
    pageShader->settings = s;

    te_gfx_material* material = MakeModeMaterial(page, BlendAdditive, activated);
    te_gfx_shader* shader = NewPageShader(decals);
    AddShader(material, shader);
    shader->settings = BlendedShaderSettings(shader->settings, TE_GFX_PRESET_ADD);
    FinishModeShader(shader, pageShader);

    material = MakeModeMaterial(page, BlendSubtractive, activated);

    auto* dropped = static_cast<te_gfx_shader*>(MemoryAllocate(sizeof(te_gfx_shader)));
    *dropped = te_gfx_shader{};
    dropped->magic = TE_GFX_SHADER_MAGIC;
    dropped->type = 0x12;
    te_gfx_shader* subtractive = NewPageShader(decals);
    AddShader(material, subtractive);
    subtractive->settings = BlendedShaderSettings(subtractive->settings, TE_GFX_PRESET_SUBTRACT);
    FinishModeShader(subtractive, pageShader);

    material = MakeModeMaterial(page, BlendCutout, activated);
    shader = NewPageShader(decals);
    AddShader(material, shader);
    s = shader->settings;
    s = SetField(s, 0, 1, 0);
    s = SetField(s, 59, 1, 0);
    s = PageAlphaTest(s);
    s = LeaveOutExtras(s);
    shader->settings = s;
    subtractive->settings = SetField(subtractive->settings, 21, 2, GsDepthGequal);
    FinishModeShader(shader, pageShader);
}

void FrustumPlanes(f32 depth, Vector4* planes, const Vector4* eye, const Vector4* topRight, const Vector4* bottomRight,
                   const Vector4* bottomLeft, const Vector4* topLeft)
{
    PlaneFromTriangle(&planes[NearPlane], topRight, bottomLeft, bottomRight);
    Vector4 along = {0.0f, 0.0f, depth, 1.0f};
    PlaneAlongNormal(&planes[NearPlane], &along, &planes[FarPlane]);
    planes[FarPlane].x = -planes[FarPlane].x;
    planes[FarPlane].y = -planes[FarPlane].y;
    planes[FarPlane].z = -planes[FarPlane].z;
    planes[FarPlane].w = -planes[FarPlane].w;

    PlaneFromTriangle(&planes[FirstSidePlane], eye, bottomRight, topRight);
    PlaneFromTriangle(&planes[FirstSidePlane + 1], eye, topLeft, bottomLeft);
    PlaneFromTriangle(&planes[FirstSidePlane + 2], eye, topRight, topLeft);
    PlaneFromTriangle(&planes[FirstSidePlane + 3], eye, bottomLeft, bottomRight);
}

void ViewFrustumPlanes(f32 near, f32 far, f32 aspect, f32 scale, Vector4* planes, const s32* fieldOfView)
{
    s32 half = static_cast<s32>(static_cast<f32>(*fieldOfView) * 0.5f);
    f32 height = near * TanOfAngle(&half);
    f32 width = height * aspect * scale;
    height = height * scale;
    Vector4 topRight = {width, height, near, 1.0f};
    Vector4 topLeft = {-width, height, near, 1.0f};
    Vector4 bottomRight = {width, -height, near, 1.0f};
    Vector4 bottomLeft = {-width, -height, near, 1.0f};
    Vector4 eye = {0.0f, 0.0f, 0.0f, 1.0f};
    FrustumPlanes(far - near, planes, &eye, &topRight, &bottomRight, &bottomLeft, &topLeft);
}
}

extern "C"
{
RigidModelData* te_ModelConstruct(void* memory, u32 id) { return ModelConstruct(memory, id); }
void te_ModelRead(RigidModelData* model, Stream* stream) { ModelRead(MD(model), stream); }
RigidModel* te_MeshConstruct(void* memory, u32 id) { return RigidModelConstruct(memory, id); }
void te_MeshRead(RigidModel* mesh, Stream* stream)
{
    s32 version;
    stream->ReadS32(&version);
    ReadRigidModelData(RM(mesh), stream);
}
}

namespace Platform::Graphics
{
RigidModelData* NewModel(u32 id) { return ModelConstruct(MemoryAllocate(sizeof(te_gfx_model)), id); }
void DeleteModel(RigidModelData* model) { ModelDestroy(MD(model), DestroyAndFree); }
void ReadModel(RigidModelData* model, Stream* stream) { ModelRead(MD(model), stream); }

RigidModel* NewRigidModel(u32 id) { return RigidModelConstruct(MemoryAllocate(sizeof(te_gfx_rigid_model)), id); }
void DeleteRigidModel(RigidModel* model) { RigidModelDestroy(RM(model), DestroyAndFree); }
void ReadRigidModel(RigidModel* model, Stream* stream) { RigidModelRead(RM(model), stream); }

Skin* NewSkin(u32 id) { return SkinConstruct(MemoryAllocate(sizeof(te_gfx_skin)), id); }
void DeleteSkin(Skin* skin) { SkinDestroy(SK(skin), DestroyAndFree); }
void ReadSkin(Skin* skin, Stream* stream) { SkinRead(SK(skin), stream); }

BlendSkin* NewBlendSkin(u32 id) { return BlendSkinConstruct(MemoryAllocate(sizeof(te_gfx_blend_skin)), id); }
void DeleteBlendSkin(BlendSkin* skin) { BlendSkinDestroy(BS(skin), DestroyAndFree); }
void ReadBlendSkin(BlendSkin* skin, Stream* stream) { BlendSkinRead(BS(skin), stream); }

RigidModel* NewMesh(u32 id) { return RigidModelConstruct(MemoryAllocate(sizeof(te_gfx_rigid_model)), id); }
void DeleteMesh(RigidModel* mesh) { RigidModelDestroy(RM(mesh), DestroyAndFree); }
void ReadMesh(RigidModel* mesh, Stream* stream) { te_MeshRead(mesh, stream); }

Lod* NewLod(u32 id) { return LodConstruct(MemoryAllocate(sizeof(te_gfx_lod)), id); }
void DeleteLod(Lod* lod) { LodDestroy(LD(lod), DestroyAndFree); }
void ReadLod(Lod* lod, Stream* stream) { LodRead(LD(lod), stream); }

Sky* NewSky(u32 id) { return SkyConstruct(MemoryAllocate(sizeof(te_gfx_sky)), id); }
void DeleteSky(Sky* sky) { SkyDestroy(SY(sky), DestroyAndFree); }
void ReadSky(Sky* sky, Stream* stream) { SkyRead(SY(sky), stream); }

RigidModel* LodModelAt(Lod* lodHandle, u32 distance)
{
    te_gfx_lod* lod = LD(lodHandle);
    if (lod->meshes == 0 || lod->count == 0 || distance < lod->nearest || lod->farthest < distance)
    {
        return nullptr;
    }

    auto** meshes = P<RigidModel*>(lod->meshes);
    RigidModel* mesh = meshes[0];
    s32 last = lod->count - 1;
    if (last <= 0 || distance < lod->switchDistances[0])
    {
        return mesh;
    }

    mesh = meshes[1];
    for (s32 index = 1; index < last && distance >= lod->switchDistances[index]; index++)
    {
        mesh = meshes[index + 1];
    }

    return mesh;
}

MaterialResource* FirstMaterial(const RigidModel* modelHandle)
{
    const te_gfx_rigid_model* model = reinterpret_cast<const te_gfx_rigid_model*>(modelHandle);
    return model->materials != 0 && model->count != 0 ? P<MaterialResource*>(model->materials)[0] : nullptr;
}

void RestartAnimations(RigidModel* modelHandle)
{
    te_gfx_rigid_model* model = RM(modelHandle);
    u32 count = model->count;
    const te_gfx_model* data = P<te_gfx_model>(model->model);
    if (data != nullptr && data->subModelCount < count)
    {
        count = data->subModelCount;
    }

    auto** materials = P<MaterialResource*>(model->materials);
    for (u32 index = 0; index < count; index++)
    {
        RestartMaterialAnimations(materials[index]);
    }
}

void RestartAnimations(Skin* skinHandle)
{
    te_gfx_skin* skin = SK(skinHandle);
    auto** materials = P<MaterialResource*>(skin->materials);
    for (u32 index = 0; index < skin->count; index++)
    {
        RestartMaterialAnimations(materials[index]);
    }
}

void RestartAnimations(BlendSkin* skinHandle)
{
    te_gfx_blend_skin* skin = BS(skinHandle);
    auto** materials = P<MaterialResource*>(skin->materials);
    for (u32 index = 0; index < skin->count; index++)
    {
        RestartMaterialAnimations(materials[index]);
    }
}

void LoadParticlePage(ParticlePage* page, const char* path, bool decals)
{
    MemoryStream stream;
    MemoryStream::ConstructFromFile(&stream, path, false);
    ReadParticlePageData(page, &stream, true, decals);
    stream.Destroy(DestroyOnly);
}

void ReadParticlePage(ParticlePage* page, Stream* stream, bool decals)
{
    ReadParticlePageData(page, stream, false, decals);
}

void SetViewFrustum(f32 near, f32 far, f32 aspect, const s32* fieldOfView)
{
    s32 angle = *fieldOfView;
    ViewFrustumPlanes(near, far, aspect, 1.0f, te_gfx_ViewPlanes, &angle);
    angle = *fieldOfView;
    ViewFrustumPlanes(near, far, aspect, FarSidesScale, te_gfx_FarViewPlanes, &angle);
}

const Vector4* ViewPlanes()
{
    return te_gfx_ViewPlanes;
}

void DrawChunkSky(Sky* skyHandle, RenderView* view)
{
    te_gfx_sky* sky = SY(skyHandle);
    Reference* reference = view->cameraObject;
    ReferencedObject* camera = reference != nullptr ? reference->object : nullptr;
    if (camera == nullptr || camera->place == nullptr)
    {
        return;
    }

    ObjectPlace* place = camera->place;
    place->SyncPosition();
    ps2e_gfx2_draw_sky(A(sky), A(view), A(&place->position));
}
}
