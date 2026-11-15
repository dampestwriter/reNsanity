#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "games/slus_20909_crash_twinsanity/hle/saves/save_storage.h"
#include "ps2e_seam.h"
#include "te_services.h"

namespace te {

namespace {
using Storage = title::SaveStorage;

bool String(uint32_t address, size_t max, std::string& out) {
    out.clear();
    const char* s = GuestString(address, max + 1);
    if (!s) return false;
    const size_t n = std::strlen(s);
    if (n > max) return false;
    out.assign(s, n);
    return true;
}

bool WriteGuest(uint32_t address, const void* bytes, size_t size) {
    uint8_t* p = GuestPointer(address, size);
    if (!p) return false;
    std::memcpy(p, bytes, size);
    return true;
}
}

Saves::Saves(Clock& clock, const Settings& settings) : clock_(clock) {
    Storage::Config config;
    config.directory = settings.mc_dir;

    config.present = {true, false, true, false};
    core_.reset(new Storage(config));
}
Saves::~Saves() = default;

int32_t Saves::Initialise(uint32_t region, uint32_t product) {
    std::string r, p;
    if (!String(region, 7, r) || !String(product, 15, p)) return 0;
    if (!started_) started_ = core_->Start();
    if (!started_) return 0;
    return core_->Initialise(r, p) ? 1 : 0;
}

void Saves::Publish() {
    auto completion = core_->TakeCompletion();
    if (!completion) return;
    if (completion->result.succeeded == 1) {
        if (outputs_.data && !completion->bytes.empty())
            WriteGuest(outputs_.data, completion->bytes.data(), std::min<size_t>(completion->bytes.size(), outputs_.size));
        if (outputs_.value) WriteGuest(outputs_.value, &completion->value, sizeof(completion->value));
        if (outputs_.entry && completion->entry_present)
            WriteGuest(outputs_.entry, &completion->entry, sizeof(completion->entry));
    }
    outputs_ = {};
}

int32_t Saves::Update(int32_t port, int32_t slot, uint32_t info) {
    Storage* card = core_.get();
    const auto operation = card->Update(unsigned(port), unsigned(slot), clock_.BoundaryCount());
    Publish();
    if (info) {
        const Storage::Info i = card->StorageInfo(unsigned(port), unsigned(slot));
        const int32_t words[3] = {i.type, i.free_kilobytes, i.formatted};
        WriteGuest(info, words, sizeof(words));
    }
    return static_cast<int32_t>(operation);
}

void Saves::GetResult(uint32_t out, int32_t operation) {
    const Storage::Result r = core_->GetResult(static_cast<Storage::Operation>(operation));
    const int32_t words[3] = {r.succeeded, r.step, r.error};
    WriteGuest(out, words, sizeof(words));
}

namespace {
int32_t SubmitRequest(Storage& storage, Storage::Request request, uint64_t boundary, bool valid) {
    if (!valid) return 0;
    const bool ok = storage.Submit(std::move(request), boundary);
    return ok ? 1 : 0;
}
}

int32_t Saves::Format(int32_t port, int32_t slot) {
    Storage::Request request;
    request.operation = Storage::Operation::Format;
    request.port = unsigned(port); request.slot = unsigned(slot);
    const int32_t ok = SubmitRequest(*core_, std::move(request), clock_.BoundaryCount(), true);
    if (ok) outputs_ = {};
    return ok;
}

int32_t Saves::CreateSave(int32_t port, int32_t slot, uint32_t save) {
    Storage::Request request;
    request.operation = Storage::Operation::Create;
    request.port = unsigned(port); request.slot = unsigned(slot);
    const bool valid = String(save, 31, request.save);
    const int32_t ok = SubmitRequest(*core_, std::move(request), clock_.BoundaryCount(), valid);
    if (ok) outputs_ = {};
    return ok;
}

int32_t Saves::MeasureSave(int32_t port, int32_t slot, uint32_t save, uint32_t kilobytes) {
    Storage::Request request;
    request.operation = Storage::Operation::Measure;
    request.port = unsigned(port); request.slot = unsigned(slot);
    const bool valid = String(save, 31, request.save) && GuestPointer(kilobytes, 4) != nullptr;
    const int32_t ok = SubmitRequest(*core_, std::move(request), clock_.BoundaryCount(), valid);
    if (ok) { outputs_ = {}; outputs_.value = kilobytes; }
    return ok;
}

int32_t Saves::FindFile(int32_t port, int32_t slot, uint32_t file, uint32_t save, uint32_t size, uint32_t entry) {
    Storage::Request request;
    request.operation = Storage::Operation::Find;
    request.port = unsigned(port); request.slot = unsigned(slot);
    bool valid = String(file, 31, request.file) && String(save, 31, request.save);
    valid = valid && (!size || GuestPointer(size, 4)) && (!entry || GuestPointer(entry, sizeof(Storage::FileEntry)));
    const int32_t ok = SubmitRequest(*core_, std::move(request), clock_.BoundaryCount(), valid);
    if (ok) { outputs_ = {}; outputs_.value = size; outputs_.entry = entry; }
    return ok;
}

int32_t Saves::Write(int32_t port, int32_t slot, uint32_t save, uint32_t file, uint32_t data, int32_t size) {
    Storage::Request request;
    request.operation = Storage::Operation::Write;
    request.port = unsigned(port); request.slot = unsigned(slot);
    bool valid = size >= 0 && String(save, 31, request.save) && String(file, 31, request.file);
    request.size = static_cast<uint32_t>(size < 0 ? 0 : size);
    valid = valid && request.size <= Storage::kMaxBytes;
    if (valid && request.size) {
        const uint8_t* p = GuestPointer(data, request.size);
        valid = p != nullptr;
        if (valid) request.bytes.assign(p, p + request.size);
    }
    const int32_t ok = SubmitRequest(*core_, std::move(request), clock_.BoundaryCount(), valid);
    if (ok) outputs_ = {};
    return ok;
}

int32_t Saves::Read(int32_t port, int32_t slot, uint32_t save, uint32_t file, uint32_t buffer, int32_t size) {
    Storage::Request request;
    request.operation = Storage::Operation::Read;
    request.port = unsigned(port); request.slot = unsigned(slot);
    bool valid = size >= 0 && String(save, 31, request.save) && String(file, 31, request.file);
    request.size = static_cast<uint32_t>(size < 0 ? 0 : size);
    valid = valid && request.size <= Storage::kMaxBytes && (!request.size || GuestPointer(buffer, request.size));
    const uint32_t read_size = request.size;
    const int32_t ok = SubmitRequest(*core_, std::move(request), clock_.BoundaryCount(), valid);
    if (ok) {
        outputs_ = {};
        outputs_.data = buffer;
        outputs_.size = read_size;
    }
    return ok;
}

int32_t Saves::FileKilobytes(int32_t size) {
    return static_cast<int32_t>(Storage::FileKilobytes(static_cast<uint64_t>(static_cast<uint32_t>(size))));
}

int32_t Saves::SaveBytes(int32_t kilobytes, int32_t files) {
    return static_cast<int32_t>(Storage::SaveBytes(static_cast<uint32_t>(kilobytes), static_cast<uint32_t>(files)));
}

}

