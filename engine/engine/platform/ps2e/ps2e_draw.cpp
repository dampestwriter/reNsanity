#include "engine/platform/ps2e/ps2e_draw.h"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "engine/gs/format/pixel_convert.h"
#include "engine/platform/host/platform.h"
#include "engine/gs/hle/ir/draw_ir.h"
#include "engine/gs/hle/texsrc/texture_source.h"
#include "engine/platform/ps2e/ps2e_draw_backend.h"
#include "engine/platform/sdl/window.h"
#include "engine/render/backend.h"

using namespace ps2r;

struct ps2e_draw {
    ps2e_draw_reuse_stamp reuse_stamp{1, 1};
    platform::Window window;
    std::unique_ptr<render::Backend> device;
    render::SurfaceId color, depth;
    render::SurfaceId backdrop_color, backdrop_depth;
    bool backdrop_valid = false;
    render::CdCopyState cd_copy;
    int frame_w = 0, frame_h = 0;
    int render_width = 0;
    float scale_x = 1.f;
    std::vector<ps2e_draw_vertex> effect_vertices;
    float scale = 1.f;
    int phys_w = 0, phys_h = 0;
    int drawable_w = 0, drawable_h = 0;

    std::mutex presentation_mutex;
    ps2e_draw_presentation wanted{};
    ps2e_draw_presentation applied{};
    int applied_window_mode = PS2E_DRAW_WINDOWED, applied_window_scale = 0;
    bool in_frame = false;
    std::atomic<uint32_t> next_texture{1};
    struct Texture {
        render::SurfaceId surface;
        int w = 0, h = 0;
        int levels = 0;

        uint8_t alpha_zero_levels = 0;
        uint8_t alpha_zero_extent = 0;
        int replacement_log2 = -1;
        uint64_t census[3] = {};
        render::SurfaceId yuv[3];
    };
    std::unordered_map<uint32_t, Texture> textures;

    struct TextureSlot { uint32_t id = 0; uint64_t generation = 0; Texture* texture = nullptr; };
    TextureSlot texture_slots[512];
    uint64_t texture_generation = 1;

    Texture frame_copy;
    bool frame_copy_valid = false;
    uint64_t draws = 0;


    std::atomic<uint32_t> next_mesh_reserved{1};
    std::vector<uint32_t> mesh_alias;
    std::vector<uint8_t> scratch;

    render::SurfaceId yuv_rb, yuv_g;
    uint32_t yuv_serial = 0;
    gs::DrawIr ir;
    bool ir_raster_valid = false;

    bool mesh_run = false;
    ps2e_draw_state mesh_run_state{};
    uint64_t mesh_run_draws = 0;
    uint32_t haze_palettes[8][256] = {};
    bool haze_have = false;
};

