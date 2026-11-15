#pragma once

#include <cstddef>
#include <cstdint>

namespace ps2r {
namespace gs {

class VramMirror {
public:
    virtual ~VramMirror() = default;
    virtual void OnWriteStream(uint32_t byte_off, const uint8_t* src, size_t len) = 0;
    virtual void OnNoteWriteNoBytes(uint32_t byte_off, size_t len) = 0;
    virtual void OnNoteStreamWrite(uint32_t start, uint32_t size) = 0;
    virtual void OnSettleWrites() = 0;
    virtual void OnNoteCpuWrite(uint32_t start, uint32_t size) = 0;
};

}
}
