
#include "stream_service.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <system_error>
#include <thread>

#include "audio_service.h"
#include "engine/cdvd/iso_fs.h"
#include "engine/core/config/config.h"
#include "engine/services/read_timing.h"
#include <atomic>

#include "te_host.h"
#include "te_services.h"

namespace te::stream {
namespace {

constexpr double kDriveScale = 0.25;
double DriveScale() { return kDriveScale; }

const IsoFs* Disc() {
    te::Disc& disc = te::Services::Get().disc();
    disc.Initialise();
    return disc.IsOpen() ? &disc.Iso() : nullptr;
}
inline uint64_t DoubleBits(double v) { uint64_t u; std::memcpy(&u, &v, sizeof u); return u; }
inline double BitsDouble(uint64_t u) { double v; std::memcpy(&v, &u, sizeof v); return v; }
}

void StreamService::Reset() {
    archive_ranges_.clear(); archive_folders_.clear(); level_music_.clear();
    windows_.clear();
    window_clock_ = 0;
    music_cache_.Clear();
    files_.clear();
    buffers_.clear();
    pending_.clear();
    timing_.reset();
    state_ = State::Stopped;
    channels_ = 0;
    next_file_ = 0;
    last_size_ = 0;
    hooked_ = false;
    instant_set_ = false;
}

void StreamService::EnsureTiming() {
    if (!instant_set_) {
        instant_ = false;
        instant_set_ = true;
    }
    if (!timing_) {
        ps2r::CdvdConfig cfg;
        cfg.latency_model = ps2r::CdvdLatencyModel::Modelled;
        cfg.delivery_visibility = ps2r::CdvdDeliveryVisibility::Poll;
        cfg.poll_latency_scale = DriveScale();
        cfg.latency_scale = DriveScale();
        timing_.reset(new ReadTiming(cfg));
    }
    if (!hooked_) {

        te::Services::Get().clock().AddBoundaryHook("stream drive", [](uint64_t boundary) { Service().OnBoundary(boundary); });
        hooked_ = true;
    }
}

int32_t StreamService::Initialise(int32_t channels, uint32_t workMemory) {
    (void)workMemory;
    if (channels <= 0 || channels > 256) return -1;
    channels_ = channels;
    if (!Disc()) {

        std::fprintf(stderr, "[reNsanity] stream: no disc image open -- Initialise fails\n");
        state_ = State::Stopped;
        return -1;
    }
    EnsureTiming();
    state_ = State::Running;
    return 0;
}

bool StreamService::ValidChannel(int32_t channel) const {
    return channel >= 0 && channel < channels_;
}

int32_t StreamService::Update() {
    return state_ == State::Running ? 0 : static_cast<int32_t>(state_);
}

uint32_t StreamService::AllocateBuffer(uint32_t size) const {

    const uint32_t granule = 0x40;
    const uint32_t rounded = (size + granule - 1) & ~(granule - 1);
    std::vector<std::pair<uint32_t, uint32_t>> used;
    for (const auto& [channel, buffer] : buffers_) if (buffer.size) used.emplace_back(buffer.address, buffer.size);
    std::sort(used.begin(), used.end());
    uint32_t cursor = kBufferPoolBase;
    for (const auto& [address, used_size] : used) {
        if (address - cursor >= rounded) return cursor;
        cursor = std::max(cursor, address + used_size);
    }
    if (kBufferPoolBase + kBufferPoolBytes - cursor >= rounded) return cursor;
    return 0;
}

void StreamService::AttachBuffer(int32_t channel, uint32_t size, uint32_t soundAddress, uint32_t soundSize) {
    if (!ValidChannel(channel)) return;
    DetachBuffer(channel);
    const uint32_t granule = 0x40;
    const uint32_t rounded = (size + granule - 1) & ~(granule - 1);
    const uint32_t address = AllocateBuffer(rounded);
    if (rounded && address == 0) return;
    Buffer buffer;
    buffer.address = rounded ? address : 0;
    buffer.size = rounded;
    buffer.sound_address = soundAddress;
    buffer.sound_size = soundSize ? soundSize : size;
    buffers_[channel] = buffer;
}

void StreamService::DetachBuffer(int32_t channel) {
    if (!ValidChannel(channel)) return;
    ForceChannel(channel);
    buffers_.erase(channel);
}

uint32_t StreamService::FreeBufferMemory() const {
    uint32_t used = 0;
    for (const auto& [channel, buffer] : buffers_) used += buffer.size;
    return used >= kBufferPoolBytes ? 0 : kBufferPoolBytes - used;
}

uint32_t StreamService::ChannelBufferAddress(int32_t channel) const {
    const auto it = buffers_.find(channel);
    return it == buffers_.end() ? 0 : it->second.address;
}

int32_t StreamService::OpenFile(const char* path) {

    File file;
    file.path = path ? path : "";
    IsoFs::Entry entry;
    const IsoFs* iso = Disc();
    if (iso && !file.path.empty() && iso->Find(file.path, entry) && !entry.directory) {
        file.found = true;
        file.image_offset = uint64_t{entry.lba} * IsoFs::kSectorSize;
        file.size = entry.size;
    }
    const int32_t number = next_file_++;
    files_[number] = file;
    last_size_ = file.found ? file.size : 0;
    if (file.found) {
        music_cache_.IndexBank(*iso, file.path, file.image_offset, file.size);
    }
    return number;
}

void StreamService::CloseFile(int32_t file) {

    std::vector<int32_t> on_file;
    for (const auto& [channel, p] : pending_) if (p.file == file) on_file.push_back(channel);
    for (int32_t channel : on_file) ForceChannel(channel);
    files_.erase(file);
}

bool StreamService::FileExtent(int32_t file, uint64_t& image_offset, uint32_t& size) const {
    const auto it = files_.find(file);
    if (it == files_.end() || !it->second.found) return false;
    image_offset = it->second.image_offset;
    size = it->second.size;
    return true;
}

bool StreamService::ReadRange(int32_t file, uint32_t offset, uint32_t size, std::vector<uint8_t>& out) {
    uint64_t image_offset = 0;
    uint32_t file_size = 0;
    out.clear();
    if (!FileExtent(file, image_offset, file_size)) return false;
    const uint32_t clamped = offset <= file_size ? std::min<uint32_t>(size, file_size - offset) : 0;
    out.resize(clamped);
    const IsoFs* iso = Disc();
    const std::size_t got = clamped && iso ? iso->ReadBytes(image_offset + offset, clamped, out.data()) : 0;
    out.resize(got);
    return true;
}

struct StreamService::Window {
    std::mutex mutex;
    bool done = false;
    FileBytes bytes;
    uint64_t asked = 0;
    std::thread reader;
    ~Window() {
        if (reader.joinable()) reader.join();
    }
};

std::shared_ptr<StreamService::Window> StreamService::CachedWindow(int32_t file, uint32_t offset, uint32_t size, bool start,
                                                                   uint32_t* clamped) {
    uint64_t image_offset = 0;
    uint32_t file_size = 0;
    if (!FileExtent(file, image_offset, file_size)) return nullptr;
    *clamped = offset <= file_size ? std::min<uint32_t>(size, file_size - offset) : 0;
    const auto key = std::make_pair(image_offset + offset, *clamped);
    auto it = windows_.find(key);
    if (it != windows_.end()) {
        Window& hit = *it->second;
        hit.asked = ++window_clock_;
        return it->second;
    }
    if (!start || *clamped == 0) return nullptr;

    uint64_t total = key.second;
    for (const auto& [k, v] : windows_) total += k.second;
    while (total > kWindowCacheBytes) {
        auto oldest = windows_.end();
        for (auto i = windows_.begin(); i != windows_.end(); ++i) {
            std::lock_guard<std::mutex> lock(i->second->mutex);
            if (i->second->done && (oldest == windows_.end() || i->second->asked < oldest->second->asked)) oldest = i;
        }
        if (oldest == windows_.end()) return nullptr;
        total -= oldest->first.second;
        windows_.erase(oldest);
    }
    auto w = std::make_shared<Window>();
    w->asked = ++window_clock_;
    const IsoFs* iso = Disc();
    const auto read = [raw = w.get(), iso, at = key.first, n = key.second, path = files_.at(file).path] {
        auto bytes = std::make_shared<std::vector<uint8_t>>(n);
        bytes->resize(iso ? iso->ReadBytes(at, n, bytes->data()) : 0);
        std::lock_guard<std::mutex> lock(raw->mutex);
        raw->bytes = std::move(bytes);
        raw->done = true;
    };
    try {
        w->reader = std::thread(read);
    } catch (const std::system_error&) {
        read();
    }
    windows_.emplace(key, w);
    return w;
}

void StreamService::PrefetchFileBytes(int32_t file, uint32_t offset, uint32_t size) {
    uint64_t at = 0; uint32_t n = 0;
    if (FileExtent(file, at, n) && music_cache_.Contains(at + offset, size)) return;
    uint32_t clamped = 0;
    (void)CachedWindow(file, offset, size, true, &clamped);
}

bool StreamService::ReadFileLease(int32_t file, uint32_t offset, uint32_t size, FileBytes& out) {
    out.reset();
    uint32_t clamped = 0;
    const auto w = CachedWindow(file, offset, size, true, &clamped);
    if (!w) {
        auto bytes = std::make_shared<std::vector<uint8_t>>();
        if (!ReadRange(file, offset, size, *bytes)) return false;
        out = std::move(bytes);
        return true;
    }
    if (w->reader.joinable()) w->reader.join();
    {
        std::lock_guard<std::mutex> lock(w->mutex);
        out = w->bytes;
    }
    if (out->size() != clamped) {
        for (auto i = windows_.begin(); i != windows_.end(); ++i)
            if (i->second == w) { windows_.erase(i); break; }
    }
    return true;
}

bool StreamService::ReadMusicAsset(int32_t file, uint32_t offset, uint32_t size, title::MusicAsset::Layout layout,
                                  title::MusicAsset::Data& out, std::string* error) {
    uint64_t at = 0; uint32_t n = 0;
    const bool hit = FileExtent(file, at, n) && music_cache_.Find(at + offset, size, layout, out);
    if (hit) return true;
    FileBytes bytes;
    return ReadFileLease(file, offset, size, bytes) && title::MusicAsset::Read(*bytes, layout, out, error);
}

uint64_t StreamService::Boundary() const { return te::Services::Get().clock().BoundaryCount(); }
uint64_t StreamService::DriveFrame() const {
    return te::Services::Get().clock().Fields();
}
double StreamService::DriveFrameMs() { return kFrameMs; }
double StreamService::FrameEndMs() const { return static_cast<double>(DriveFrame() + 1) * DriveFrameMs(); }

void StreamService::Land(int32_t channel, Pending& p) {
    (void)channel;
    Materialize(p);
    LearnArchive(p);
    if (p.bank) {
        te::audio::Service().StoreSoundBank(p.bank_id, std::move(p.bytes));
        return;
    }
    if (p.bytes.empty()) return;
    if (uint8_t* dst = te::GuestPointer(p.destination, p.bytes.size())) {
        std::memcpy(dst, p.bytes.data(), p.bytes.size());
    }
}

void StreamService::LandPending(int32_t channel) {
    const auto it = pending_.find(channel);
    if (it == pending_.end()) return;
    Pending p = std::move(it->second);
    pending_.erase(it);
    Land(channel, p);
}

void StreamService::ForceChannel(int32_t channel) {
    if (pending_.count(channel) == 0) return;
    LandPending(channel);
}

void StreamService::Issue(int32_t channel, int32_t file, uint32_t offset, uint32_t size, uint32_t destination, bool bank, uint32_t bank_id) {
    EnsureTiming();
    ForceChannel(channel);
    Pending p;
    p.file = file;
    p.offset = offset;
    p.size = size;
    p.destination = destination;
    p.bank = bank;
    p.bank_id = bank_id;
    p.issue_boundary = Boundary();
    uint64_t image_offset = 0;
    uint32_t file_size = 0;
    const bool known = FileExtent(file, image_offset, file_size);
    const uint32_t clamped = known && offset <= file_size ? std::min(size, file_size - offset) : 0;
    if (!instant_ && known && clamped) {
        p.bytes.resize(clamped);
        p.deferred = true;

        const IsoFs* iso = Disc();
        iso->Prefetch(image_offset + offset, clamped);
        Predict(file, offset, clamped);
    } else {
        ReadRange(file, offset, size, p.bytes);
    }
    {
    }
    if (instant_ || !known || p.bytes.empty()) {

        Land(channel, p);
        return;
    }

    const uint64_t first = (image_offset + offset) / IsoFs::kSectorSize;
    const uint64_t last = (image_offset + offset + p.bytes.size() - 1) / IsoFs::kSectorSize;
    const uint32_t lba = static_cast<uint32_t>(first);
    const uint32_t sectors = static_cast<uint32_t>(last - first + 1);
    const uint64_t tick = p.issue_boundary;
    timing_->DueAtMs(tick, static_cast<double>(DriveFrame()) * DriveFrameMs(), lba, sectors);
    p.end_ms = timing_->LastEndMs();
    pending_[channel] = std::move(p);
}

void StreamService::Materialize(const Pending& p) const {
    if (!p.deferred) return;
    uint64_t at = 0; uint32_t size = 0;
    const IsoFs* iso = Disc();
    if (!iso || !FileExtent(p.file, at, size) ||
        iso->ReadDeferredBytes(at + p.offset, p.bytes.size(), p.bytes.data()) != p.bytes.size()) {

        std::fprintf(stderr, "[reNsanity] stream: deferred media read failed file=%d offset=%u size=%zu\n", p.file, p.offset, p.bytes.size());
        std::abort();
    }
    p.deferred = false;
}

namespace {
std::string ArchiveKey(std::string path) {
    for (char& c : path) { if (c == '/') c = '\\'; if (c >= 'a' && c <= 'z') c -= 'a' - 'A'; }
    const auto semi = path.find(';'); if (semi != std::string::npos) path.resize(semi);
    return path;
}
uint32_t ArchiveU32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24;
}
}
void StreamService::LearnArchive(const Pending& p) {
    const auto f = files_.find(p.file);
    if (f == files_.end() || p.offset || p.bytes.size() != f->second.size || p.bytes.size() < 4) return;
    auto key = ArchiveKey(f->second.path);
    if (key.size() < 3 || key.compare(key.size()-3, 3, ".BH")) return;
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    std::vector<std::tuple<uint32_t, uint32_t, std::string>> folders;
    size_t pos = 4;
    while (pos < p.bytes.size()) {
        if (p.bytes.size() - pos < 4) return;
        const uint32_t len = ArchiveU32(p.bytes.data() + pos); pos += 4;
        if (!len || len > 1024 || len > p.bytes.size() - pos || p.bytes.size() - pos - len < 8) return;

        std::string name(reinterpret_cast<const char*>(p.bytes.data() + pos), len);
        name.resize(std::strlen(name.c_str()));
        for (char& c : name) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        std::string folder;
        if (!name.compare(0, 7, "levels\\")) {
            const size_t a = name.find('\\', 7), b = a == std::string::npos ? a : name.find('\\', a + 1);
            if (b != std::string::npos) folder = name.substr(7, b - 7);
        }
        pos += len;
        const uint32_t at = ArchiveU32(p.bytes.data() + pos), n = ArchiveU32(p.bytes.data() + pos + 4); pos += 8;
        if (n && uint64_t(at) + n <= UINT32_MAX) {
            ranges.emplace_back(at, n);
            if (!folder.empty()) folders.emplace_back(at, n, folder);
        }
    }
    key.back() = 'D';
    std::sort(folders.begin(), folders.end());
    archive_folders_[key] = std::move(folders);
    std::sort(ranges.begin(), ranges.end());
    archive_ranges_[key] = std::move(ranges);
}