namespace {

constexpr uint8_t kStencilOrigin = 15;
constexpr uint8_t kCasterBit = 0x20;
constexpr uint8_t kCounterMask = 0x1F;
constexpr uint8_t kInsideBit = 0x10;

bool ZeroAlpha(const void* pixels, int w, int h, size_t stride) {
    const auto* bytes = static_cast<const uint8_t*>(pixels);
    if (!bytes) return false;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (bytes[size_t(y) * stride + size_t(x) * 4 + 3]) return false;
    return true;
}

ps2e_draw::Texture* FindTexture(ps2e_draw* d, uint32_t id) {
    auto& slot = d->texture_slots[id & 511u];
    if (slot.id == id && slot.generation == d->texture_generation) return slot.texture;
    const auto it = d->textures.find(id);
    if (it == d->textures.end()) return nullptr;
    slot = {id, d->texture_generation, &it->second};
    return &it->second;
}
bool RejectZeroTexture(const ps2e_draw* d, const ps2e_draw_state* st) {
    if (!st->alpha_test || st->alpha_fail != 0 ||
        st->stencil != PS2E_DRAW_STENCIL_OFF || st->dest_alpha_test || !st->tcc ||
        !st->texture || st->texture == PS2E_DRAW_TEXTURE_FRAME_COPY ||
        st->alpha_method == PS2E_DRAW_ATST_ALWAYS ||
        st->tfx < 0 || st->tfx > 3 || st->tfx == PS2E_DRAW_TFX_HIGHLIGHT) return false;
    auto it = d->textures.find(st->texture);
    if (it == d->textures.end()) return false;
    const auto& t = it->second;
    if (t.replacement_log2 >= 0 || t.yuv[0].id || t.levels < 0 || t.levels > 6) return false;
    const unsigned levels = (1u << (std::max(int(t.alpha_zero_extent), t.levels) + 1)) - 1u;
    if ((t.alpha_zero_levels & levels) != levels) return false;
    switch (st->alpha_method) {
    case PS2E_DRAW_ATST_NEVER: return true;
    case PS2E_DRAW_ATST_LESS: return st->alpha_reference == 0;
    case PS2E_DRAW_ATST_EQUAL:
    case PS2E_DRAW_ATST_GEQUAL: return st->alpha_reference != 0;
    case PS2E_DRAW_ATST_GREATER: return true;
    case PS2E_DRAW_ATST_NOTEQUAL: return st->alpha_reference == 0;
    default: return false;
    }
}

render::StencilState VolumeStencil(const ps2e_draw_state* st) {
    render::StencilState s;
    s.enabled = true;
    if (st->stencil == PS2E_DRAW_STENCIL_INSIDE) {
        s.func = render::StencilState::kEqual;
        s.ref = kInsideBit;
        s.cmp_mask = kInsideBit;
        s.write_mask = 0;
        return s;
    }

    if (st->dest_alpha_test) {
        s.func = render::StencilState::kEqual;
        s.ref = 0;
        s.cmp_mask = kCasterBit;
    } else {
        s.func = render::StencilState::kAlways;
        s.ref = 0;
        s.cmp_mask = 0xFF;
    }
    s.write_mask = kCounterMask;
    s.depth_fail_op = st->stencil == PS2E_DRAW_STENCIL_COUNT_UP ? render::StencilState::kIncrWrap
                                                                 : render::StencilState::kDecrWrap;
    return s;
}

void SeedCasterMask(ps2e_draw* d, const ps2e_draw_state* st) {
    render::Rect rect{};
    if (st->scissor_x0 || st->scissor_y0 || st->scissor_x1 || st->scissor_y1) {
        const int x0 = std::clamp(st->scissor_x0, 0, d->frame_w - 1);
        const int y0 = std::clamp(st->scissor_y0, 0, d->frame_h - 1);
        const int x1 = std::clamp(st->scissor_x1, x0, d->frame_w - 1) + 1;
        const int y1 = std::clamp(st->scissor_y1, y0, d->frame_h - 1) + 1;
        const int px = render::PhysicalEdge(x0, d->scale_x), py = render::PhysicalEdge(y0, d->scale);
        rect = {px, py, render::PhysicalEdge(x1, d->scale_x) - px, render::PhysicalEdge(y1, d->scale) - py};
    }
    d->device->MarkStencilWhereOpaque(d->color, d->depth, kCasterBit, rect);
}

uint8_t IntegerScale(float scale) {
    const int i = int(scale);
    return uint8_t(float(i) == scale && i >= 1 ? i : 1);
}

bool MakeFrameTarget(ps2e_draw& d, int w, int h, float scale_in, int width = 0) {
    ++d.reuse_stamp.resources;
    ++d.reuse_stamp.target;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return false;
    const float scale = std::isfinite(scale_in) ? std::clamp(scale_in, 1.f, 4.f) : 1.f;

    const float scale_x = width > 0 ? float(width) / float(w) : scale;
    const int pw = width > 0 ? width : render::PhysicalEdge(w, scale);
    const int ph = render::PhysicalEdge(h, scale);
    if (pw > 8192 || ph > 8192) return false;
    if (d.backdrop_color.id) d.device->DestroySurface(d.backdrop_color);
    if (d.backdrop_depth.id) d.device->DestroySurface(d.backdrop_depth);
    d.backdrop_color = {}; d.backdrop_depth = {}; d.backdrop_valid = false;
    if (d.color.id) d.device->DestroySurface(d.color);
    if (d.depth.id) d.device->DestroySurface(d.depth);
    render::SurfaceDesc cd;
    cd.width = uint32_t(pw);
    cd.height = uint32_t(ph);
    cd.scale = scale_x == scale ? IntegerScale(scale) : 1;
    cd.format = render::SurfaceFormat::Rgba8;
    cd.renderable = true;

    render::SurfaceDesc dd = cd;
    dd.format = render::SurfaceFormat::DepthStencil24_8;
    dd.renderable = false;
    d.color = d.device->CreateSurface(cd);
    d.depth = d.device->CreateSurface(dd);
    if (d.frame_copy.surface.id) d.device->DestroySurface(d.frame_copy.surface);
    render::SurfaceDesc fc = cd;
    fc.renderable = false;
    d.frame_copy = ps2e_draw::Texture{};
    d.frame_copy.surface = d.device->CreateSurface(fc);
    d.frame_copy.w = int(cd.width);
    d.frame_copy.h = int(cd.height);
    if (d.frame_copy.surface.id) {
        std::vector<uint8_t> zero(size_t(cd.width) * cd.height * 4, 0);
        d.device->Upload(d.frame_copy.surface, render::Rect{0, 0, int(cd.width), int(cd.height)}, zero.data(), size_t(cd.width) * 4);
    }
    d.frame_copy_valid = false;
    d.cd_copy = render::CdCopyState{};
    d.frame_w = w;
    d.frame_h = h;
    d.scale = scale;
    d.scale_x = scale_x;
    d.render_width = width;
    d.phys_w = pw;
    d.phys_h = ph;
    if (!d.color.id || !d.depth.id) return false;
    d.device->Clear(d.color, d.depth, 0x80000000u, 0.0f, render::kClearColor | render::kClearDepth);
    return true;
}

void PresentWindow(const ps2e_draw& d, int* ww, int* wh) {
    int w = d.frame_w, h = d.frame_h;
    if (d.drawable_w > 0 && d.drawable_h > 0) {
        w = d.drawable_w;
        h = d.drawable_h;
    }
    if (w <= 0 || h <= 0) { w = d.frame_w; h = d.frame_h; }
    *ww = w;
    *wh = h;
}

void PictureSize(const ps2e_draw& d, const ps2e_draw_presentation& want, int ww, int wh, int* out_w, int* out_h) {
    double aspect = double(d.frame_w) / d.frame_h;
    switch (want.aspect) {
    case PS2E_DRAW_ASPECT_4_3: aspect = 4.0 / 3.0; break;
    case PS2E_DRAW_ASPECT_16_9: aspect = 16.0 / 9.0; break;
    case PS2E_DRAW_ASPECT_16_10: aspect = 16.0 / 10.0; break;
    case PS2E_DRAW_ASPECT_21_9: aspect = 21.0 / 9.0; break;
    case PS2E_DRAW_ASPECT_WINDOW: aspect = double(ww) / wh; break;
    default: break;
    }
    int dw = 1, dh = 1;
    if (want.fit == PS2E_DRAW_FIT_FILL) {

        if (double(ww) / wh > aspect) { dw = ww; dh = std::max(1, int(ww / aspect + 0.5)); }
        else { dh = wh; dw = std::max(1, int(wh * aspect + 0.5)); }
    } else {

        if (double(ww) / wh > aspect) { dh = wh; dw = std::max(1, int(wh * aspect + 0.5)); }
        else { dw = ww; dh = std::max(1, int(ww / aspect + 0.5)); }
        if (want.fit == PS2E_DRAW_FIT_INTEGER) {

            const int k = std::max(1, dh / d.frame_h);
            dh = k * d.frame_h;
            dw = std::max(1, int(dh * aspect + 0.5));
        }
    }
    *out_w = dw;
    *out_h = dh;
}

float WantedScale(const ps2e_draw& d, const ps2e_draw_presentation& want) {
    if (want.render_scale > 0.f) return std::clamp(want.render_scale, 1.f, 4.f);
    if (want.render_scale < 0.f && d.frame_h > 0) {
        int ww = 0, wh = 0, dw = 0, dh = 0;
        PresentWindow(d, &ww, &wh);
        PictureSize(d, want, ww, wh, &dw, &dh);
        return std::clamp(float(dh) / float(d.frame_h), 1.f, 4.f);
    }
    return float(std::clamp(want.scale, 1, 4));
}

void ScaleAlpha(const uint8_t* in, size_t stride_in, int w, int h, std::vector<uint8_t>& out) {
    out.resize(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = in + size_t(y) * stride_in;
        uint8_t* dst = out.data() + size_t(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            dst[x * 4 + 0] = row[x * 4 + 0];
            dst[x * 4 + 1] = row[x * 4 + 1];
            dst[x * 4 + 2] = row[x * 4 + 2];
            dst[x * 4 + 3] = gs::PixelConvert::ScaleAlphaPs2ToGpu(row[x * 4 + 3]);
        }
    }
}


}

namespace {

}

