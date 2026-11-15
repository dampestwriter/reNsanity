
#include "movie_player.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "audio_service.h"
#include "engine/cdvd/iso_fs.h"
#include "engine/platform/ps2e/ps2e_draw.h"
#include "engine/platform/ps2e/ps2e_movie.h"
#include "ps2e_seam.h"
#include "te_graphics_hooks.h"
#include "te_host.h"
#include "te_services.h"

namespace te::movie {

MoviePlayer::MoviePlayer() = default;
MoviePlayer::~MoviePlayer() {
    for (auto& [address, p] : players_) DestroyDecoder(p);
}

MoviePlayer::Player* MoviePlayer::Find(uint32_t player) {
    const auto it = players_.find(player);
    if (it == players_.end()) return nullptr;
    return &it->second;
}

uint64_t MoviePlayer::DueField(uint64_t present, double fps, uint32_t hz) {
    if (!(fps > 0.0) || hz == 0) return present * 2;
    return static_cast<uint64_t>(std::floor(static_cast<double>(present) * static_cast<double>(hz) / fps + 1e-9));
}

void MoviePlayer::Construct(uint32_t player) {

    if (uint8_t* bytes = te::GuestPointer(player, kPlayerSize)) std::memset(bytes, 0, kPlayerSize);
    auto it = players_.find(player);
    if (it != players_.end()) DestroyDecoder(it->second);
    players_[player] = Player{};
    players_[player].constructed = true;
}

void MoviePlayer::BeginPresenting(uint32_t player, uint32_t present) {
    Player* p = Find(player);
    if (!p) return;
    p->presenting = true;
    p->present = present;
    p->queue.clear();
}

bool MoviePlayer::Open(uint32_t player, const char* file, uint32_t audioChannel, int32_t width, uint32_t lentMemory) {
    Player* p = Find(player);
    if (!p) return false;
    if (p->open) Close(player);
    p->file = file ? file : "";
    p->audio_channel = audioChannel;
    p->width = width;
    p->lent_memory = lentMemory;
    p->pcm_popped = 0;
    p->frames = 0;
    p->fields = p->presented = 0;
    p->fps = 0.0;
    p->sound_started = false;
    p->volume = 0.f;
    p->decoded.reset();
    p->queue.clear();
    p->held.clear();
    p->picture_index = 0;
    p->texture_filled = false;

    te::Disc& disc = te::Services::Get().disc();
    disc.Initialise();
    IsoFs::Entry entry;
    if (!disc.IsOpen() || p->file.empty() || !disc.Iso().Find(p->file, entry) || entry.directory || entry.size == 0) return false;
    p->image_offset = uint64_t{entry.lba} * IsoFs::kSectorSize;
    p->file_size = entry.size;
    p->read_cursor = 0;
    p->input_done = false;

    ps2e_movie_config cfg{};
    cfg.audio_channel = static_cast<int>(audioChannel);
    cfg.max_pictures = 3;
    cfg.yuv420 = YuvOnDevice() ? 1 : 0;
    p->decoder = ps2e_movie_create(&cfg);
    if (!p->decoder) {
        return false;
    }
    p->open = true;
    playing_ = p;
    EnsureMixerSource();

    if (DecodeNext(*p)) {
        p->queue.push_back(std::move(p->decoded));
        DecodeNext(*p);
    }
    FeedSound(*p);
    if (p->fps <= 0.0) p->fps = ps2e_movie_fps(p->decoder);
    return true;
}

void MoviePlayer::StartSound(uint32_t player, float volume) {
    Player* p = Find(player);
    if (!p || !p->open) return;
    p->volume = std::clamp(volume, 0.f, 1.f);
    p->sound_started = true;
}

bool MoviePlayer::Step(uint32_t player) {
    Player* p = Find(player);
    if (!p || !p->open) return false;
    if (!p->decoder) return false;
    if (Ended(*p)) return false;
    if (p->decoded && p->queue.size() < kPictureQueue) {
        p->queue.push_back(std::move(p->decoded));
        DecodeNext(*p);
        p->frames++;
    }
    FeedSound(*p);
    return true;
}

void MoviePlayer::WaitFrame(uint32_t player) {
    Player* p = Find(player);
    if (!p || !p->open) return;
    if (!p->decoder) return;

    if (p->sound_started && p->fps > 0.0 && p->pcm_popped > 0 && p->decoder && !ps2e_movie_audio_disabled(p->decoder)) {
        int64_t unheard = 0;
        if (te::audio::Service().MovieUnheardFrames(unheard)) {
            const int64_t heard = int64_t(p->pcm_popped) - unheard;
            const int64_t want = int64_t(std::llround(double(p->presented) * 48000.0 / p->fps));
            const int64_t per_field = int64_t(48000u / 60u);
            const int64_t ahead = (heard - want) / per_field;
            if (ahead >= 1) {
                const int64_t skip = std::min<int64_t>(ahead, 4);
                p->fields += static_cast<uint64_t>(skip);
            }
        }
    }
    const uint64_t due = DueField(p->presented, p->fps, 60u);

    p = Boundary(player);
    for (int extra = 0; p && p->fields + 1 < due && extra < 8; ++extra) p = Boundary(player);
    if (!p || !p->open) return;
    if (p->presenting && p->present) {
        ++p->presented;
                te_call_void_callback(p->present);
        p = Find(player);

        if (p) p->fields += 1;
        if (p) p->quad_pending = false;
    }
}

void MoviePlayer::QueuePicture(uint32_t player) {
    Player* p = Find(player);
    if (!p || !p->open || p->queue.empty()) return;
    std::unique_ptr<Picture> picture = std::move(p->queue.front());
    p->queue.pop_front();
    const uint64_t index = p->picture_index++;
    int64_t unheard = 0;
    if (picture && p->sound_started && p->fps > 0.0 && p->pcm_popped > 0 && p->decoder && !ps2e_movie_audio_disabled(p->decoder) &&
        te::audio::Service().MovieUnheardFrames(unheard)) {
        p->held.push_back({std::move(picture), index, te::Services::Get().clock().BoundaryCount()});
        ReleaseHeld(*p, false);
        return;
    }
    ReleaseHeld(*p, true);
    if (picture) UploadPicture(*p, *picture);
}

void MoviePlayer::ReleaseHeld(Player& p, bool all) {
    constexpr int64_t kHalfBoundaryFrames = 400;
    if (p.held.empty()) return;
    int64_t unheard = 0;
    const bool gated = !all && p.sound_started && p.fps > 0.0 && te::audio::Service().MovieUnheardFrames(unheard);
    const int64_t heard = int64_t(p.pcm_popped) - unheard;
    const uint64_t now = te::Services::Get().clock().BoundaryCount();

    constexpr size_t kMostBehind = 4;
    const auto due_at = [&](const Player::Held& h) {

        return !gated || heard + kHalfBoundaryFrames >= int64_t(std::llround(double(h.index) * 48000.0 / p.fps)) || now >= h.boundary + 15;
    };
    size_t due_count = 0;
    for (const Player::Held& h : p.held) {
        if (!due_at(h)) break;
        ++due_count;
    }
    const size_t release = all ? p.held.size() : due_count > kMostBehind ? due_count - kMostBehind + 1 : std::min<size_t>(due_count, 1);
    std::unique_ptr<Picture> show;
    for (size_t n = 0; n < release && !p.held.empty(); ++n) {
        Player::Held& h = p.held.front();
        if (!all && !due_at(h)) break;
        show = std::move(h.picture);
        p.held.pop_front();
    }
    if (show) UploadPicture(p, *show);
}

void MoviePlayer::Draw(uint32_t player, uint32_t area, int32_t width, int32_t height, uint32_t skippedRows) {
    Player* p = Find(player);
    if (!p || !p->open) return;
    ReleaseHeld(*p, false);
    const auto* a = reinterpret_cast<const int32_t*>(te::GuestPointer(area, 16));
    if (!p->decoder || p->frames < 2 || !p->texture || !p->texture_filled) return;
    if (!a || a[2] <= 0 || a[3] <= 0 || height <= 0) return;
    (void)width;
    const float x0 = float(a[0]), y0 = float(a[1]), x1 = float(a[0] + a[2]), y1 = float(a[1] + a[3]);
    const float v0 = float(skippedRows) / float(height), v1 = float(height - int32_t(skippedRows)) / float(height);
    te_graphics_record_movie_quad(p->texture, x0, y0, x1, y1, 0.f, v0, 1.f, v1);
    p->quad_pending = true;
    p->quad[0] = x0; p->quad[1] = y0; p->quad[2] = x1; p->quad[3] = y1; p->quad[4] = v0; p->quad[5] = v1;
}

void MoviePlayer::Close(uint32_t player) {
    Player* p = Find(player);
    if (!p) return;
    p->open = false;
    p->presenting = false;
    p->sound_started = false;
    p->decoded.reset();
    p->queue.clear();
    p->held.clear();
    p->quad_pending = false;
    DestroyDecoder(*p);
    if (playing_ == p) playing_ = nullptr;
}

bool MoviePlayer::Pump(Player& p) {
    if (!p.decoder || p.input_done) return false;
    if (p.read_cursor >= p.file_size) {
        p.input_done = true;
        ps2e_movie_end_input(p.decoder);
        return false;
    }
    const uint32_t n = std::min<uint32_t>(kReadSlice, p.file_size - p.read_cursor);
    std::vector<uint8_t> slice(n);
    const std::size_t got = te::Services::Get().disc().Iso().ReadBytes(p.image_offset + p.read_cursor, n, slice.data());
    p.read_cursor += static_cast<uint32_t>(got);

    if (got == n && p.read_cursor < p.file_size)
        te::Services::Get().disc().Iso().Prefetch(p.image_offset + p.read_cursor,
            std::min<uint32_t>(512u << 10, p.file_size - p.read_cursor));
    if (got) ps2e_movie_feed(p.decoder, slice.data(), got);
    if (got < n || p.read_cursor >= p.file_size) {
        p.input_done = true;
        ps2e_movie_end_input(p.decoder);
    }
    return true;
}

bool MoviePlayer::DecodeNext(Player& p) {
    p.decoded.reset();
    if (!p.decoder) return false;
    while (ps2e_movie_pictures_ready(p.decoder) == 0) {
        if (ps2e_movie_ended(p.decoder) || !Pump(p)) {
            if (ps2e_movie_pictures_ready(p.decoder) == 0) return false;
            break;
        }
    }
    ps2e_movie_picture pic{};
    if (!ps2e_movie_take_picture(p.decoder, &pic)) return false;
    auto out = std::make_unique<Picture>();
    out->width = pic.width;
    out->height = pic.height;
    if (pic.yuv420) out->yuv.assign(pic.yuv420, pic.yuv420 + ps2e_movie_yuv420_bytes(pic.width, pic.height));
    else if (pic.rgba8) out->rgba.assign(pic.rgba8, pic.rgba8 + size_t(pic.width) * size_t(pic.height) * 4);
    p.decoded = std::move(out);
    if (p.fps <= 0.0) p.fps = ps2e_movie_fps(p.decoder);
    return true;
}

void MoviePlayer::FeedSound(Player& p) {
    if (!p.decoder) return;
    for (int pumps = 0; ps2e_movie_pcm_queued(p.decoder) < kSoundBufferFrames && pumps < 16; ++pumps) {
        if (ps2e_movie_audio_disabled(p.decoder)) return;
        if (!Pump(p)) return;
    }
}

bool MoviePlayer::Ended(const Player& p) const {
    return p.decoder && ps2e_movie_ended(p.decoder) && !p.decoded;
}

MoviePlayer::Player* MoviePlayer::Boundary(uint32_t player) {
    te::Services::Get().clock().Boundary();
    Player* p = Find(player);
    if (p) p->fields += 1;
    return p;
}

namespace {
uint64_t DifferingPixels(const uint8_t* a, const uint8_t* b, size_t pixels) {
    uint64_t n = 0;
    for (size_t i = 0; i < pixels; ++i) n += std::memcmp(a + i * 4, b + i * 4, 4) != 0;
    return n;
}
}

bool MoviePlayer::YuvOnDevice() {
    if (yuv_device_ < 0) {
        int on = 0;
        if (ps2e_draw* dev = te_graphics_device()) {
            struct Job { ps2e_draw* dev; int on; } job{dev, 0};
            te_graphics_on_render([](void* user) { Job& j = *static_cast<Job*>(user); j.on = ps2e_draw_yuv420_supported(j.dev); }, &job);
            on = job.on;
        }
        yuv_device_ = on;
    }
    return yuv_device_ == 1;
}

void MoviePlayer::UploadPicture(Player& p, Picture& picture) {
    const int w = picture.width, h = picture.height;
    if (w <= 0 || h <= 0) return;
    if (picture.yuv.empty()) {
        if (picture.rgba.size() < size_t(w) * size_t(h) * 4) return;
        UploadRgba(p, std::move(picture.rgba), w, h);
        return;
    }
    if (picture.yuv.size() < ps2e_movie_yuv420_bytes(w, h)) return;
    ps2e_movie_yuv_tables_info tables{};
    const int state = ps2e_movie_yuv_tables(w, h, &tables);
    const auto checked = state == 1 ? yuv_checked_.find(tables.serial) : yuv_checked_.end();
    if (checked != yuv_checked_.end() && checked->second && UploadPlanes(p, picture)) return;

    std::vector<uint8_t> rgba(size_t(w) * size_t(h) * 4);
    if (!ps2e_movie_yuv_to_rgba(picture.yuv.data(), w, h, rgba.data())) return;
    if (state == 1 && checked == yuv_checked_.end()) {
        check_scratch_.resize(rgba.size());
        ps2e_movie_yuv_tables_apply(&tables, picture.yuv.data(), w, h, check_scratch_.data());
        const uint64_t differ = DifferingPixels(rgba.data(), check_scratch_.data(), size_t(w) * size_t(h));
        yuv_checked_[tables.serial] = differ == 0;
    }
    UploadRgba(p, std::move(rgba), w, h);
}

bool MoviePlayer::UploadPlanes(Player& p, Picture& picture) {
    ps2e_draw* dev = te_graphics_device();
    ps2e_movie_yuv_tables_info tables{};
    if (!dev || ps2e_movie_yuv_tables(picture.width, picture.height, &tables) != 1) return false;

    const auto confirmed = gpu_checked_.find(tables.serial);
    const bool first = confirmed == gpu_checked_.end();
    if (!first && !confirmed->second) return false;
    const bool verify = first;
    if (!verify && p.texture && p.texture_yuv && p.texture_w == picture.width && p.texture_h == picture.height &&
        p.texture_tables == tables.serial) {

        struct Post { ps2e_draw* dev; uint32_t texture; std::vector<uint8_t> yuv; };
        te_graphics_post_render([](void* user) {
            Post* j = static_cast<Post*>(user);
            ps2e_draw_texture_upload_yuv420(j->dev, j->texture, j->yuv.data());
            delete j;
        }, new Post{dev, p.texture, std::move(picture.yuv)});
        p.texture_filled = true;
        return true;
    }
    if (verify) check_scratch_.resize(size_t(picture.width) * size_t(picture.height) * 4);
    struct Job { ps2e_draw* dev; Player* p; const Picture* picture; const ps2e_movie_yuv_tables_info* tables; uint8_t* readback; int uploaded; }
        job{dev, &p, &picture, &tables, verify ? check_scratch_.data() : nullptr, 0};
    te_graphics_on_render([](void* user) {
        Job& j = *static_cast<Job*>(user);
        Player& p = *j.p;
        const Picture& picture = *j.picture;
        if (!ps2e_draw_yuv_tables(j.dev, j.tables->serial, j.tables->rb, j.tables->g)) return;
        if (p.texture && (!p.texture_yuv || p.texture_w != picture.width || p.texture_h != picture.height)) {
            ps2e_draw_texture_destroy(j.dev, p.texture);
            p.texture = 0;
            p.texture_filled = false;
        }
        if (!p.texture) {
            p.texture = ps2e_draw_texture_create_yuv420(j.dev, picture.width, picture.height);
            p.texture_w = picture.width;
            p.texture_h = picture.height;
            p.texture_yuv = true;
            if (!p.texture) return;
        }
        p.texture_tables = j.tables->serial;
        j.uploaded = ps2e_draw_texture_upload_yuv420(j.dev, p.texture, picture.yuv.data());
        if (j.uploaded && j.readback && !ps2e_draw_texture_readback(j.dev, p.texture, j.readback)) j.readback = nullptr;
    }, &job);
    if (!job.uploaded) return false;
    if (job.readback) {
        std::vector<uint8_t> rgba(check_scratch_.size());
        if (ps2e_movie_yuv_to_rgba(picture.yuv.data(), picture.width, picture.height, rgba.data())) {
            const uint64_t differ = DifferingPixels(rgba.data(), check_scratch_.data(), size_t(picture.width) * size_t(picture.height));
            if (first) {
                gpu_checked_[tables.serial] = differ == 0;
                if (differ) {
                    UploadRgba(p, std::move(rgba), picture.width, picture.height);
                    return true;
                }
            }
        }
    } else if (first) {
        gpu_checked_[tables.serial] = false;
        return false;
    }
    p.texture_filled = true;
    return true;
}

void MoviePlayer::UploadRgba(Player& p, std::vector<uint8_t>&& rgba, int w, int h) {
    ps2e_draw* dev = te_graphics_device();
    if (!dev) return;
    if (p.texture && !p.texture_yuv && p.texture_w == w && p.texture_h == h) {

        struct Post { ps2e_draw* dev; uint32_t texture; int w, h; std::vector<uint8_t> rgba; };
        te_graphics_post_render([](void* user) {
            Post* j = static_cast<Post*>(user);
            ps2e_draw_texture_upload(j->dev, j->texture, 0, 0, j->w, j->h, j->rgba.data(), 0);
            delete j;
        }, new Post{dev, p.texture, w, h, std::move(rgba)});
        p.texture_filled = true;
        return;
    }

    struct Job { ps2e_draw* dev; Player* p; const uint8_t* rgba; int w, h; int uploaded; } job{dev, &p, rgba.data(), w, h, 0};
    te_graphics_on_render([](void* user) {
        Job& j = *static_cast<Job*>(user);
        Player& p = *j.p;
        if (p.texture && (p.texture_yuv || p.texture_w != j.w || p.texture_h != j.h)) {
            ps2e_draw_texture_destroy(j.dev, p.texture);
            p.texture = 0;
        }
        if (!p.texture) {
            p.texture = ps2e_draw_texture_create(j.dev, j.w, j.h, nullptr);
            p.texture_w = j.w;
            p.texture_h = j.h;
            p.texture_yuv = false;
            if (!p.texture) return;
        }

        j.uploaded = ps2e_draw_texture_upload(j.dev, p.texture, 0, 0, j.w, j.h, j.rgba, 0);
    }, &job);
    if (job.uploaded) {
        p.texture_filled = true;
    }
}

void MoviePlayer::DestroyDecoder(Player& p) {
    if (p.decoder) {
        ps2e_movie_destroy(p.decoder);
        p.decoder = nullptr;
    }
    if (p.texture) {
        if (ps2e_draw* dev = te_graphics_device()) {
            struct Job { ps2e_draw* dev; uint32_t texture; } job{dev, p.texture};
            te_graphics_on_render([](void* user) { Job& j = *static_cast<Job*>(user); ps2e_draw_texture_destroy(j.dev, j.texture); }, &job);
        }
        p.texture = 0;
        p.texture_filled = false;
        p.texture_yuv = false;
        p.texture_tables = 0;
    }
}

void MoviePlayer::EnsureMixerSource() {
    if (mixer_hooked_) return;
    mixer_hooked_ = true;
    te::audio::Service().SetMovieSource(this);
}

std::array<float, 2> MoviePlayer::Gains() const {
    if (!playing_ || !playing_->open || !playing_->sound_started) return {0.f, 0.f};
    return {playing_->volume, playing_->volume};
}

int MoviePlayer::Pop(int16_t* stereo, int frames) {
    if (!playing_ || !playing_->open || !playing_->sound_started || !playing_->decoder || frames <= 0) return 0;
    const uint32_t got = ps2e_movie_pcm_read(playing_->decoder, stereo, static_cast<uint32_t>(frames));
    playing_->pcm_popped += got;
    if (got < static_cast<uint32_t>(frames) && !ps2e_movie_ended(playing_->decoder)) {}
    return static_cast<int>(got);
}

MoviePlayer& Service() {
    static MoviePlayer player;
    return player;
}

}

