#include "games/slus_20909_crash_twinsanity/hle/saves/save_storage.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace title {
namespace fs = std::filesystem;
namespace {
int32_t IoError() { return errno == ENOSPC ? SaveStorage::kNoSpace : SaveStorage::kIoError; }
bool Directory(const fs::path& path) {
    std::error_code error;
    const auto type = fs::symlink_status(path, error).type();
    return !error && type == fs::file_type::directory;
}
uint32_t LastStep(SaveStorage::Operation op) { return op == SaveStorage::Operation::Write ? 5 : 1; }
}

SaveStorage::SaveStorage(Config config) : config_(std::move(config)) {}

struct SaveStorage::MirrorSlot {
    bool loaded = false, exists = false;
    struct Dir {
        std::map<std::string, std::vector<uint8_t>> files;
        bool other = false;
    };
    std::map<std::string, Dir> dirs;
    std::map<std::string, uint64_t> root_files;
    uint64_t nested_used = 0;
};

struct SaveStorage::MirrorWriter {
    std::mutex lock;
    std::condition_variable wake, idle;
    std::deque<std::function<void()>> ops;
    bool stop = false, busy = false;
    std::thread thread;
    MirrorWriter() : thread([this] { Run(); }) {}
    ~MirrorWriter() {
        {
            std::lock_guard<std::mutex> g(lock);
            stop = true;
        }
        wake.notify_all();
        thread.join();
    }
    void Push(std::function<void()> op) {
        {
            std::lock_guard<std::mutex> g(lock);
            ops.push_back(std::move(op));
        }
        wake.notify_all();
    }
    void Flush() {
        std::unique_lock<std::mutex> l(lock);
        idle.wait(l, [this] { return ops.empty() && !busy; });
    }
    void Run() {
        std::unique_lock<std::mutex> l(lock);
        for (;;) {
            wake.wait(l, [this] { return stop || !ops.empty(); });
            if (ops.empty()) {
                if (stop) return;
                continue;
            }
            auto op = std::move(ops.front());
            ops.pop_front();
            busy = true;
            l.unlock();
            op();
            l.lock();
            busy = false;
            if (ops.empty()) idle.notify_all();
        }
    }
};

namespace {
std::mutex g_mirrors_lock;
std::vector<SaveStorage*> g_mirrors;
void FlushAllMirrors() {
    std::vector<SaveStorage*> all;
    {
        std::lock_guard<std::mutex> g(g_mirrors_lock);
        all = g_mirrors;
    }
    for (SaveStorage* s : all) s->FlushMirror();
}

uint64_t NestedUsed(const fs::path& directory) {
    uint64_t used = 2, files = 0;
    std::error_code error;
    for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        if (it->path().filename().string().find(".save-pending-") == 0) continue;
        const auto status = it->symlink_status(error);
        if (error) break;
        if (fs::is_symlink(status)) continue;
        if (fs::is_directory(status)) used += NestedUsed(it->path());
        else if (fs::is_regular_file(status)) {
            used += SaveStorage::FileKilobytes(it->file_size(error));
            ++files;
        }
    }
    return used + (files + 1) / 2;
}
}

SaveStorage::~SaveStorage() {
    if (!writer_) return;
    writer_->Flush();
    {
        std::lock_guard<std::mutex> g(g_mirrors_lock);
        g_mirrors.erase(std::remove(g_mirrors.begin(), g_mirrors.end(), this), g_mirrors.end());
    }
    writer_.reset();
}

void SaveStorage::FlushMirror() {
    if (writer_) writer_->Flush();
}

SaveStorage::MirrorSlot& SaveStorage::Mirror(unsigned port, unsigned slot) const {
    auto& m = mirror_[port * 2 + slot];
    if (!m) m = std::make_unique<MirrorSlot>();
    if (m->loaded) return *m;
    m->loaded = true;
    const fs::path root = Root(port, slot);
    m->exists = Directory(root);
    if (!m->exists) return *m;
    std::error_code error;
    for (fs::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
        const std::string name = it->path().filename().string();
        if (name.find(".save-pending-") == 0) continue;
        const auto status = it->symlink_status(error);
        if (error) break;
        if (fs::is_symlink(status)) continue;
        if (fs::is_regular_file(status)) {
            m->root_files[name] = it->file_size(error);
            continue;
        }
        if (!fs::is_directory(status)) continue;
        auto& dir = m->dirs[name];
        {
            constexpr const char* kPrevious = ".save-pending-previous-";
            std::vector<fs::path> leftovers;
            std::error_code scan_error;
            for (fs::directory_iterator scan(it->path(), scan_error), stop; !scan_error && scan != stop; scan.increment(scan_error))
                if (scan->path().filename().string().rfind(kPrevious, 0) == 0) leftovers.push_back(scan->path());
            for (const fs::path& leftover : leftovers) {
                const fs::path original = it->path() / leftover.filename().string().substr(std::char_traits<char>::length(kPrevious));
                std::error_code ignored;
                if (!fs::exists(original, ignored)) fs::rename(leftover, original, ignored);
                else fs::remove(leftover, ignored);
            }
        }
        std::error_code inner;
        for (fs::directory_iterator f(it->path(), inner), stop; !inner && f != stop; f.increment(inner)) {
            const std::string file = f->path().filename().string();
            if (file.find(".save-pending-") == 0) continue;
            const auto file_status = f->symlink_status(inner);
            if (inner) break;
            if (fs::is_symlink(file_status)) { dir.other = true; continue; }
            if (fs::is_directory(file_status)) {
                dir.other = true;
                m->nested_used += NestedUsed(f->path());
                continue;
            }
            if (!fs::is_regular_file(file_status)) { dir.other = true; continue; }
            std::ifstream stream(f->path(), std::ios::binary);
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
            dir.files[file] = std::move(bytes);
        }
    }
    return *m;
}

