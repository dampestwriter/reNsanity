#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

#include "engine/ipu/hle/decoded_picture.h"
#include "engine/ipu/hle/video_decoder.h"

struct AVCodecParserContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace ps2r::ipu {

class FfmpegVideoDecoder : public VideoDecoder {
public:
    FfmpegVideoDecoder() = default;
    ~FfmpegVideoDecoder() override;

    FfmpegVideoDecoder(const FfmpegVideoDecoder&) = delete;
    FfmpegVideoDecoder& operator=(const FfmpegVideoDecoder&) = delete;

    void Feed(const uint8_t* data, size_t size, std::deque<DecodedPicture>& out) override;
    void Flush(std::deque<DecodedPicture>& out) override;
    double AuthoredFps() const override { return authored_fps_; }
    void Shutdown() override;

    void SetPlanarOutput(bool on) { planar_ = on; }



private:
    bool EnsureInitialized();
    bool SendPacket(const uint8_t* data, int size, std::deque<DecodedPicture>& out);
    bool ReceiveFrames(std::deque<DecodedPicture>& out);
    bool ConvertFrame(std::deque<DecodedPicture>& out);
    bool Deliver(DecodedPicture&& pic, std::deque<DecodedPicture>& out);

    AVCodecParserContext* parser_ = nullptr;
    AVCodecContext* codec_ctx_ = nullptr;
    double authored_fps_ = 0.0;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    SwsContext* sws_ctx_ = nullptr;
    int sws_w_ = 0;
    int sws_h_ = 0;
    int sws_fmt_ = -1;
    bool initialized_ = false;
    bool drained_ = false;
    bool planar_ = false;
};

}