extern "C" {

ps2e_draw* ps2e_draw_create(const ps2e_draw_config* config) {
    if (!config || config->frame_width <= 0 || config->frame_height <= 0) return nullptr;
    auto d = std::make_unique<ps2e_draw>();
    if (!d->window.Create()) return nullptr;
    d->device = ps2e::CreateDrawBackend(d->window);
    if (!d->device) return nullptr;

    d->device->SetWindowSize(d->window.Width(), d->window.Height());
    if (!MakeFrameTarget(*d, config->frame_width, config->frame_height, 1.f)) return nullptr;
    d->window.DrawableSize(&d->drawable_w, &d->drawable_h);
    return d.release();
}

void ps2e_draw_destroy(ps2e_draw* d) {
    if (!d) return;
    if (d->device) {
        for (auto& t : d->textures) {
            d->device->DestroySurface(t.second.surface);
            for (const render::SurfaceId& plane : t.second.yuv)
                if (plane.id) d->device->DestroySurface(plane);
        }
        if (d->yuv_rb.id) d->device->DestroySurface(d->yuv_rb);
        if (d->yuv_g.id) d->device->DestroySurface(d->yuv_g);
        if (d->frame_copy.surface.id) d->device->DestroySurface(d->frame_copy.surface);
        if (d->backdrop_color.id) d->device->DestroySurface(d->backdrop_color);
        if (d->backdrop_depth.id) d->device->DestroySurface(d->backdrop_depth);
        if (d->color.id) d->device->DestroySurface(d->color);
        if (d->depth.id) d->device->DestroySurface(d->depth);
        d->device.reset();
    }
    d->window.Destroy();
    delete d;
}

int ps2e_draw_pump(ps2e_draw* d) {
    if (!d) return 0;
    int quit = 0;

    SDL_PumpEvents();
    SDL_Event e;
    while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_QUIT, SDL_QUIT) > 0) quit = 1;
    while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_WINDOWEVENT, SDL_WINDOWEVENT) > 0) {
        {
            if (e.window.event == SDL_WINDOWEVENT_CLOSE) quit = 1;
            if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e.window.event == SDL_WINDOWEVENT_RESIZED) {
                int w = 0, h = 0;
                d->window.DrawableSize(&w, &h);
                if (w > 0 && h > 0) {
                    d->drawable_w = w;
                    d->drawable_h = h;
                }
            }
        }
    }
    if (SDL_PeepEvents(nullptr, 0, SDL_PEEKEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT) > 512) SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);

    int w = 0, h = 0;
    d->window.DrawableSize(&w, &h);
    if (w > 0 && h > 0) {
        d->drawable_w = w;
        d->drawable_h = h;
    }
    return quit;
}

int ps2e_draw_bind_thread(ps2e_draw* d, int) { return d ? 1 : 0; }

int ps2e_draw_resize_frame(ps2e_draw* d, int w, int h) {
    if (!d || d->in_frame) return 0;
    if (w == d->frame_w && h == d->frame_h) return 1;
    return MakeFrameTarget(*d, w, h, d->scale, d->render_width) ? 1 : 0;
}

void ps2e_draw_frame_size(const ps2e_draw* d, int* w, int* h) {
    if (w) *w = d ? d->frame_w : 0;
    if (h) *h = d ? d->frame_h : 0;
}

uint32_t ps2e_draw_texture_create(ps2e_draw* d, int w, int h, const void* rgba8) {
    if (d) ++d->reuse_stamp.resources;
    if (!d || w <= 0 || h <= 0 || w > 4096 || h > 4096) return 0;
    render::SurfaceDesc td;
    td.width = uint32_t(w);
    td.height = uint32_t(h);
    td.format = render::SurfaceFormat::Rgba8;
    td.renderable = false;
    const render::SurfaceId s = d->device->CreateSurface(td);
    if (!s.id) return 0;
    const uint32_t id = d->next_texture++;
    ps2e_draw::Texture& t = d->textures[id];
    t.surface = s;
    t.w = w;
    t.h = h;
    if (rgba8) ps2e_draw_texture_upload(d, id, 0, 0, w, h, rgba8, size_t(w) * 4);
    return id;
}

uint32_t ps2e_draw_texture_create_gpu_alpha(ps2e_draw* d, int w, int h, const void* rgba8) {
    if (d) ++d->reuse_stamp.resources;
    const uint32_t id = ps2e_draw_texture_create(d, w, h, nullptr);
    if (!id || !rgba8) return id;
    auto it = d->textures.find(id);
    d->device->Upload(it->second.surface, render::Rect{0, 0, w, h}, rgba8, size_t(w) * 4);
    it->second.alpha_zero_levels = ZeroAlpha(rgba8, w, h, size_t(w) * 4) ? 1 : 0;
    return id;
}

int ps2e_draw_texture_upload(ps2e_draw* d, uint32_t texture, int x, int y, int w, int h, const void* rgba8,
                             size_t stride) {
    if (d) ++d->reuse_stamp.resources;
    if (!d || !rgba8 || w <= 0 || h <= 0) return 0;
    auto it = d->textures.find(texture);
    if (it == d->textures.end()) return 0;
    if (x < 0 || y < 0 || x + w > it->second.w || y + h > it->second.h) return 0;
    if (stride == 0) stride = size_t(w) * 4;

    it->second.alpha_zero_levels &= uint8_t(~1u);
    if (x == 0 && y == 0 && w == it->second.w && h == it->second.h && ZeroAlpha(rgba8, w, h, stride))
        it->second.alpha_zero_levels |= 1;
    ScaleAlpha(static_cast<const uint8_t*>(rgba8), stride, w, h, d->scratch);
    d->device->Upload(it->second.surface, render::Rect{x, y, w, h}, d->scratch.data(), size_t(w) * 4);
    return 1;
}

int ps2e_draw_yuv420_supported(ps2e_draw* d) {
    return d && d->device && d->device->QueryCaps().yuv420_convert ? 1 : 0;
}

int ps2e_draw_yuv_tables(ps2e_draw* d, uint32_t serial, const uint8_t* rb, const uint8_t* g) {
    if (d) ++d->reuse_stamp.resources;
    if (!ps2e_draw_yuv420_supported(d) || !serial || !rb || !g) return 0;
    if (serial == d->yuv_serial && d->yuv_rb.id && d->yuv_g.id) return 1;
    const auto make = [d](render::SurfaceId& s, uint32_t w, uint32_t h, const uint8_t* bytes) {
        if (!s.id) {
            render::SurfaceDesc sd;
            sd.width = w;
            sd.height = h;
            sd.format = render::SurfaceFormat::R8;
            sd.renderable = false;
            s = d->device->CreateSurface(sd);
            if (!s.id) return false;
        }
        d->device->Upload(s, render::Rect{0, 0, int(w), int(h)}, bytes, w);
        return true;
    };
    if (!make(d->yuv_rb, 256, 512, rb) || !make(d->yuv_g, 4096, 4096, g)) {
        d->yuv_serial = 0;
        return 0;
    }
    d->yuv_serial = serial;
    return 1;
}

uint32_t ps2e_draw_texture_create_yuv420(ps2e_draw* d, int w, int h) {
    if (d) ++d->reuse_stamp.resources;
    if (!ps2e_draw_yuv420_supported(d) || w < 2 || h < 2 || (w & 1) || (h & 1) || w > 4096 || h > 4096) return 0;
    render::SurfaceDesc td;
    td.width = uint32_t(w);
    td.height = uint32_t(h);
    td.format = render::SurfaceFormat::Rgba8;
    td.renderable = true;
    const render::SurfaceId s = d->device->CreateSurface(td);
    if (!s.id) return 0;
    render::SurfaceId planes[3];
    for (int i = 0; i < 3; ++i) {
        render::SurfaceDesc pd;
        pd.width = uint32_t(i ? w / 2 : w);
        pd.height = uint32_t(i ? h / 2 : h);
        pd.format = render::SurfaceFormat::R8;
        pd.renderable = false;
        planes[i] = d->device->CreateSurface(pd);
        if (!planes[i].id) {
            for (int j = 0; j < i; ++j) d->device->DestroySurface(planes[j]);
            d->device->DestroySurface(s);
            return 0;
        }
    }
    const uint32_t id = d->next_texture++;
    ps2e_draw::Texture& t = d->textures[id];
    t.surface = s;
    t.w = w;
    t.h = h;
    for (int i = 0; i < 3; ++i) t.yuv[i] = planes[i];
    return id;
}