extern "C" {

void ps2e_movie_construct(ps2e_u32 player) {
    te::movie::Service().Construct(player);
}
void ps2e_movie_begin_presenting(ps2e_u32 player, ps2e_u32 present) { te::movie::Service().BeginPresenting(player, present); }
ps2e_s32 ps2e_movie_open(ps2e_u32 player, ps2e_u32 file, ps2e_s32 audioChannel, ps2e_s32 width, ps2e_u32 lentMemory) {
    return te::movie::Service().Open(player, te::GuestString(file, 0x100), static_cast<uint32_t>(audioChannel), width, lentMemory) ? 1 : 0;
}
void ps2e_movie_start_sound(ps2e_u32 player, ps2e_f32 volume) { te::movie::Service().StartSound(player, volume); }
ps2e_s32 ps2e_movie_step(ps2e_u32 player) { return te::movie::Service().Step(player) ? 1 : 0; }
void ps2e_movie_wait_frame(ps2e_u32 player) { te::movie::Service().WaitFrame(player); }
void ps2e_movie_queue_picture(ps2e_u32 player) { te::movie::Service().QueuePicture(player); }
void ps2e_movie_draw(ps2e_u32 player, ps2e_u32 area, ps2e_s32 width, ps2e_s32 height, ps2e_s32 skippedRows) {
    te::movie::Service().Draw(player, area, width, height, static_cast<uint32_t>(skippedRows));
}
void ps2e_movie_close(ps2e_u32 player) { te::movie::Service().Close(player); }

}
