#include "engine/ipu/hle/pss_demux.h"

#include <algorithm>

namespace ps2r::ipu {

namespace {

uint16_t ReadBe16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

}

PssDemux::PssDemux(PssDemuxListener& listener) : listener_(listener) {}

void PssDemux::Append(const uint8_t* data, size_t size) {
    if (!data || size == 0) return;
    buffer_.insert(buffer_.end(), data, data + size);
}

size_t PssDemux::FindStartCode(const std::vector<uint8_t>& buf, size_t from) {
    if (buf.size() < 4 || from + 3 >= buf.size()) return kStartCodeNotFound;
    for (size_t i = from; i + 3 < buf.size(); ++i) {
        if (buf[i] == 0x00 && buf[i + 1] == 0x00 && buf[i + 2] == 0x01) return i;
    }
    return kStartCodeNotFound;
}

bool PssDemux::ContainsSequenceEnd(const uint8_t* data, size_t size) {
    if (!data || size < 4) return false;
    for (size_t i = 0; i + 3 < size; ++i) {
        if (data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01 &&
            data[i + 3] == kSequenceEnd) {
            return true;
        }
    }
    return false;
}

bool PssDemux::IsVideoStreamId(uint8_t id) { return id >= 0xE0 && id <= 0xEF; }
bool PssDemux::IsAudioStreamId(uint8_t id) { return id == 0xBD || (id >= 0xC0 && id <= 0xDF); }

bool PssDemux::IsLengthPrefixedHeader(uint8_t id) {
    switch (id) {
    case kSystemHeader:
    case 0xBC: case 0xBE: case 0xBF:
    case 0xF0: case 0xF1: case 0xF2: case 0xF8: case 0xFF:
        return true;
    default:
        return false;
    }
}

size_t PssDemux::ParsePesPayloadOffset(const uint8_t* packet, size_t packet_size) {
    if (!packet || packet_size <= 6) return packet_size;

    size_t pos = 6;

    if (packet_size >= 9 && (packet[pos] & 0xC0) == 0x80) {
        return std::min(packet_size, 9 + static_cast<size_t>(packet[pos + 2]));
    }

    while (pos < packet_size && packet[pos] == 0xFF) ++pos;

    if (pos + 1 < packet_size && (packet[pos] & 0xC0) == 0x40) pos += 2;
    if (pos >= packet_size) return packet_size;

    const uint8_t flags = packet[pos];
    if      ((flags & 0xF0) == 0x20) pos += 5;
    else if ((flags & 0xF0) == 0x30) pos += 10;
    else if (flags == 0x0F)          pos += 1;

    return std::min(packet_size, pos);
}

void PssDemux::ErasePrefix(size_t n) {
    const size_t clamped = std::min(n, buffer_.size());
    if (clamped == 0) return;
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(clamped));
}

void PssDemux::Process(bool final_chunk) {
    auto& buf = buffer_;
    for (;;) {
        if (ended_) {

            ErasePrefix(buf.size());
            return;
        }

        const size_t start = FindStartCode(buf, 0);
        if (start == kStartCodeNotFound) {
            if (final_chunk)         ErasePrefix(buf.size());
            else if (buf.size() > 3) ErasePrefix(buf.size() - 3);
            return;
        }
        if (start > 0) ErasePrefix(start);
        if (buf.size() < 4) return;

        const uint8_t stream_id = buf[3];

        if (stream_id == kProgramEnd) {
            ended_ = true;
            listener_.OnProgramEnd();
            ErasePrefix(4);
            ErasePrefix(buf.size());
            return;
        }

        if (stream_id == kPackHeader) {
            if (buf.size() < 12) {
                if (final_chunk) ErasePrefix(buf.size());
                return;
            }
            size_t pack_size = 12;
            if ((buf[4] & 0xC0) == 0x40) {
                if (buf.size() < 14) {
                    if (final_chunk) ErasePrefix(buf.size());
                    return;
                }
                pack_size = 14 + static_cast<size_t>(buf[13] & 0x07);
            }
            if (buf.size() < pack_size) {
                if (final_chunk) ErasePrefix(buf.size());
                return;
            }
            ErasePrefix(pack_size);
            continue;
        }

        if (buf.size() < 6) {
            if (final_chunk) ErasePrefix(buf.size());
            return;
        }
        const uint16_t pkt_len = ReadBe16(buf.data() + 4);

        if (IsLengthPrefixedHeader(stream_id)) {
            const size_t end = 6 + static_cast<size_t>(pkt_len);
            if (buf.size() < end) {
                if (final_chunk) ErasePrefix(buf.size());
                return;
            }
            ErasePrefix(end);
            continue;
        }

        size_t pkt_end = 0;
        if (pkt_len != 0) {
            pkt_end = 6 + static_cast<size_t>(pkt_len);
            if (buf.size() < pkt_end) {
                if (!final_chunk) return;
                pkt_end = buf.size();
            }
        } else {
            const size_t next = FindStartCode(buf, 6);
            if (next == kStartCodeNotFound) {
                if (!final_chunk) return;
                pkt_end = buf.size();
            } else {
                pkt_end = next;
            }
        }

        if (IsVideoStreamId(stream_id)) {
            const size_t payload = ParsePesPayloadOffset(buf.data(), pkt_end);
            if (payload < pkt_end) {
                const bool has_end = ContainsSequenceEnd(buf.data() + payload, pkt_end - payload);
                if (has_end) ended_ = true;
                listener_.OnVideoEs(buf.data() + payload, pkt_end - payload, has_end);
            }
        } else if (IsAudioStreamId(stream_id)) {
            const size_t payload = ParsePesPayloadOffset(buf.data(), pkt_end);
            if (payload < pkt_end) {
                listener_.OnAudioPes(buf.data() + payload, pkt_end - payload);
            }
        }

        ErasePrefix(pkt_end);
    }
}

}