int ps2e_draw_texture_upload_yuv420(ps2e_draw* d, uint32_t texture, const uint8_t* yuv420) {
    if (d) ++d->reuse_stamp.resources;
    if (!d || !yuv420 || !d->yuv_serial) return 0;
    auto it = d->textures.find(texture);
    if (it == d->textures.end() || !it->second.yuv[0].id) return 0;
    const ps2e_draw::Texture& t = it->second;
    const int cw = t.w / 2, ch = t.h / 2;
    const uint8_t* u = yuv420 + size_t(t.w) * size_t(t.h);
    const uint8_t* v = u + size_t(cw) * size_t(ch);
    d->device->Upload(t.yuv[0], render::Rect{0, 0, t.w, t.h}, yuv420, size_t(t.w));
    d->device->Upload(t.yuv[1], render::Rect{0, 0, cw, ch}, u, size_t(cw));
    d->device->Upload(t.yuv[2], render::Rect{0, 0, cw, ch}, v, size_t(cw));
    return d->device->ConvertYuv420(t.surface, t.yuv[0], t.yuv[1], t.yuv[2], d->yuv_rb, d->yuv_g) ? 1 : 0;
}

uint32_t ps2e_draw_texture_reserve(ps2e_draw* d) { return d ? d->next_texture++ : 0; }

int ps2e_draw_texture_rename(ps2e_draw* d, uint32_t from, uint32_t to) {
    if (d) ++d->reuse_stamp.resources;
    if (!d || !from || !to || from == to) return 0;
    auto it = d->textures.find(from);
    if (it == d->textures.end() || d->textures.count(to)) return 0;
    ps2e_draw::Texture t = std::move(it->second);
    d->textures.erase(it);
    d->texture_generation++;
    d->textures.emplace(to, std::move(t));
    return 1;
}

int ps2e_draw_texture_readback(ps2e_draw* d, uint32_t texture, void* rgba8) {
    if (!d || !rgba8) return 0;
    auto it = d->textures.find(texture);
    if (it == d->textures.end() || !it->second.yuv[0].id) return 0;
    d->device->Readback(it->second.surface, render::Rect{0, 0, it->second.w, it->second.h}, rgba8, size_t(it->second.w) * 4);
    return 1;
}

int ps2e_draw_texture_set_mips(ps2e_draw* d, uint32_t texture, int count, const void* const* levels) {
    if (d) ++d->reuse_stamp.resources;
    if (!d || count < 0 || count > 6 || (count && !levels)) return 0;
    auto it = d->textures.find(texture);
    if (it == d->textures.end()) return 0;
    ps2e_draw::Texture& t = it->second;
    for (int i = 0; i < count; ++i) if (!levels[i]) return 0;
    t.alpha_zero_extent = uint8_t(std::max(int(t.alpha_zero_extent), count));
    t.alpha_zero_levels &= 1;
    for (int i = 1; i <= count; ++i)
        if (ZeroAlpha(levels[i - 1], std::max(t.w >> i, 1), std::max(t.h >> i, 1),
                      size_t(std::max(t.w >> i, 1)) * 4)) t.alpha_zero_levels |= uint8_t(1u << i);
    for (int i = 1; i <= count; ++i) {
        if (!levels[i - 1]) return 0;
        d->device->UploadLevel(t.surface, i, std::max(t.w >> i, 1), std::max(t.h >> i, 1), levels[i - 1]);
    }
    d->device->FinishMipChain(t.surface, count, false);

    t.levels = count;
    return 1;
}

uint32_t ps2e_draw_texture_create_replacement(ps2e_draw* d, int w, int h, int scale, int format, int levels,
                                              const void* const* data, const size_t* sizes) {
    if (d) ++d->reuse_stamp.resources;
    if (!d || w <= 0 || h <= 0 || scale <= 0 || (scale & (scale - 1)) || levels <= 0 || !data || !sizes) return 0;
    const render::SurfaceId s = d->device->CreateCompressed(uint32_t(w * scale), uint32_t(h * scale), uint32_t(format), levels,
                                                            data, sizes);
    if (!s.id) return 0;
    const uint32_t id = d->next_texture++;
    ps2e_draw::Texture& t = d->textures[id];
    t.surface = s;
    t.w = w;
    t.h = h;
    t.levels = levels - 1;
    t.replacement_log2 = 0;
    while ((1 << t.replacement_log2) < scale) t.replacement_log2++;
    return id;
}

int ps2e_draw_texture_census(const ps2e_draw* d, uint32_t texture, uint64_t out[3]) {
    if (!d || !out) return 0;
    auto it = d->textures.find(texture);
    if (it == d->textures.end()) return 0;
    for (int i = 0; i < 3; ++i) out[i] = it->second.census[i];
    return 1;
}

void ps2e_draw_texture_destroy(ps2e_draw* d, uint32_t texture) {
    if (d) ++d->reuse_stamp.resources;
    if (!d) return;
    auto it = d->textures.find(texture);
    if (it == d->textures.end()) return;
    d->device->DestroySurface(it->second.surface);
    for (const render::SurfaceId& plane : it->second.yuv)
        if (plane.id) d->device->DestroySurface(plane);
    d->textures.erase(it);
    d->texture_generation++;
}

void ps2e_draw_begin_frame(ps2e_draw* d, const ps2e_draw_clear* clear) {
    if (!d) return;
    d->mesh_run = false;
    if (d->in_frame) d->device->EndDrawRun();

    {
        std::lock_guard<std::mutex> lock(d->presentation_mutex);
        const float want_scale = WantedScale(*d, d->wanted);
        const float want_x = d->wanted.render_width > 0 ? float(d->wanted.render_width) / float(d->frame_w) : want_scale;
        if (want_scale != d->scale || want_x != d->scale_x || d->wanted.render_width != d->render_width)
            MakeFrameTarget(*d, d->frame_w, d->frame_h, want_scale, d->wanted.render_width);
        if (d->applied.texture_filter != d->wanted.texture_filter) ++d->reuse_stamp.resources;
        d->applied.texture_filter = d->wanted.texture_filter;
        d->applied.haze_direct = d->wanted.haze_direct;
    }
    d->device->BeginFrame();
    d->in_frame = true;
    d->frame_copy_valid = false;
    if (clear && (clear->clear_colour || clear->clear_depth)) {
        ++d->reuse_stamp.target;
        uint32_t flags = 0;
        if (clear->clear_colour) flags |= render::kClearColor;
        if (clear->clear_depth) flags |= render::kClearDepth;

        const uint32_t rgba = uint32_t(clear->red) | uint32_t(clear->green) << 8 | uint32_t(clear->blue) << 16 |
                              0xFFu << 24;
        d->device->Clear(d->color, d->depth, rgba, 0.0f, flags);
    }
    d->device->BeginDrawRun();
}

