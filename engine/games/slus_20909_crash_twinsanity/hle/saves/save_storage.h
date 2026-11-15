#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace title {

class SaveStorage {
public:
    enum class Operation : int32_t {
        NoStorage = -1, None = 0, Checking = 1, Format = 2, Create = 4,
        Measure = 7, Find = 8, Read = 10, Write = 11
    };
    static constexpr uint32_t kMaxBytes = 2 * 1024 * 1024;
    static constexpr int32_t kNoSpace = -3, kMissing = -4, kDenied = -5,
                             kIoError = -8, kInvalid = -22, kNoStorage = -100;
    struct Result { int32_t succeeded = -1, step = -1, error = 0; };
    struct Info { int32_t type = -1, free_kilobytes = -1, formatted = -1; };
    struct FileEntry {
        std::array<uint8_t, 8> created{}, modified{};
        uint32_t size = 0;
        uint16_t attributes = 0, reserved0 = 0;
        std::array<uint32_t, 2> reserved1{};
        std::array<char, 32> name{};
    };
    static_assert(sizeof(FileEntry) == 64, "game directory entry ABI");
    struct Request {
        Operation operation = Operation::None;
        unsigned port = 0, slot = 0;
        std::string save, file;
        uint32_t size = 0;
        std::vector<uint8_t> bytes;
    };
    struct Completion {

        Completion();
        uint64_t generation = 0, sequence = 0;
        Operation operation = Operation::None;
        Result result;
        int32_t value = -1;
        FileEntry entry;
        bool entry_present = false, committed = false;
        std::vector<uint8_t> bytes;
    };
    struct Config {
        std::filesystem::path directory;
        uint32_t capacity_kilobytes = 0x1f00;
        std::array<bool, 4> present{{true, true, true, true}};

        uint64_t inspect_boundaries = 60;
    };
    struct State {
        bool initialised = false;
        std::string region, product;
        uint64_t generation = 1, sequence = 0, last_boundary = 0;
        Operation active = Operation::None;
        uint32_t progress = 0;
        Request pending;
        std::array<Result, 12> results{};
        std::array<Info, 4> info{};
        std::optional<Completion> completion;
    };

    explicit SaveStorage(Config config);
    ~SaveStorage();

    void FlushMirror();

    bool Start();
    bool Initialise(const std::string& region, const std::string& product);
    bool Submit(Request request, uint64_t boundary);
    Operation Update(unsigned port, unsigned slot, uint64_t boundary);
    Info StorageInfo(unsigned port, unsigned slot) const;
    Result GetResult(Operation operation) const;
    std::optional<Completion> TakeCompletion();
    bool Busy() const { return state_.active != Operation::None; }
    Operation Active() const { return state_.active; }

    static uint64_t FileKilobytes(uint64_t bytes);
    static uint64_t SaveBytes(uint64_t kilobytes, uint64_t files);

private:
    static bool Name(const std::string& name, size_t max, bool empty = false);
    static bool Supported(Operation operation);
    std::filesystem::path Root(unsigned port, unsigned slot) const;
    std::filesystem::path SavePath(const Request& request) const;

    struct MirrorSlot;
    struct MirrorWriter;
    MirrorSlot& Mirror(unsigned port, unsigned slot) const;
    Info Inspect(unsigned port, unsigned slot) const;
    Completion Execute(const Request& request);
    std::unique_ptr<MirrorWriter> writer_;
    mutable std::array<std::unique_ptr<MirrorSlot>, 4> mirror_{};
    int32_t AtomicWrite(const std::filesystem::path& file, const std::vector<uint8_t>& bytes, bool& committed);
    void Finish(Completion completion);
    Config config_;
    State state_;

    std::array<uint64_t, 4> inspected_at_{};
    std::array<bool, 4> inspected_{};
};

inline SaveStorage::Completion::Completion() = default;

}