extern "C" {

ps2e_s32 ps2e_saves_initialise(ps2e_u32 region, ps2e_u32 product) { return te::Services::Get().saves().Initialise(region, product); }
ps2e_s32 ps2e_saves_update(ps2e_s32 port, ps2e_s32 slot, ps2e_u32 info) { return te::Services::Get().saves().Update(port, slot, info); }
void ps2e_saves_get_result(ps2e_u32 out, ps2e_s32 operation) { te::Services::Get().saves().GetResult(out, operation); }
ps2e_s32 ps2e_saves_format(ps2e_s32 port, ps2e_s32 slot) { return te::Services::Get().saves().Format(port, slot); }
ps2e_s32 ps2e_saves_create_save(ps2e_s32 port, ps2e_s32 slot, ps2e_u32 save) { return te::Services::Get().saves().CreateSave(port, slot, save); }
ps2e_s32 ps2e_saves_measure_save(ps2e_s32 port, ps2e_s32 slot, ps2e_u32 save, ps2e_u32 kilobytes) { return te::Services::Get().saves().MeasureSave(port, slot, save, kilobytes); }
ps2e_s32 ps2e_saves_find_file(ps2e_s32 port, ps2e_s32 slot, ps2e_u32 file, ps2e_u32 save, ps2e_u32 size, ps2e_u32 entry) { return te::Services::Get().saves().FindFile(port, slot, file, save, size, entry); }
ps2e_s32 ps2e_saves_write(ps2e_s32 port, ps2e_s32 slot, ps2e_u32 save, ps2e_u32 file, ps2e_u32 data, ps2e_s32 size) { return te::Services::Get().saves().Write(port, slot, save, file, data, size); }
ps2e_s32 ps2e_saves_read(ps2e_s32 port, ps2e_s32 slot, ps2e_u32 save, ps2e_u32 file, ps2e_u32 buffer, ps2e_s32 size) { return te::Services::Get().saves().Read(port, slot, save, file, buffer, size); }
ps2e_s32 ps2e_saves_file_kilobytes(ps2e_s32 size) { return te::Saves::FileKilobytes(size); }
ps2e_s32 ps2e_saves_save_bytes(ps2e_s32 kilobytes, ps2e_s32 files) { return te::Saves::SaveBytes(kilobytes, files); }

}
