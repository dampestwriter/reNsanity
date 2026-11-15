#pragma once
#include <cstdint>
#include "music_cache.h"
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

class ReadTiming;

namespace te::stream {

class StreamService {
public:
    enum class State : int32_t { Stopped = 0, Running = 1, DiscError = 2, Recovering = 4 };
    static constexpr int32_t kNoFile = -1;

    static constexpr uint32_t kBufferPoolBytes = 0x1A0000;
    static constexpr uint32_t kBufferPoolBase = 0x00060000;

    static constexpr double kFrameMs = 16667.0 / 1000.0;
    static constexpr uint64_t kPollChargeTicks = 92;

    int32_t Initialise(int32_t channels, uint32_t workMemory);
    int32_t Update();
    State GetState() const { return state_; }
    void AttachBuffer(int32_t channel, uint32_t size, uint32_t soundAddress, uint32_t soundSize);
    void DetachBuffer(int32_t channel);
    uint32_t FreeBufferMemory() const;
    int32_t OpenFile(const char* path);
    uint32_t FileSize() const { return last_size_; }
    void CloseFile(int32_t file);
    void Read(int32_t channel, int32_t file, uint32_t offset, uint32_t size, uint32_t destination);
    void ReadSoundBank(int32_t channel, uint32_t bank, int32_t file, uint32_t offset, uint32_t size);
    bool IsReading(int32_t channel);
    int32_t Wait(int32_t channel);

    bool FileExtent(int32_t file, uint64_t& image_offset, uint32_t& size) const;

    uint32_t ChannelBufferAddress(int32_t channel) const;
    using FileBytes = std::shared_ptr<const std::vector<uint8_t>>;
    bool ReadFileLease(int32_t file, uint32_t offset, uint32_t size, FileBytes& out);

    bool ReadMusicAsset(int32_t file, uint32_t offset, uint32_t size, title::MusicAsset::Layout layout,
                        title::MusicAsset::Data& out, std::string* error);
    uint64_t MusicCacheBytes() const { return music_cache_.Bytes(); }

    void PrefetchFileBytes(int32_t file, uint32_t offset, uint32_t size);

    void OnBoundary(uint64_t boundary);
    bool Instant() const { return instant_; }
    int32_t Channels() const { return channels_; }
    void Reset();


private:
    MusicCache music_cache_;
    struct Window;
    static constexpr uint64_t kWindowCacheBytes = 64ull << 20;
    std::shared_ptr<Window> CachedWindow(int32_t file, uint32_t offset, uint32_t size, bool start, uint32_t* clamped);
    std::map<std::pair<uint64_t, uint32_t>, std::shared_ptr<Window>> windows_;
    uint64_t window_clock_ = 0;
    struct File { uint64_t image_offset = 0; uint32_t size = 0; std::string path; bool found = false; };
    struct Buffer { uint32_t address = 0, size = 0, sound_address = 0, sound_size = 0; };

    struct Pending {
        int32_t file = kNoFile;
        uint32_t offset = 0, size = 0, destination = 0;
        bool bank = false;
        uint32_t bank_id = 0;
        uint64_t issue_boundary = 0;
        double end_ms = 0;
        mutable std::vector<uint8_t> bytes;
        mutable bool deferred = false;
    };

    std::map<std::string, std::vector<std::pair<uint32_t, uint32_t>>> archive_ranges_;

    std::map<std::string, std::vector<std::tuple<uint32_t, uint32_t, std::string>>> archive_folders_;
    std::string level_music_;
    void LevelMusic(int32_t file, uint32_t offset);
    void LearnArchive(const Pending& p);
    void Predict(int32_t file, uint32_t offset, uint32_t size);
    void Materialize(const Pending& p) const;
    bool ValidChannel(int32_t channel) const;
    uint32_t AllocateBuffer(uint32_t size) const;

    bool ReadRange(int32_t file, uint32_t offset, uint32_t size, std::vector<uint8_t>& out);
    void Issue(int32_t channel, int32_t file, uint32_t offset, uint32_t size, uint32_t destination, bool bank, uint32_t bank_id);
    void Land(int32_t channel, Pending& p);
    void LandPending(int32_t channel);
    void ForceChannel(int32_t channel);
    uint64_t Boundary() const;

    uint64_t DriveFrame() const;
    static double DriveFrameMs();
    double FrameEndMs() const;
    void EnsureTiming();

    State state_ = State::Stopped;
    int32_t channels_ = 0;
    int32_t next_file_ = 0;
    uint32_t last_size_ = 0;
    bool instant_ = false, instant_set_ = false, hooked_ = false;
    std::map<int32_t, File> files_;
    std::map<int32_t, Buffer> buffers_;
    std::map<int32_t, Pending> pending_;
    std::unique_ptr<ReadTiming> timing_;
};

StreamService& Service();


}
