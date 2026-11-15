#include "music_cache.h"
#include <algorithm>
#include <cctype>
#include <new>
#include "engine/cdvd/iso_fs.h"
namespace te::stream {
namespace {
uint32_t Le32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
}
namespace {

struct LevelMusic { const char* folder; std::vector<uint8_t> ids; };
const std::vector<LevelMusic>& LevelTable() {
    static const std::vector<LevelMusic> table = {
        {"earth\\cavern", {0x1C, 0x53, 0x54, 0x7E, 0x80, 0x86}},
        {"earth\\docamok", {0x00, 0x1B, 0x1D, 0x1F, 0x5A, 0x0B, 0x57, 0x87}},
        {"earth\\hub", {0x00, 0x1B, 0x1E, 0x20, 0x50, 0x59, 0x5A, 0x2F, 0x30, 0x34, 0x4A, 0x64, 0x7F, 0x81, 0x82, 0x84, 0x85, 0x8D}},
        {"earth\\totem", {0x0A, 0x1F, 0x59, 0x5A, 0x8A}},
        {"ice\\highseas", {0x09, 0x21, 0x24, 0x37, 0x38, 0x59, 0x6D, 0x6E, 0x70, 0x76, 0x77, 0x78, 0x79, 0x0F, 0x2A, 0x2B, 0x2D, 0x2E, 0x43, 0x44, 0x55}},
        {"ice\\hub", {0x00, 0x21, 0x25, 0x59, 0x69, 0x77, 0x7C, 0x10, 0x12, 0x16, 0x31, 0x4B, 0x4C, 0x5D, 0x62}},
        {"ice\\iceclimb", {0x21, 0x23, 0x70, 0x74, 0x15, 0x17, 0x18, 0x19, 0x63}},
        {"ice\\slipslide", {0x00, 0x1B, 0x21, 0x36, 0x74, 0x8B, 0x13, 0x32}},
        {"school\\boiler", {0x28, 0x39, 0x79, 0x7A, 0x7B, 0x0F, 0x45, 0x46, 0x47, 0x48}},
        {"school\\cortex", {0x08, 0x29, 0x89, 0x14}},
        {"school\\crash", {0x08, 0x3A, 0x67, 0x89, 0x2F, 0x33}},
        {"school\\madame", {0x25, 0x3B, 0x89, 0x42}},
        {"school\\rooftop", {0x29, 0x6F, 0x89, 0x51, 0x52}},
        {"school\\sch_hub", {0x25, 0x5B, 0x7C, 0x1A}},
        {"altearth\\core", {0x3E, 0x3F, 0x40, 0x68, 0x69, 0x49, 0x3A, 0x1C, 0x88, 0x1E, 0x23, 0x25, 0x29, 0x36, 0x3C, 0x3D, 0x1B}},
        {"altearth\\hub", {0x07, 0x3D, 0x3E, 0x3F, 0x59, 0x72, 0x58, 0x65}},
        {"altearth\\lab", {0x3C, 0x71, 0x89, 0x56}},
        {"altearth\\rockslid", {0x1B, 0x3C, 0x3D, 0x71, 0x2C}},
    };
    return table;
}
bool ReadTable(const IsoFs& iso, const std::string& path, std::vector<uint8_t>& metadata, uint32_t& count, uint32_t& block) {
    std::string header = path.substr(0, path.find(';'));
    std::transform(header.begin(), header.end(), header.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (header.size() < 3 || header.compare(header.size() - 3, 3, ".mb")) return false;
    header.back() = 'h';
    IsoFs::Entry mh;
    if (!iso.Find(header, mh) || mh.directory || mh.size < 8 || mh.size > (1u << 20)) return false;
    metadata.resize(mh.size);
    if (iso.ReadBytes(uint64_t(mh.lba) * IsoFs::kSectorSize, mh.size, metadata.data()) != mh.size) return false;
    count = Le32(metadata.data()); block = Le32(metadata.data() + 4);
    return count && count <= (metadata.size() - 8) / 20;
}
}
void MusicCache::IndexBank(const IsoFs& iso, const std::string& path, uint64_t extent, uint32_t size) {
    std::vector<uint8_t> metadata;
    uint32_t count = 0, block = 0;
    if (!ReadTable(iso, path, metadata, count, block)) return;
    Bank bank{&iso, extent, {}};
    for (uint32_t i = 0; i < count; ++i) {
        const auto* p = metadata.data() + 8 + size_t(i) * 20;
        const Track t{Le32(p), Le32(p + 4), Le32(p + 8)};
        if (t.kind == 1 && (!block || block % 16 || t.n % (uint64_t(block) * 2))) return;
        if (t.kind < 2 && (!t.n || t.off > size || t.n > size - t.off || t.n > (64u << 20))) return;
        bank.tracks.push_back(t);
    }
    std::string name = path.substr(0, path.find(';'));
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    const bool music = name.size() >= 8 && name.compare(name.size() - 8, 8, "music.mb") == 0;
    std::lock_guard<std::mutex> l(mutex_);
    (music ? music_ : voice_) = std::move(bank);
    RefreshLocked();
}
void MusicCache::Admit(const std::string& folder) {
    std::lock_guard<std::mutex> l(mutex_);
    if (folder == folder_) return;
    previous_ = folder_;
    folder_ = folder;
    RefreshLocked();
}
void MusicCache::RefreshLocked() {
    std::set<std::pair<uint64_t, uint32_t>> wanted;
    std::vector<std::pair<const IsoFs*, std::pair<uint64_t, uint32_t>>> order;
    for (const std::string* f : {&folder_, &previous_}) {
        for (const LevelMusic& level : LevelTable()) {
            if (*f != level.folder) continue;
            for (const uint8_t id : level.ids) {
                if (music_.tracks.empty()) break;
                const Bank* bank = &music_;
                Track t = music_.tracks[std::min<size_t>(id, music_.tracks.size() - 1)];
                if (t.kind == 2) {
                    if (voice_.tracks.empty()) continue;
                    bank = &voice_;
                    t = voice_.tracks[std::min<size_t>(id, voice_.tracks.size() - 1)];
                    if (t.kind > 1) continue;
                }
                const std::pair<uint64_t, uint32_t> w{bank->extent + t.off, t.n};
                if (wanted.insert(w).second) order.push_back({bank->iso, w});
            }
        }
    }
    for (auto it = raws_.begin(); it != raws_.end();) {
        if (wanted.count(it->first)) { ++it; continue; }
        bytes_ -= it->first.second;
        it = raws_.erase(it);
    }
    wanted_ = wanted;
    for (const auto& [iso, w] : order) {
        if (raws_.count(w)) continue;
        bool queued = false;
        for (const Job& j : jobs_) if (j.window && j.extent == w.first && j.size == w.second) queued = true;
        if (!queued) jobs_.push_back(Job{iso, std::string(), w.first, w.second, true});
    }
    if (!jobs_.empty()) {
        if (!worker_.joinable()) worker_ = std::thread([this] { Work(); });
        cv_.notify_one();
    }
}
bool MusicCache::ReadWindow(const IsoFs& iso, uint64_t at, uint32_t n) {
    {
        std::lock_guard<std::mutex> l(mutex_);
        if (!wanted_.count({at, n}) || raws_.count({at, n}) || n > kBudget - bytes_) return false;
    }
    try {
        auto raw = std::make_shared<std::vector<uint8_t>>(n);
        if (iso.ReadBytes(at, n, raw->data()) != n) return false;
        std::lock_guard<std::mutex> l(mutex_);

        if (!wanted_.count({at, n}) || n > kBudget - bytes_) return false;
        if (raws_.emplace(std::make_pair(at, n), std::move(raw)).second) bytes_ += n;
        return true;
    } catch (const std::bad_alloc&) { return false; }
}
void MusicCache::Work() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> l(mutex_);
            cv_.wait(l, [this] { return stop_.load() || !jobs_.empty(); });
            if (stop_.load()) return;
            job = jobs_.front();
            jobs_.pop_front();
        }
        ReadWindow(*job.iso, job.extent, job.size);
    }
}
void MusicCache::Clear() {
    if (worker_.joinable()) {
        { std::lock_guard<std::mutex> l(mutex_); stop_ = true; }
        cv_.notify_all();
        worker_.join();
    }
    std::lock_guard<std::mutex> l(mutex_);
    stop_ = false;
    jobs_.clear();
    raws_.clear(); bytes_ = 0;
    music_ = Bank(); voice_ = Bank(); folder_.clear(); previous_.clear(); wanted_.clear();
}
bool MusicCache::Find(uint64_t at, uint32_t size, title::MusicAsset::Layout layout, title::MusicAsset::Data& out) const {
    std::shared_ptr<const std::vector<uint8_t>> raw;
    {
        std::lock_guard<std::mutex> l(mutex_);
        const auto it = raws_.find({at, size});
        if (it == raws_.end()) return false;
        raw = it->second;
    }
    return title::MusicAsset::Read(*raw, layout, out);
}
bool MusicCache::Contains(uint64_t at, uint32_t size) const {
    std::lock_guard<std::mutex> l(mutex_);
    return raws_.count({at, size}) != 0;
}
}
