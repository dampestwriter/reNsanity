#include "engine/platform/ps2e/ps2e_movie.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "engine/ipu/hle/decoded_picture.h"
#include "engine/ipu/hle/ffmpeg_video_decoder.h"
#include "engine/ipu/hle/fmv_audio_capture.h"
#include "engine/ipu/hle/fmv_pcm_sink.h"
#include "engine/ipu/hle/pss_demux.h"
#include "engine/ipu/hle/pss_demux_listener.h"
#include "engine/ipu/hle/yuv_rgba.h"

using namespace ps2r;

struct ps2e_movie final : ipu::PssDemuxListener, ipu::FmvPcmSink {
    explicit ps2e_movie(const ps2e_movie_config& config);
    ~ps2e_movie() override = default;

    void OnVideoEs(const uint8_t* data, size_t size, bool contains_sequence_end) override;
    void OnAudioPes(const uint8_t* data, size_t size) override;
    void OnProgramEnd() override;

    void Push(const int16_t* stereo, uint32_t frames) override;
    void GateOpen() override {}
    void Flush() override {}

    void Feed(const uint8_t* bytes, size_t size);
    void EndInput();
    void DrainPending();
    bool TakePicture(ps2e_movie_picture& out);
    uint32_t ReadPcm(int16_t* stereo, uint32_t frames);

    int max_pictures = 4;
    ipu::PssDemux demux;
    ipu::FfmpegVideoDecoder decoder;
    ipu::FmvAudioCapture capture;
    std::deque<ipu::DecodedPicture> pictures;
    ipu::DecodedPicture taken;
    std::vector<uint8_t> pending_es;
    size_t pending_off = 0;
    bool pending_sequence_end = false;
    bool input_ended = false;
    bool decoder_flushed = false;
    std::deque<int16_t> pcm;
};

ps2e_movie::ps2e_movie(const ps2e_movie_config& config) : demux(*this), capture(*this) {
    if (config.max_pictures > 0) max_pictures = config.max_pictures;
    capture.SelectChannel(config.audio_channel < 0 ? ipu::FmvAudioCapture::kAnyChannel : config.audio_channel);
    decoder.SetPlanarOutput(config.yuv420 != 0);
}

void ps2e_movie::OnVideoEs(const uint8_t* data, size_t size, bool contains_sequence_end) {
    if (!data || size == 0) {
        if (contains_sequence_end) input_ended = true;
        return;
    }
    if (!pending_es.empty() || static_cast<int>(pictures.size()) >= max_pictures) {
        pending_es.insert(pending_es.end(), data, data + size);
        if (contains_sequence_end) pending_sequence_end = true;
        return;
    }
    decoder.Feed(data, size, pictures);
    if (contains_sequence_end) input_ended = true;
}

void ps2e_movie::OnAudioPes(const uint8_t* data, size_t size) {
    capture.OnAudioPes(data, size);
}

void ps2e_movie::OnProgramEnd() { input_ended = true; }

void ps2e_movie::Push(const int16_t* stereo, uint32_t frames) {
    if (!stereo || frames == 0) return;
    pcm.insert(pcm.end(), stereo, stereo + size_t(frames) * 2);
}

void ps2e_movie::DrainPending() {
    constexpr size_t kSlice = 64 * 1024;
    while (pending_off < pending_es.size() && static_cast<int>(pictures.size()) < max_pictures) {
        const size_t n = std::min(kSlice, pending_es.size() - pending_off);
        decoder.Feed(pending_es.data() + pending_off, n, pictures);
        pending_off += n;
    }
    if (pending_off >= pending_es.size()) {
        pending_es.clear();
        pending_off = 0;
        if (pending_sequence_end) {
            pending_sequence_end = false;
            input_ended = true;
        }
    } else if (pending_off > 4 * 1024 * 1024) {
        pending_es.erase(pending_es.begin(), pending_es.begin() + static_cast<std::ptrdiff_t>(pending_off));
        pending_off = 0;
    }
}