void StreamService::LevelMusic(int32_t file, uint32_t offset) {
    const auto f = files_.find(file);
    if (f == files_.end()) return;
    const auto table = archive_folders_.find(ArchiveKey(f->second.path));
    if (table == archive_folders_.end()) return;
    auto it = std::upper_bound(table->second.begin(), table->second.end(), std::make_tuple(offset, UINT32_MAX, std::string("\x7f")));
    if (it == table->second.begin()) return;
    --it;
    if (offset - std::get<0>(*it) >= std::get<1>(*it)) return;
    level_music_ = std::get<2>(*it);
    music_cache_.Admit(level_music_);
}

void StreamService::Predict(int32_t file, uint32_t offset, uint32_t size) {
    const auto f = files_.find(file);
    if (f == files_.end() || !f->second.found) return;
    const IsoFs* iso = Disc();

    const uint64_t end = uint64_t(offset) + size;
    uint64_t limit = f->second.size;
    const auto table = archive_ranges_.find(ArchiveKey(f->second.path));
    if (table != archive_ranges_.end()) {
        auto it = std::upper_bound(table->second.begin(), table->second.end(), std::make_pair(offset, UINT32_MAX));
        if (it != table->second.begin()) {
            --it;
            const uint64_t chunk_end = uint64_t(it->first) + it->second;
            if (offset >= it->first && end <= chunk_end && chunk_end <= f->second.size) limit = chunk_end;
        }
    }

    if (end < limit) iso->Prefetch(f->second.image_offset + end,
        size_t(std::min<uint64_t>(512u << 10, limit - end)));
}