ps2e_draw_reuse_stamp ps2e_draw_get_reuse_stamp(const ps2e_draw* d) {
    return d ? d->reuse_stamp : ps2e_draw_reuse_stamp{};
}

int ps2e_draw_backdrop(ps2e_draw* d, int restore) {
    if (!d || !d->in_frame) return 0;
    d->mesh_run = false;
    d->device->EndDrawRun();
    if (!restore && !d->backdrop_color.id) {
        auto c = d->device->DescribeSurface(d->color);
        auto z = d->device->DescribeSurface(d->depth);
        d->backdrop_color = d->device->CreateSurface(c);
        d->backdrop_depth = d->device->CreateSurface(z);
    }
    bool ok = false;
    if (restore && d->backdrop_valid) {
        ok = d->device->CopyAttachments(d->backdrop_color, d->backdrop_depth, d->color, d->depth);
        if (ok) { ++d->reuse_stamp.target; d->cd_copy = {}; }
    } else if (!restore && d->backdrop_color.id && d->backdrop_depth.id) {
        ok = d->device->CopyAttachments(d->color, d->depth, d->backdrop_color, d->backdrop_depth);
        d->backdrop_valid = ok;
    }
    d->device->BeginDrawRun();
    return ok ? 1 : 0;
}

static uint32_t RoundedLog2(int n) {
    if (n <= 0) return uint32_t(std::lround(std::log2(double(n))));
    const uint32_t k = 31u - uint32_t(__builtin_clz(uint32_t(n)));
    return uint64_t(n) * uint64_t(n) >= (uint64_t(1) << (2 * k + 1)) ? k + 1 : k;
}

static void PrepareRaster(ps2e_draw* d, const ps2e_draw_state* st) {

    gs::DrawIr& ir = d->ir;

    ir.tex = gs::TexReadView{};
    ir.tex_enabled = false;
    ir.vertex_count = 0;
    ir.prim_type = 3;
    ir.gouraud = st->gouraud != 0;
    ir.alpha_blend = st->blend != 0;
    ir.use_fst = false;
    ir.ctx = 0;
    ir.off_x = ir.off_y = 0.f;
    ir.raster.frame.fbp = 0;
    ir.raster.frame.fbw = uint32_t(d->frame_w);
    ir.raster.frame.psm = 0;
    ir.raster.frame.fbmsk = st->no_colour_write ? 0xFFFFFFFFu : 0u;

    ir.raster.zbuf.zbp = uint32_t(d->frame_w) * uint32_t(d->frame_h) * 4u;
    ir.raster.zbuf.psm = 0;
    ir.raster.zbuf.zmsk = st->depth_write == 0;
    ir.raster.test.zte = true;
    ir.raster.test.ztst = uint8_t(std::clamp(st->depth_test, 0, 3));
    ir.raster.test.ate = st->alpha_test != 0;
    ir.raster.test.atst = uint8_t(std::clamp(st->alpha_method, 0, 7));
    ir.raster.test.aref = st->alpha_reference;
    ir.raster.test.afail = uint8_t(std::clamp(st->alpha_fail, 0, 3));
    ir.raster.alpha.a = uint8_t(st->blend_a & 3);
    ir.raster.alpha.b = uint8_t(st->blend_b & 3);
    ir.raster.alpha.c = uint8_t(st->blend_c & 3);
    ir.raster.alpha.d = uint8_t(st->blend_d & 3);
    ir.raster.alpha.fix = st->blend_fix;
    ir.raster.colclamp = true;

    ir.raster.fba = st->fba != 0 && st->blend == 0;
    if (st->scissor_x0 == 0 && st->scissor_y0 == 0 && st->scissor_x1 == 0 && st->scissor_y1 == 0) {
        ir.raster.scissor = {0, 0, uint16_t(d->frame_w - 1), uint16_t(d->frame_h - 1)};
    } else {
        ir.raster.scissor = {uint16_t(std::clamp(st->scissor_x0, 0, d->frame_w - 1)),
                             uint16_t(std::clamp(st->scissor_y0, 0, d->frame_h - 1)),
                             uint16_t(std::clamp(st->scissor_x1, 0, d->frame_w - 1)),
                             uint16_t(std::clamp(st->scissor_y1, 0, d->frame_h - 1))};
    }

}

static int PrepareTexture(ps2e_draw* d, const ps2e_draw_state* st, gs::TextureSource& ts) {
    gs::DrawIr& ir = d->ir;

    ir.tex = gs::TexReadView{};
    ir.tex_enabled = false;
    ps2e_draw::Texture* tex = nullptr;
    if (st->texture == PS2E_DRAW_TEXTURE_FRAME_COPY) {

        if (!d->frame_copy_valid || !d->frame_copy.surface.id) {
            return -2;
        }
        tex = &d->frame_copy;
    } else if (st->texture) {
        tex = FindTexture(d, st->texture);
        if (!tex) {
            return -2;
        }
    }
    if (tex) {
        ir.tex_enabled = true;
        ir.tex.fst = false;
        ir.tex.tfx = uint32_t(std::clamp(st->tfx, 0, 3));
        ir.tex.tcc = st->tcc ? 1u : 0u;
        ir.tex.psm = 0;
        ir.tex.tw = RoundedLog2(tex->w);
        ir.tex.th = RoundedLog2(tex->h);
        ir.tex.tbw = uint32_t(tex->w);
        ir.tex.mmag = st->linear != 0;
        ir.tex.mmin = st->linear ? 1 : 0;
        ir.tex.wms = uint8_t(st->wrap_u ? 1 : 0);
        ir.tex.wmt = uint8_t(st->wrap_v ? 1 : 0);
        ts.enabled = true;
        ts.owner.kind = gs::TextureSource::OwnerKind::Decoded;
        ts.owner.surface = tex->surface;
        ts.owner.owned = true;
        ts.transform.uv_tex_w = ts.transform.uv_pre_w = tex->w;
        ts.transform.uv_tex_h = ts.transform.uv_pre_h = tex->h;
        ts.transform.draw_scale = 1;

        const bool linear = d->applied.texture_filter == 1 ? false : d->applied.texture_filter == 2 ? true : st->linear != 0;
        ts.sampler.mag_linear = linear;
        ts.sampler.mmin = linear ? 1 : 0;
        ts.sampler.wms = uint8_t(st->wrap_u ? 1 : 0);
        ts.sampler.wmt = uint8_t(st->wrap_v ? 1 : 0);

        const int mxl = std::min(st->mip_max_level, tex->levels);
        if (mxl > 0 && st->mip_minify >= 2 && st->mip_minify <= 5) {
            ts.owner.levels = uint8_t(tex->levels);
            ts.transform.mip_levels = uint8_t(mxl);
            ts.transform.mip_lcm = false;
            ts.transform.mip_k = int16_t(int32_t(uint32_t(st->mip_k) << 20) >> 20);
            ts.transform.mip_l = uint8_t(st->mip_l & 3);

            ts.sampler.mmin = uint8_t(d->applied.texture_filter == 1 ? 2 : d->applied.texture_filter == 2 ? 4 : st->mip_minify);
            ir.tex.mmin = ts.sampler.mmin;
        }

        if (tex->replacement_log2 >= 0 && tex->levels > 0) {
            if (ts.transform.mip_levels > 0) {
                ts.transform.mip_k = int16_t(ts.transform.mip_k + 16 * tex->replacement_log2);
                ts.transform.mip_levels = uint8_t(std::min(int(ts.transform.mip_levels) + tex->replacement_log2, tex->levels));
            } else if (linear) {
                ts.sampler.mmin = 5;
                ts.owner.levels = uint8_t(tex->levels);
            }
        }

        const int kind = ts.transform.mip_levels > 0 ? 2 : linear ? 1 : 0;
        tex->census[kind]++;
    }

    return 0;
}