void ps2e_movie::Feed(const uint8_t* bytes, size_t size) {
    if (bytes && size) {
        demux.Append(bytes, size);
        demux.Process(false);
        if (demux.Ended()) input_ended = true;
    }
    DrainPending();

    if (input_ended && pending_es.empty() && !decoder_flushed) {
        decoder_flushed = true;
        decoder.Flush(pictures);
    }
}

void ps2e_movie::EndInput() {
    demux.Process(true);
    input_ended = true;
    Feed(nullptr, 0);
}

bool ps2e_movie::TakePicture(ps2e_movie_picture& out) {
    if (pictures.empty()) return false;
    taken = std::move(pictures.front());
    pictures.pop_front();
    out.width = taken.width;
    out.height = taken.height;
    out.rgba8 = taken.rgba.empty() ? nullptr : taken.rgba.data();
    out.yuv420 = taken.yuv.empty() ? nullptr : taken.yuv.data();

    Feed(nullptr, 0);
    return true;
}

uint32_t ps2e_movie::ReadPcm(int16_t* stereo, uint32_t frames) {
    if (!stereo || frames == 0) return 0;
    const uint32_t have = static_cast<uint32_t>(pcm.size() / 2);
    const uint32_t n = std::min(frames, have);
    for (uint32_t i = 0; i < n * 2; ++i) stereo[i] = pcm[i];
    pcm.erase(pcm.begin(), pcm.begin() + static_cast<std::ptrdiff_t>(n) * 2);
    return n;
}

namespace {

class YuvTablesCache {
public:
    ~YuvTablesCache() {
        std::thread t;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            t = std::move(worker_);
        }
        if (t.joinable()) t.join();
    }
    int Get(int w, int h, ps2e_movie_yuv_tables_info* out) {
        std::lock_guard<std::mutex> lock(mutex_);
        Entry& e = entries_[{w, h}];
        if (e.state == kReady) {
            if (out) *out = ps2e_movie_yuv_tables_info{e.tables->rb.data(), e.tables->g.data(), e.serial, e.tables->probe_ms};
            return 1;
        }
        if (e.state == kRefused) return -1;
        if (e.state == kIdle && !busy_) {
            if (worker_.joinable()) worker_.join();
            e.state = kProbing;
            busy_ = true;
            worker_ = std::thread([this, w, h] { Probe(w, h); });
        }
        return 0;
    }

private:
    enum State { kIdle, kProbing, kReady, kRefused };
    struct Entry {
        State state = kIdle;
        std::shared_ptr<const ipu::YuvRgbaTables> tables;
        uint32_t serial = 0;
    };
    void Probe(int w, int h) {
        std::string why;
        auto tables = ipu::ProbeYuvRgbaTables(w, h, &why);
        std::lock_guard<std::mutex> lock(mutex_);
        Entry& e = entries_[{w, h}];
        busy_ = false;
        if (!tables) {
            e.state = kRefused;
            return;
        }
        for (const auto& [size, other] : entries_)
            if (other.state == kReady && other.tables->SameAs(*tables)) {
                e.tables = other.tables;
                e.serial = other.serial;
                break;
            }
        if (!e.tables) {
            e.tables = tables;
            e.serial = ++serials_;
        }
        e.state = kReady;
    }
    std::mutex mutex_;
    std::map<std::pair<int, int>, Entry> entries_;
    std::thread worker_;
    bool busy_ = false;
    uint32_t serials_ = 0;
};

YuvTablesCache& Tables() {
    static YuvTablesCache cache;
    return cache;
}

}