SaveStorage::Info SaveStorage::Inspect(unsigned port, unsigned slot) const {
    if (port >= 2 || slot >= 2 || !config_.present[port * 2 + slot]) return {0, 0, 0};
    const MirrorSlot& m = Mirror(port, slot);
    if (!m.exists) return {0, 0, 0};
    uint64_t used = m.nested_used;
    for (const auto& [name, dir] : m.dirs) {
        used += 2 + (dir.files.size() + 1) / 2;
        for (const auto& [file, bytes] : dir.files) used += FileKilobytes(bytes.size());
    }
    if (!m.root_files.empty()) {
        used += (m.root_files.size() + 1) / 2;
        for (const auto& [file, bytes] : m.root_files) used += FileKilobytes(bytes);
    }
    const uint64_t remaining = used >= config_.capacity_kilobytes ? 0 : config_.capacity_kilobytes - used;
    return {2, static_cast<int32_t>(std::min<uint64_t>(remaining, INT32_MAX)), 1};
}

SaveStorage::Completion SaveStorage::Execute(const Request& request) {
    Completion out;
    out.operation = request.operation;
    out.result = {1, static_cast<int32_t>(LastStep(request.operation)), 0};
    const auto fail = [&](int32_t error) { out.result.succeeded = 0; out.result.error = error; };
    if (!writer_) {
        writer_ = std::make_unique<MirrorWriter>();
        std::lock_guard<std::mutex> g(g_mirrors_lock);
        static const bool registered = [] { return std::atexit(FlushAllMirrors) == 0; }();
        (void)registered;
        g_mirrors.push_back(this);
    }
    MirrorSlot& m = Mirror(request.port, request.slot);
    const fs::path root = Root(request.port, request.slot), save = SavePath(request);
    const std::string save_name = save.filename().string();
    if (request.operation == Operation::Format) {
        m.exists = true;
        m.dirs.clear();
        m.root_files.clear();
        m.nested_used = 0;
        out.committed = true;
        const std::string suffix = std::to_string(state_.generation) + "-" + std::to_string(state_.sequence);
        writer_->Push([this, root, suffix] {
            std::error_code error;
            const fs::path empty = config_.directory / (".save-format-" + root.filename().string() + "-" + suffix);
            const fs::path backup = config_.directory / (".save-backup-" + root.filename().string() + "-" + suffix);
            if (fs::exists(empty, error) || fs::exists(backup, error) || !fs::create_directory(empty, error) || error) return;
            fs::rename(root, backup, error);
            if (error) { fs::remove(empty, error); return; }
            fs::rename(empty, root, error);
            if (error) {
                std::error_code rollback;
                fs::rename(backup, root, rollback);
                fs::remove(empty, rollback);
                return;
            }
            fs::remove_all(backup, error);
        });
        return out;
    }
    if (request.operation == Operation::Create) {
        if (m.dirs.count(save_name)) return out;
        if (m.root_files.count(save_name)) { fail(kDenied); return out; }
        if (StorageInfo(request.port, request.slot).free_kilobytes < 2) { fail(kNoSpace); return out; }
        m.dirs[save_name];
        out.committed = true;
        writer_->Push([save] {
            std::error_code error;
            fs::create_directory(save, error);
        });
        return out;
    }
    if (m.root_files.count(save_name)) { fail(kDenied); return out; }
    const auto found = m.dirs.find(save_name);
    if (found == m.dirs.end()) {
        if (request.operation == Operation::Measure) out.value = -1;
        else fail(kMissing);
        return out;
    }
    MirrorSlot::Dir& dir = found->second;
    if (dir.other) { fail(kDenied); return out; }
    if (dir.files.size() > 18) { fail(kNoSpace); return out; }
    if (request.operation == Operation::Measure) {
        uint64_t size = 0;
        for (const auto& [file, bytes] : dir.files) size += FileKilobytes(bytes.size());
        out.value = static_cast<int32_t>(size + (dir.files.size() + 1) / 2 + 2);
        return out;
    }
    const auto file = dir.files.find(request.file);
    const bool exists = file != dir.files.end();
    if (request.operation == Operation::Find) {
        if (!exists) { out.value = -1; return out; }
        const uint64_t bytes = file->second.size();
        if (bytes > INT32_MAX) { fail(kIoError); return out; }
        out.value = static_cast<int32_t>(bytes);
        out.entry.size = static_cast<uint32_t>(bytes);
        out.entry.attributes = 0x8497;
        out.entry.created = out.entry.modified = {{0, 0, 0, 12, 15, 8, 0xea, 7}};
        std::copy(request.file.begin(), request.file.end(), out.entry.name.begin());
        out.entry_present = true;
        return out;
    }
    if (request.operation == Operation::Read) {
        if (!exists) { fail(kMissing); return out; }
        const size_t n = std::min<size_t>(request.size, file->second.size());
        out.bytes.assign(file->second.begin(), file->second.begin() + static_cast<std::ptrdiff_t>(n));
        return out;
    }
    if (!exists && dir.files.size() >= 18) { fail(kNoSpace); return out; }
    const uint64_t old_size = exists ? file->second.size() : 0;
    const uint64_t free = static_cast<uint32_t>(StorageInfo(request.port, request.slot).free_kilobytes);
    const uint64_t entry_cost = !exists && dir.files.size() % 2 == 0 ? 1 : 0;
    if (FileKilobytes(request.size) + entry_cost > free + FileKilobytes(old_size)) { fail(kNoSpace); return out; }
    dir.files[request.file] = request.bytes;
    out.committed = true;
    writer_->Push([this, path = save / request.file, bytes = request.bytes] {
        bool committed = false;
        AtomicWrite(path, bytes, committed);
    });
    return out;
}