static int PrepareRecord(ps2e_draw* d, const ps2e_draw_state* st, gs::TextureSource& ts) {
    d->ir_raster_valid = false;
    PrepareRaster(d, st);
    return PrepareTexture(d, st, ts);
}

static bool RestEqual(const ps2e_draw_state* a, const ps2e_draw_state* b) {
    static_assert(sizeof(ps2e_draw_state) == 120, "a field added to ps2e_draw_state is a texture field or not: RestEqual");
    return a->gouraud == b->gouraud && a->depth_test == b->depth_test && a->depth_write == b->depth_write &&
           a->blend == b->blend && a->blend_a == b->blend_a && a->blend_b == b->blend_b && a->blend_c == b->blend_c &&
           a->blend_d == b->blend_d && a->blend_fix == b->blend_fix && a->alpha_test == b->alpha_test &&
           a->alpha_method == b->alpha_method && a->alpha_reference == b->alpha_reference && a->alpha_fail == b->alpha_fail &&
           a->scissor_x0 == b->scissor_x0 && a->scissor_y0 == b->scissor_y0 && a->scissor_x1 == b->scissor_x1 &&
           a->scissor_y1 == b->scissor_y1 && a->no_colour_write == b->no_colour_write && a->stencil == b->stencil &&
           a->fba == b->fba && a->dest_alpha_test == b->dest_alpha_test && a->haze == b->haze && a->haze_flat == b->haze_flat;
}

static render::DrawParams TargetParams(ps2e_draw* d, const ps2e_draw_state* st) {
    render::DrawParams p;
    p.color = d->color;
    p.depth = d->depth;
    p.render_w = p.target_w = d->frame_w;
    p.render_h = p.target_h = d->frame_h;

    p.scale = d->scale_x == d->scale ? IntegerScale(d->scale) : 1;
    p.scale_exact = d->scale;
    p.scale_exact_x = d->render_width > 0 ? d->scale_x : 0.f;
    p.has_32bit_content = true;
    p.cd_copy = &d->cd_copy;

    if (st->stencil != PS2E_DRAW_STENCIL_OFF) p.stencil_state = VolumeStencil(st);
    return p;
}

void ps2e_draw_haze_palettes(ps2e_draw* d, const uint32_t* palettes) {
    if (!d || !palettes) return;
    std::memcpy(d->haze_palettes, palettes, sizeof d->haze_palettes);
    d->haze_have = true;
}

int ps2e_draw_haze_supported(const ps2e_draw* d) { return d && d->device && d->device->QueryCaps().depth_haze ? 1 : 0; }

namespace {

int SubmitHaze(ps2e_draw* d, const ps2e_draw_state* st) {
    const int n = st->haze - 1;
    if (!d->haze_have || n < 0 || n > 7) return -1;
    uint32_t logical[256];
    for (uint32_t c = 0; c < 256; ++c) {
        const uint32_t at = (c & ~0x18u) | ((c & 8u) << 1) | ((c & 0x10u) >> 1);
        uint32_t w = d->haze_palettes[n][at];
        if (st->haze_flat & 0x01000000u) w = (w & 0xFF000000u) | (st->haze_flat & 0x00FFFFFFu);
        logical[c] = w;
    }
    d->mesh_run = false;
    if (!d->device->DepthHaze(d->color, d->depth, logical, d->applied.haze_direct != 0)) return -1;
    d->draws++;
    return 0;
}
}

int ps2e_draw_submit(ps2e_draw* d, const ps2e_draw_state* st, const ps2e_draw_vertex* v, int count) {
    if (d) ++d->reuse_stamp.target;
    if (!d || !st || !v || count < 3) {
        return -1;
    }
    if (!d->in_frame) ps2e_draw_begin_frame(d, nullptr);
    if (st->haze) return SubmitHaze(d, st);
    if (st->stencil == PS2E_DRAW_STENCIL_CLEAR) {

        d->device->ClearStencil(d->depth, kStencilOrigin);
        SeedCasterMask(d, st);
        d->draws++;
        return 0;
    }
    const int tri_verts = (count / 3) * 3;
    if (RejectZeroTexture(d, st)) {
        d->mesh_run = false;
        return tri_verts / 3;
    }

    if (count == 6 && d->depth.id && st->no_colour_write && st->depth_write &&
        st->depth_test == PS2E_DRAW_DEPTH_ALWAYS && !st->texture && !st->alpha_test && !st->blend && !st->fba &&
        !st->dest_alpha_test && st->stencil == PS2E_DRAW_STENCIL_OFF &&
        !st->scissor_x0 && !st->scissor_y0 && !st->scissor_x1 && !st->scissor_y1 &&
        float(d->frame_w) * d->scale_x == float(d->phys_w) && float(d->frame_h) * d->scale == float(d->phys_h)) {
        const float w = float(d->frame_w), h = float(d->frame_h);
        const float xy[6][2] = {{0.f, 0.f}, {w, 0.f}, {0.f, h}, {w, 0.f}, {w, h}, {0.f, h}};
        bool exact = true;
        for (int i = 0; i < 6; ++i)
            exact = exact && v[i].x == xy[i][0] && v[i].y == xy[i][1] && v[i].z == 0.f;
        if (exact) {
            d->device->EndDrawRun();
            d->mesh_run = false;
            d->device->Clear(d->color, d->depth, 0, 0.f, render::kClearDepth);
            d->device->BeginDrawRun();
            d->draws++;
            return 2;
        }
    }
    gs::DrawIr& ir = d->ir;
    gs::TextureSource ts;
    if (PrepareRecord(d, st, ts) < 0) return -2;
    const render::DrawParams p = TargetParams(d, st);
    {

        static_assert(sizeof(ps2e_draw_vertex) == sizeof(render::RawVertex), "the producer's vertex is the raw layout");
        if (d->device->DrawsRaw(ir, ts, p)) {
            float box[4] = {v[0].x, v[0].y, v[0].x, v[0].y};
            bool finite = true;
            for (int i = 0; i < tri_verts; ++i) {
                const float x = v[i].x, y = v[i].y;
                box[0] = std::min(box[0], x);
                box[1] = std::min(box[1], y);
                box[2] = std::max(box[2], x);
                box[3] = std::max(box[3], y);
                if (!std::isfinite(v[i].q)) finite = false;
            }
            if (finite) {
                ir.verts.clear();
                ir.vertex_count = uint32_t(tri_verts);
                const render::DrawResult r =
                    d->device->DrawRaw(ir, ts, p, reinterpret_cast<const render::RawVertex*>(v), size_t(tri_verts), box);
                d->draws++;
                if (!r.drew) {
                    return -3;
                }
                return tri_verts / 3;
            }
        }
    }

    ir.verts.clear();
    if (ir.verts.capacity() < size_t(tri_verts)) ir.verts.reserve(std::max(size_t(tri_verts), ir.verts.capacity() * 2));

    struct alignas(4) Rec {
        float x, y, z, s, t, q;
        uint16_t uv_u, uv_v;
        uint8_t r, g, b, a, fog, draw_kick, pad[2];
    };
    static_assert(sizeof(Rec) == sizeof(gs::VertexRec), "Rec mirrors VertexRec's layout");
    for (int i = 0; i < tri_verts; ++i) {
        const ps2e_draw_vertex& in = v[i];
        Rec rec;
        rec.x = in.x;
        rec.y = in.y;
        rec.z = in.z;

        rec.q = (std::isfinite(in.q) && in.q != 0.f) ? in.q : 1.f;
        rec.s = in.u;
        rec.t = in.v;

        rec.uv_u = rec.uv_v = 0;
        rec.r = in.r;
        rec.g = in.g;
        rec.b = in.b;
        rec.a = in.a;
        rec.fog = 0;
        rec.draw_kick = 1;
        rec.pad[0] = rec.pad[1] = 0;
        gs::VertexRec vertex;
        std::memcpy(&vertex, &rec, sizeof vertex);
        ir.verts.push_back(vertex);
    }
    ir.vertex_count = uint32_t(tri_verts);

    const render::DrawResult r = d->device->Draw(ir, ts, p);
    d->draws++;
    if (!r.drew) {
        return -3;
    }
    return tri_verts / 3;
}

