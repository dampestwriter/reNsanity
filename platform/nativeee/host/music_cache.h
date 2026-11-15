#pragma once
#include <cstdint>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <vector>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include "games/slus_20909_crash_twinsanity/hle/audio/music_asset.h"
class IsoFs;
namespace te::stream {

class MusicCache {
public:
    static constexpr uint64_t kBudget = 512ull << 20;
    ~MusicCache() { Clear(); }
    bool Find(uint64_t at, uint32_t size, title::MusicAsset::Layout layout, title::MusicAsset::Data& out) const;
    bool Contains(uint64_t at, uint32_t size) const;
    uint64_t Bytes() const { std::lock_guard<std::mutex> l(mutex_); return bytes_; }

    void Clear();

    void IndexBank(const IsoFs& iso, const std::string& path, uint64_t extent, uint32_t size);
    void Admit(const std::string& folder);
private:
    struct Job { const IsoFs* iso; std::string path; uint64_t extent; uint32_t size; bool window = false; };
    struct Track { uint32_t kind, n, off; };
    struct Bank { const IsoFs* iso = nullptr; uint64_t extent = 0; std::vector<Track> tracks; };
    void RefreshLocked();
    bool ReadWindow(const IsoFs& iso, uint64_t at, uint32_t n);
    Bank music_, voice_;
    std::string folder_, previous_;
    std::set<std::pair<uint64_t, uint32_t>> wanted_;
    void Work();
    using Key = std::tuple<uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, bool>;
    static Key MakeKey(uint64_t at, uint32_t size, title::MusicAsset::Layout l) {
        return {at, size, l.tracks, l.block_bytes, l.final_bytes, l.looping};
    }
    std::map<std::pair<uint64_t, uint32_t>, std::shared_ptr<const std::vector<uint8_t>>> raws_;
    uint64_t bytes_ = 0;
    mutable std::mutex mutex_;
    std::deque<Job> jobs_;
    std::condition_variable cv_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
};
}