bool SaveStorage::Name(const std::string& name, size_t max, bool empty) {
    if ((!empty && name.empty()) || name.size() > max || name == "." || name == "..") return false;
    for (unsigned char c : name) if (c < 32 || c == 127 || c == '/' || c == '\\' || c == ':' || c == '*') return false;
    return true;
}
bool SaveStorage::Supported(Operation op) {
    return op == Operation::Format || op == Operation::Create || op == Operation::Measure ||
           op == Operation::Find || op == Operation::Read || op == Operation::Write;
}
fs::path SaveStorage::Root(unsigned port, unsigned slot) const {

    return config_.directory / ("mc" + std::to_string(port) + (slot ? "_slot" + std::to_string(slot) : ""));
}
fs::path SaveStorage::SavePath(const Request& request) const {
    return Root(request.port, request.slot) / (state_.region + state_.product + request.save);
}
uint64_t SaveStorage::FileKilobytes(uint64_t bytes) { return bytes / 1024 + (bytes % 1024 != 0); }
uint64_t SaveStorage::SaveBytes(uint64_t kilobytes, uint64_t files) {
    const uint64_t entries = files / 2 + files % 2;
    const uint64_t max = UINT64_MAX / 1024;
    if (kilobytes > max - 2 || entries > max - 2 - kilobytes) return UINT64_MAX;
    return (kilobytes + entries + 2) * 1024;
}
bool SaveStorage::Start() {
    if (config_.directory.empty()) return false;
    std::error_code error;
    for (unsigned port = 0; port < 2; ++port) for (unsigned slot = 0; slot < 2; ++slot) {
        if (!config_.present[port * 2 + slot]) continue;
        fs::create_directories(Root(port, slot), error);
        if (error || !Directory(Root(port, slot))) return false;
    }
    return true;
}
bool SaveStorage::Initialise(const std::string& region, const std::string& product) {
    if (state_.initialised) return state_.region == region && state_.product == product;
    if (!Name(region, 7) || !Name(product, 15) || !Start()) return false;
    state_.region = region;
    state_.product = product;
    state_.initialised = true;
    for (unsigned port = 0; port < 2; ++port) for (unsigned slot = 0; slot < 2; ++slot)
        state_.info[port * 2 + slot] = Inspect(port, slot);
    return true;
}
SaveStorage::Info SaveStorage::StorageInfo(unsigned port, unsigned slot) const {
    return port < 2 && slot < 2 ? state_.info[port * 2 + slot] : Info{0, 0, 0};
}
SaveStorage::Result SaveStorage::GetResult(Operation operation) const {
    const auto index = static_cast<unsigned>(operation);
    return index < state_.results.size() ? state_.results[index] : Result{0, 0, kInvalid};
}
bool SaveStorage::Submit(Request request, uint64_t boundary) {
    if (Busy() || state_.completion) return false;
    if (!state_.initialised || request.port >= 2 || request.slot >= 2 ||
        !config_.present[request.port * 2 + request.slot])
        return false;
    if (!Supported(request.operation) || !Name(request.save, 31, true) ||
        ((request.operation == Operation::Read || request.operation == Operation::Write ||
          request.operation == Operation::Find) && !Name(request.file, 31)) ||
        request.file.rfind(".save-pending-", 0) == 0 ||
        request.size > kMaxBytes ||
        state_.sequence == UINT64_MAX ||
        (request.operation != Operation::Write && !request.bytes.empty()) ||
        (request.operation == Operation::Write &&
         (request.bytes.size() != request.size || request.save.size() >= 10)))
        return false;
    state_.results = {};
    state_.pending = std::move(request);
    state_.active = Operation::Checking;
    state_.progress = 0;
    state_.last_boundary = boundary;
    ++state_.sequence;
    return true;
}
SaveStorage::Operation SaveStorage::Update(unsigned port, unsigned slot, uint64_t boundary) {
    if (!Busy()) {
        if (port < 2 && slot < 2) {
            const unsigned index = port * 2 + slot;
            if (!inspected_[index] || state_.info[index].type != 2 || boundary < inspected_at_[index] ||
                (config_.inspect_boundaries && boundary - inspected_at_[index] >= config_.inspect_boundaries)) {
                state_.info[index] = Inspect(port, slot);
                inspected_[index] = true;
                inspected_at_[index] = boundary;
            }
        }
        return Operation::None;
    }

    if (boundary <= state_.last_boundary) return state_.active;
    state_.last_boundary = boundary;
    const auto& request = state_.pending;
    state_.info[request.port * 2 + request.slot] = Inspect(request.port, request.slot);
    if (state_.info[request.port * 2 + request.slot].type != 2) {
        Completion completion;
        completion.operation = request.operation;
        completion.result = {0, static_cast<int32_t>(state_.progress), kNoStorage};
        Finish(std::move(completion));
        return Operation::NoStorage;
    }
    if (state_.active == Operation::Checking) {
        state_.active = request.operation;
    } else if (state_.progress++ >= LastStep(request.operation)) {
        Finish(Execute(request));
    }
    return state_.active;
}
void SaveStorage::Finish(Completion completion) {
    completion.generation = state_.generation;
    completion.sequence = state_.sequence;
    state_.results[static_cast<unsigned>(completion.operation)] = completion.result;
    state_.completion = std::move(completion);
    state_.active = Operation::None;
    state_.pending = {};
    state_.progress = 0;
    inspected_.fill(false);
}
std::optional<SaveStorage::Completion> SaveStorage::TakeCompletion() {
    auto completion = std::move(state_.completion);
    state_.completion.reset();
    return completion;
}
int32_t SaveStorage::AtomicWrite(const fs::path& file, const std::vector<uint8_t>& bytes, bool& committed) {
    fs::path temporary;
    FILE* stream = nullptr;
    std::string pattern = (file.parent_path() / ".save-pending-XXXXXX").string();
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back(0);
    const int descriptor = mkstemp(name.data());
    if (descriptor < 0) return IoError();
    temporary = name.data();
    stream = fdopen(descriptor, "wb");
    if (!stream) close(descriptor);
    const auto remove_temporary = [&] { std::error_code ignored; fs::remove(temporary, ignored); };
    if (!stream) { remove_temporary(); return kIoError; }
    int32_t result = 0;
    if (!bytes.empty() && std::fwrite(bytes.data(), 1, bytes.size(), stream) != bytes.size()) result = IoError();
    if (!result && std::fflush(stream) != 0) result = IoError();
    if (!result && fsync(fileno(stream)) != 0) result = IoError();
    if (std::fclose(stream) != 0 && !result) result = IoError();
    if (result) { remove_temporary(); return result; }

    const std::string previous = (file.parent_path() / (".save-pending-previous-" + file.filename().string())).string();
    std::remove(previous.c_str());
    const bool moved = std::rename(file.c_str(), previous.c_str()) == 0;
    if (!moved && errno != ENOENT) { result = IoError(); remove_temporary(); return result; }
    if (std::rename(temporary.c_str(), file.c_str()) != 0) {
        result = IoError();
        if (moved) std::rename(previous.c_str(), file.c_str());
        remove_temporary();
        return result;
    }
    committed = true;
    if (moved) std::remove(previous.c_str());
    return 0;
}

}