uint32_t ps2e_draw_mesh_create(ps2e_draw* d, const ps2e_draw_mesh_vertex* verts, int count) {
    if (d) ++d->reuse_stamp.resources;
    static_assert(sizeof(ps2e_draw_mesh_vertex) == sizeof(render::MeshVertex), "the mesh vertex is the backend's");
    if (!d || !d->device || !verts || count < 3) return 0;
    const uint32_t mesh = d->device->CreateMesh(reinterpret_cast<const render::MeshVertex*>(verts), size_t(count / 3 * 3));
    return mesh;
}

namespace {
constexpr uint32_t kReservedMesh = 0x80000000u;

uint32_t RealMesh(const ps2e_draw* d, uint32_t mesh) {
    if (!(mesh & kReservedMesh)) return mesh;
    const size_t i = mesh & ~kReservedMesh;
    return i < d->mesh_alias.size() ? d->mesh_alias[i] : 0;
}
void AliasMesh(ps2e_draw* d, uint32_t reserved, uint32_t real) {
    const size_t i = reserved & ~kReservedMesh;
    if (d->mesh_alias.size() <= i) d->mesh_alias.resize(i + 1, 0);
    d->mesh_alias[i] = real;
}
}

uint32_t ps2e_draw_mesh_reserve(ps2e_draw* d) { return d ? (kReservedMesh | d->next_mesh_reserved++) : 0; }

int ps2e_draw_mesh_create_at(ps2e_draw* d, uint32_t reserved, const ps2e_draw_mesh_vertex* verts, int count) {
    if (!d || !(reserved & kReservedMesh)) return 0;
    if (d) ++d->reuse_stamp.resources;
    if (!d->device || !verts || count < 3) return 0;
    const uint32_t mesh = d->device->CreateMesh(reinterpret_cast<const render::MeshVertex*>(verts), size_t(count / 3 * 3));
    if (!mesh) return 0;
    AliasMesh(d, reserved, mesh);
    return 1;
}

int ps2e_draw_skin_create_at(ps2e_draw* d, uint32_t reserved, const ps2e_draw_mesh_vertex* verts, const ps2e_draw_skin_binding* bones,
                             int count) {
    if (!d || !(reserved & kReservedMesh)) return 0;
    if (d) ++d->reuse_stamp.resources;
    if (!d->device || !verts || !bones || count < 3) return 0;
    const size_t n = size_t(count / 3 * 3);
    const uint32_t mesh = d->device->CreateSkinMesh(reinterpret_cast<const render::MeshVertex*>(verts),
                                                    reinterpret_cast<const render::SkinBinding*>(bones), n);
    if (!mesh) return 0;
    AliasMesh(d, reserved, mesh);
    return 1;
}

void ps2e_draw_mesh_destroy(ps2e_draw* d, uint32_t mesh) {
    if (d) ++d->reuse_stamp.resources;
    if (d && d->device && mesh) {
        const uint32_t real = RealMesh(d, mesh);
        if (real) d->device->DestroyMesh(real);
        if (mesh & kReservedMesh) AliasMesh(d, mesh, 0);
    }
}

uint32_t ps2e_draw_skin_create(ps2e_draw* d, const ps2e_draw_mesh_vertex* verts, const ps2e_draw_skin_binding* bones, int count) {
    if (d) ++d->reuse_stamp.resources;
    static_assert(sizeof(ps2e_draw_skin_binding) == sizeof(render::SkinBinding), "the skin binding is the backend's");
    if (!d || !d->device || !verts || !bones || count < 3) return 0;
    const size_t n = size_t(count / 3 * 3);
    const uint32_t mesh = d->device->CreateSkinMesh(reinterpret_cast<const render::MeshVertex*>(verts),
                                                    reinterpret_cast<const render::SkinBinding*>(bones), n);
    return mesh;
}

namespace {
int SubmitMesh(ps2e_draw* d, const ps2e_draw_state* st, uint32_t mesh, const ps2e_draw_mesh_params* mp, const float* bones,
               int bone_vec4, int disp_vec4 = 0);
}

int ps2e_draw_submit_mesh(ps2e_draw* d, const ps2e_draw_state* st, uint32_t mesh, const ps2e_draw_mesh_params* mp) {
    return SubmitMesh(d, st, mesh, mp, nullptr, 0);
}

int ps2e_draw_submit_skin(ps2e_draw* d, const ps2e_draw_state* st, uint32_t skin, const ps2e_draw_mesh_params* mp, const float* bones,
                          int bone_vec4) {
    if (!bones || bone_vec4 <= 0 || uint32_t(bone_vec4) > render::MeshDrawParams::kMaxSkinVec4) {
        return -1;
    }
    return SubmitMesh(d, st, skin, mp, bones, bone_vec4);
}

int ps2e_draw_submit_blend_skin(ps2e_draw* d, const ps2e_draw_state* st, uint32_t skin, const ps2e_draw_mesh_params* mp,
                                const float* bones, int bone_vec4, int disp_vec4) {
    if (!bones || bone_vec4 <= 0 || uint32_t(bone_vec4) > render::MeshDrawParams::kMaxSkinVec4 || disp_vec4 < 0 ||
        uint32_t(disp_vec4) > render::MeshDrawParams::kMaxDispVec4) {
        return -1;
    }
    return SubmitMesh(d, st, skin, mp, bones, bone_vec4, disp_vec4);
}