void StreamService::Read(int32_t channel, int32_t file, uint32_t offset, uint32_t size, uint32_t destination) {
    if (!ValidChannel(channel)) return;
    LevelMusic(file, offset);
    Issue(channel, file, offset, size, destination, false, 0);
}

void StreamService::ReadSoundBank(int32_t channel, uint32_t bank, int32_t file, uint32_t offset, uint32_t size) {
    if (!ValidChannel(channel)) return;
    Issue(channel, file, offset, size, 0, true, bank);
}

bool StreamService::IsReading(int32_t channel) {
    if (!ValidChannel(channel)) return false;
    const auto it = pending_.find(channel);
    if (it == pending_.end()) return false;
    if (it->second.end_ms <= FrameEndMs()) {
        LandPending(channel);
        return false;
    }

    te::Services::Get().clock().ChargeInFrame(kPollChargeTicks);
    return true;
}

int32_t StreamService::Wait(int32_t channel) {
    if (!ValidChannel(channel)) return 0;

    while (true) {
        const auto it = pending_.find(channel);
        if (it == pending_.end()) return 0;
        if (it->second.end_ms <= FrameEndMs()) break;
        te::Services::Get().clock().Boundary();
    }
    LandPending(channel);
    return 0;
}

void StreamService::OnBoundary(uint64_t boundary) {

    (void)boundary;
    const double ended = static_cast<double>(DriveFrame()) * DriveFrameMs();
    std::vector<int32_t> due;
    for (const auto& [channel, p] : pending_) if (p.end_ms <= ended) due.push_back(channel);
    for (int32_t channel : due) {
        LandPending(channel);
    }
}

StreamService& Service() {
    static StreamService service;
    return service;
}

}
