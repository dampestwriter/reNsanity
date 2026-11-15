#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>

#include "engine/ipu/hle/decoded_picture.h"

namespace ps2r::ipu {

class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;

    virtual void Feed(const uint8_t* data, size_t size, std::deque<DecodedPicture>& out) = 0;

    virtual void Flush(std::deque<DecodedPicture>& out) = 0;

    virtual double AuthoredFps() const = 0;

    virtual void Shutdown() = 0;
};

}