namespace {
int SubmitMesh(ps2e_draw* d, const ps2e_draw_state* st, uint32_t mesh, const ps2e_draw_mesh_params* mp, const float* bones,
               int bone_vec4, int disp_vec4) {
    if (d) ++d->reuse_stamp.target;
    if (!d || !st || !mesh || !mp || st->stencil == PS2E_DRAW_STENCIL_CLEAR) {
        return -1;
    }
    if (!d->in_frame) ps2e_draw_begin_frame(d, nullptr);
    mesh = RealMesh(d, mesh);
    if (!mesh) {
        return -1;
    }
    render::MeshDrawParams m;
    std::memcpy(m.screen, mp->screen, sizeof m.screen);
    m.offset[0] = mp->offset[0];
    m.offset[1] = mp->offset[1];
    m.scroll[0] = mp->scroll[0];
    m.scroll[1] = mp->scroll[1];
    m.lit = mp->lit == 1;
    m.mode = mp->lit == 2 ? 2 : 0;
    m.cull = mp->cull;
    std::memcpy(m.light_dir, mp->light_dir, sizeof m.light_dir);
    std::memcpy(m.light_colour, mp->light_colour, sizeof m.light_colour);
    std::memcpy(m.ambient, mp->ambient, sizeof m.ambient);
    std::memcpy(m.box, mp->box, sizeof m.box);
    m.bones = bones;
    m.bone_vec4 = uint32_t(bone_vec4);
    m.disp_vec4 = uint32_t(disp_vec4);

    bool retex = false;
    if (d->mesh_run && d->mesh_run_draws == d->draws) {
        if (std::memcmp(&d->mesh_run_state, st, sizeof *st) == 0) {
            const render::DrawResult again = d->device->DrawMeshAgain(mesh, m);
            if (again.drew) {
                d->draws++;
                d->mesh_run_draws = d->draws;
                return 1;
            }
        } else {
            retex = d->ir_raster_valid && RestEqual(&d->mesh_run_state, st);
        }
    }
    d->mesh_run = false;
    gs::DrawIr& ir = d->ir;
    gs::TextureSource ts;
    render::DrawResult r;
    bool made = false;
    if (retex) {
        if (PrepareTexture(d, st, ts) < 0) return -2;
        ir.verts.clear();
        ir.vertex_count = 0;

        r = d->device->DrawMeshRetex(ir, ts, mesh, m);
        made = r.drew;
    }
    if (!made) {
        if (!retex && PrepareRecord(d, st, ts) < 0) return -2;
        ir.verts.clear();
        ir.vertex_count = 0;
        const render::DrawParams p = TargetParams(d, st);
        r = d->device->DrawMesh(ir, ts, p, mesh, m);
    }
    d->ir_raster_valid = true;
    d->draws++;
    if (!r.drew) {
        return -3;
    }
    d->mesh_run = true;
    d->mesh_run_state = *st;
    d->mesh_run_draws = d->draws;
    return 1;
}
}

void ps2e_draw_prefetch(const ps2e_draw* d, uint32_t texture, uint32_t mesh) {
    if (!d || !d->device) return;
    render::SurfaceId surface;
    if (texture && texture != PS2E_DRAW_TEXTURE_FRAME_COPY) {

        const auto& slot = d->texture_slots[texture & 511u];
        if (slot.id == texture && slot.generation == d->texture_generation && slot.texture) {
            const char* t = reinterpret_cast<const char*>(slot.texture);
            for (size_t o = 0; o < sizeof(ps2e_draw::Texture); o += 64) __builtin_prefetch(t + o, 1, 3);
            surface = slot.texture->surface;
        }
    }
    d->device->PrefetchResources(mesh ? RealMesh(d, mesh) : 0, surface, render::SurfaceId{});
}

int ps2e_draw_copy_frame(ps2e_draw* d) {
    if (d) ++d->reuse_stamp.target;
    if (!d || !d->frame_copy.surface.id) return 0;
    d->mesh_run = false;
    if (!d->in_frame) ps2e_draw_begin_frame(d, nullptr);

    const render::Rect whole{0, 0, d->frame_copy.w, d->frame_copy.h};
    d->device->CopyRect(d->color, whole, d->frame_copy.surface, whole, 0);
    d->frame_copy_valid = true;
    return 1;
}

namespace {

void Scanout(ps2e_draw* d);
}

void ps2e_draw_present(ps2e_draw* d) {
    if (!d) return;
    d->mesh_run = false;
    Scanout(d);
}

namespace {
void Scanout(ps2e_draw* d) {
    if (!d->in_frame) {
        d->device->BeginFrame();
        d->device->BeginDrawRun();
        d->in_frame = true;
    }
    d->device->EndDrawRun();

    ps2e_draw_presentation want;
    {
        std::lock_guard<std::mutex> lock(d->presentation_mutex);
        want = d->wanted;
    }
    if (want.vsync != d->applied.vsync) ps2e::DrawBackendSwapInterval(*d->device, want.vsync ? 1 : 0);
    const int filter_in_use = d->applied.texture_filter;
    d->applied = want;
    d->applied.texture_filter = filter_in_use;
    render::ScanoutDesc sd;
    sd.source = d->color;
    sd.src = {0, 0, d->phys_w, d->phys_h};

    int ww = 0, wh = 0;
    PresentWindow(*d, &ww, &wh);
    int dw = 1, dh = 1;
    PictureSize(*d, want, ww, wh, &dw, &dh);

    const int ox = int(std::lround(double(want.offset_x) * dw / d->frame_w));
    const int oy = int(std::lround(double(want.offset_y) * dh / d->frame_h));
    sd.dst = {(ww - dw) / 2 + ox, (wh - dh) / 2 + oy, dw, dh};
    sd.filter = want.nearest ? render::PresentationFilter::Nearest : render::PresentationFilter::Linear;

    if (want.flicker_merge && d->frame_h > 0) sd.merge_line = float(d->phys_h) / float(d->frame_h);
    sd.merge_x = want.edge_soften && dw > 0 ? 0.5f * float(d->phys_w) / float(dw) : 0.f;
    sd.fxaa = want.post_aa != 0;
    d->device->SetWindowSize(ww, wh);
    d->device->Present(sd);
    d->device->EndFrame();
    d->in_frame = false;
}
}

void ps2e_draw_set_presentation(ps2e_draw* d, const ps2e_draw_presentation* p) {
    if (!d || !p) return;
    std::lock_guard<std::mutex> lock(d->presentation_mutex);
    d->wanted = *p;
    d->wanted.scale = std::clamp(p->scale, 0, 4);
    d->wanted.render_width = std::clamp(p->render_width, 0, 8192);
    const float rs = p->render_scale;
    d->wanted.render_scale = !std::isfinite(rs) ? 0.f : rs < 0.f ? -1.f : rs > 0.f ? std::clamp(rs, 1.f, 4.f) : 0.f;
}


}
