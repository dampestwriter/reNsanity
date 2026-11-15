
#include "engine/cdvd/iso_fs.h"
#include "engine/cdvd/iso_read_cache.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <iterator>
#include <new>
#include <vector>

#include "engine/platform/host/platform.h"

namespace {

size_t PreadAll(int fd, uint8_t* dst, size_t n, uint64_t offset) {
    size_t done = 0;
    while (done < n) {
        const ps2r::host::SSize got = ps2r::host::FdPread(fd, dst + done, n - done, offset + done);
        if (got <= 0) break;
        done += static_cast<size_t>(got);
    }
    return done;
}

constexpr uint32_t kPvdSector = 16;
constexpr uint32_t kRawSectorSize = 2352;
constexpr size_t kRootRecordOffset = 156;
constexpr size_t kVolumeIdOffset = 40;
constexpr size_t kVolumeSpaceOffset = 80;

uint32_t Le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

void DecodeRecord(const uint8_t* p, IsoFs::Entry& e) {
    e.lba = Le32(p + 2);
    e.size = Le32(p + 10);
    std::memcpy(e.date, p + 18, 7);
    e.directory = (p[25] & 0x02) != 0;
    const size_t id_len = p[32];
    e.name.assign(reinterpret_cast<const char*>(p + 33), id_len);
}

std::string StripPrefix(const std::string& path) {
    std::string s = path;
    const size_t colon = s.find(':');
    if (colon != std::string::npos && s.compare(0, 5, "cdrom") == 0) s = s.substr(colon + 1);
    return s;
}
}

IsoFs::IsoFs() = default;
IsoFs::~IsoFs() { Close(); }

void IsoFs::Close() {
    cache_.reset();
    if (fd_ >= 0) ps2r::host::FdClose(fd_);
    fd_ = -1;
    image_bytes_ = 0;
    file_bytes_ = 0;
    sector_stride_ = kSectorSize;
    user_offset_ = 0;
    volume_sectors_ = 0;
    root_ = Entry{};
    volume_id_.clear();
}

bool IsoFs::Open(const std::string& path, std::string* error) {
    Close();
    fd_ = ps2r::host::FdOpen(path.c_str(), ps2r::host::kOpenRead);
    if (fd_ < 0) {
        if (error) *error = "cannot open " + path;
        return false;
    }
    const int64_t end = ps2r::host::FdSize(fd_);
    file_bytes_ = end > 0 ? static_cast<uint64_t>(end) : 0;
    image_bytes_ = file_bytes_;

    {
        static const uint8_t kSync[12] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
        uint8_t head[16];
        if (file_bytes_ >= kRawSectorSize && PreadAll(fd_, head, 16, 0) == 16 && std::memcmp(head, kSync, 12) == 0) {
            sector_stride_ = kRawSectorSize;
            user_offset_ = head[15] == 2 ? 24 : 16;
            image_bytes_ = (file_bytes_ / kRawSectorSize) * kSectorSize;
        }
    }
    uint8_t pvd[kSectorSize];
    if (ReadSectors(kPvdSector, 1, pvd) != 1 || pvd[0] != 1 || std::memcmp(pvd + 1, "CD001", 5) != 0) {
        if (error) *error = path + ": no ISO9660 primary volume descriptor at sector 16";
        Close();
        return false;
    }
    volume_sectors_ = Le32(pvd + kVolumeSpaceOffset);
    volume_id_.assign(reinterpret_cast<const char*>(pvd + kVolumeIdOffset), 32);
    while (!volume_id_.empty() && volume_id_.back() == ' ') volume_id_.pop_back();
    const uint8_t* root = pvd + kRootRecordOffset;
    if (root[0] < 33) {
        if (error) *error = path + ": malformed root directory record";
        Close();
        return false;
    }
    DecodeRecord(root, root_);
    root_.name = "/";
    cache_.reset(new IsoReadCache(image_bytes_, [this](uint64_t at, size_t n, uint8_t* dst) {
        return ReadUncached(at, n, dst);
    }));
    return true;
}

void IsoFs::Prefetch(uint64_t offset, size_t size) const {
    if (cache_) cache_->Prefetch(offset, size);
}
size_t IsoFs::ReadDeferredBytes(uint64_t offset, size_t size, uint8_t* dst) const {
    if (!cache_) return ReadBytes(offset, size, dst);
    if (offset >= image_bytes_) return 0;
    return cache_->Read(offset, size_t(std::min<uint64_t>(size, image_bytes_ - offset)), dst);
}

