#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "engine/ipu/hle/pss_demux_listener.h"

namespace ps2r::ipu {

class PssDemux {
public:
    static constexpr size_t kStartCodeNotFound = static_cast<size_t>(-1);
    static constexpr uint8_t kPackHeader = 0xBA;
    static constexpr uint8_t kSystemHeader = 0xBB;
    static constexpr uint8_t kProgramEnd = 0xB9;
    static constexpr uint8_t kSequenceEnd = 0xB7;

    explicit PssDemux(PssDemuxListener& listener);

    void Append(const uint8_t* data, size_t size);

    void Process(bool final_chunk);

    bool Ended() const { return ended_; }

    static size_t FindStartCode(const std::vector<uint8_t>& buf, size_t from);
    static bool ContainsSequenceEnd(const uint8_t* data, size_t size);
    static bool IsVideoStreamId(uint8_t id);
    static bool IsAudioStreamId(uint8_t id);
    static bool IsLengthPrefixedHeader(uint8_t id);

    static size_t ParsePesPayloadOffset(const uint8_t* packet, size_t packet_size);

private:

    void ErasePrefix(size_t n);

    PssDemuxListener& listener_;
    std::vector<uint8_t> buffer_;
    bool ended_ = false;
};

}