extern "C" {

size_t ps2e_movie_yuv420_bytes(int width, int height) {
    return width > 0 && height > 0 ? ipu::Yuv420Bytes(width, height) : 0;
}

int ps2e_movie_yuv_to_rgba(const uint8_t* yuv420, int width, int height, uint8_t* rgba8) {
    static std::mutex mutex;
    static ipu::YuvToRgba convert;
    std::lock_guard<std::mutex> lock(mutex);
    return convert.Convert(yuv420, width, height, rgba8) ? 1 : 0;
}

int ps2e_movie_yuv_tables(int width, int height, ps2e_movie_yuv_tables_info* out) {
    if (width < 2 || height < 2 || (width & 1) || (height & 1)) return -1;
    return Tables().Get(width, height, out);
}

void ps2e_movie_yuv_tables_apply(const ps2e_movie_yuv_tables_info* tables, const uint8_t* yuv420, int width, int height,
                                 uint8_t* rgba8) {
    if (!tables || !tables->rb || !tables->g || !yuv420 || !rgba8) return;

    const int cw = width / 2;
    const uint8_t* up = yuv420 + size_t(width) * size_t(height);
    const uint8_t* vp = up + size_t(cw) * size_t(height / 2);
    for (int y = 0; y < height; ++y) {
        const uint8_t* yr = yuv420 + size_t(y) * size_t(width);
        const uint8_t* ur = up + size_t(y >> 1) * size_t(cw);
        const uint8_t* vr = vp + size_t(y >> 1) * size_t(cw);
        uint8_t* o = rgba8 + size_t(y) * size_t(width) * 4;
        for (int x = 0; x < width; ++x) {
            const int Y = yr[x], U = ur[x >> 1], V = vr[x >> 1];
            o[x * 4 + 0] = tables->rb[size_t(V) * ipu::YuvRgbaTables::kRbWidth + size_t(Y)];
            o[x * 4 + 1] = tables->g[ipu::YuvRgbaTables::GIndex(Y, U, V)];
            o[x * 4 + 2] = tables->rb[size_t(256 + U) * ipu::YuvRgbaTables::kRbWidth + size_t(Y)];
            o[x * 4 + 3] = 0xFF;
        }
    }
}

ps2e_movie* ps2e_movie_create(const ps2e_movie_config* config) {
    ps2e_movie_config c{};
    c.audio_channel = -1;
    if (config) c = *config;
    return new ps2e_movie(c);
}

void ps2e_movie_destroy(ps2e_movie* movie) { delete movie; }

int ps2e_movie_feed(ps2e_movie* movie, const void* bytes, size_t size) {
    if (!movie) return 0;
    movie->Feed(static_cast<const uint8_t*>(bytes), size);
    return static_cast<int>(movie->pictures.size());
}

void ps2e_movie_end_input(ps2e_movie* movie) {
    if (movie) movie->EndInput();
}

int ps2e_movie_pictures_ready(const ps2e_movie* movie) { return movie ? static_cast<int>(movie->pictures.size()) : 0; }

int ps2e_movie_take_picture(ps2e_movie* movie, ps2e_movie_picture* picture) {
    if (!movie || !picture) return 0;
    return movie->TakePicture(*picture) ? 1 : 0;
}

int ps2e_movie_ended(const ps2e_movie* movie) {
    return movie && movie->input_ended && movie->pending_es.empty() && movie->decoder_flushed && movie->pictures.empty() ? 1 : 0;
}

double ps2e_movie_fps(const ps2e_movie* movie) { return movie ? movie->decoder.AuthoredFps() : 0.0; }

int ps2e_movie_audio_disabled(const ps2e_movie* movie) { return movie && movie->capture.Disabled() ? 1 : 0; }
uint32_t ps2e_movie_pcm_queued(const ps2e_movie* movie) { return movie ? static_cast<uint32_t>(movie->pcm.size() / 2) : 0; }
uint32_t ps2e_movie_pcm_read(ps2e_movie* movie, int16_t* stereo, uint32_t frames) {
    return movie ? movie->ReadPcm(stereo, frames) : 0;
}

}