size_t IsoFs::ReadBytes(uint64_t offset, size_t size, uint8_t* dst) const {
    if (fd_ < 0 || size == 0 || offset >= image_bytes_) return 0;
    const size_t n = static_cast<size_t>(std::min<uint64_t>(size, image_bytes_ - offset));
    const bool cached = cache_ != nullptr;
    const size_t got = cached ? cache_->Read(offset, n, dst) : ReadUncached(offset, n, dst);
    return got;
}

size_t IsoFs::ReadUncached(uint64_t offset, size_t size, uint8_t* dst) const {
    if (fd_ < 0 || size == 0) return 0;
    if (offset >= image_bytes_) return 0;
    const uint64_t avail = image_bytes_ - offset;
    const size_t n = avail < size ? static_cast<size_t>(avail) : size;
    if (!RawSectors()) {
        return PreadAll(fd_, dst, n, offset);
    }

    size_t done = 0;
    while (done < n) {
        const uint64_t logical = offset + done;
        const uint64_t lba = logical / kSectorSize;
        const size_t in_sector = static_cast<size_t>(logical % kSectorSize);
        const size_t chunk = std::min(n - done, static_cast<size_t>(kSectorSize) - in_sector);
        const uint64_t raw = lba * sector_stride_ + user_offset_ + in_sector;
        const size_t got = PreadAll(fd_, dst + done, chunk, raw);
        done += got;
        if (got != chunk) break;
    }
    return done;
}

uint32_t IsoFs::ReadSectors(uint32_t lba, uint32_t count, uint8_t* dst) const {

    const size_t bytes = ReadBytes(static_cast<uint64_t>(lba) * kSectorSize, static_cast<size_t>(count) * kSectorSize, dst);
    return static_cast<uint32_t>(bytes / kSectorSize);
}

bool IsoFs::IdentifierMatches(const std::string& recorded, const std::string& wanted) {

    std::string rec = recorded;
    const size_t semi = rec.find(';');
    std::string rec_base = semi == std::string::npos ? rec : rec.substr(0, semi);
    auto ieq = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++)
            if (std::toupper(static_cast<unsigned char>(a[i])) != std::toupper(static_cast<unsigned char>(b[i]))) return false;
        return true;
    };
    return ieq(rec, wanted) || ieq(rec_base, wanted);
}

bool IsoFs::FindInDirectory(const Entry& dir, const std::string& component, Entry& out) const {
    if (fd_ < 0 || dir.size == 0) return false;
    const uint32_t sectors = (dir.size + kSectorSize - 1) / kSectorSize;
    std::vector<uint8_t> buf(static_cast<size_t>(sectors) * kSectorSize);
    if (ReadSectors(dir.lba, sectors, buf.data()) != sectors) return false;
    size_t pos = 0;
    const size_t end = dir.size;
    while (pos < end) {
        const uint8_t len = buf[pos];
        if (len == 0) {
            pos = (pos / kSectorSize + 1) * kSectorSize;
            continue;
        }
        if (len < 33 || pos + len > buf.size()) return false;
        const uint8_t* p = buf.data() + pos;
        const size_t id_len = p[32];

        if (!(id_len == 1 && (p[33] == 0 || p[33] == 1))) {
            const std::string id(reinterpret_cast<const char*>(p + 33), id_len);
            if (IdentifierMatches(id, component)) {
                DecodeRecord(p, out);
                return true;
            }
        }
        pos += len;
    }
    return false;
}

bool IsoFs::Find(const std::string& path, Entry& out) const {
    if (fd_ < 0) return false;
    const std::string s = StripPrefix(path);
    Entry cur = root_;
    size_t i = 0;
    bool any = false;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == '/' || s[i] == '\\')) i++;
        if (i >= s.size()) break;
        size_t j = i;
        while (j < s.size() && s[j] != '/' && s[j] != '\\') j++;
        const std::string component = s.substr(i, j - i);
        if (!cur.directory) return false;
        Entry next;
        if (!FindInDirectory(cur, component, next)) return false;
        cur = next;
        any = true;
        i = j;
    }
    if (!any) return false;
    out = cur;
    return true;
}
