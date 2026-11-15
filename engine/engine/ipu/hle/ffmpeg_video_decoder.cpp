#include "engine/ipu/hle/ffmpeg_video_decoder.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace ps2r::ipu {

FfmpegVideoDecoder::~FfmpegVideoDecoder() { Shutdown(); }

void FfmpegVideoDecoder::Feed(const uint8_t* data, size_t size, std::deque<DecodedPicture>& out) {
    if (!data || size == 0) return;
    if (!EnsureInitialized()) return;

    if (size > std::numeric_limits<size_t>::max() - AV_INPUT_BUFFER_PADDING_SIZE) return;
    std::vector<uint8_t> input(size + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    std::memcpy(input.data(), data, size);
    const uint8_t* cursor = input.data();
    size_t remaining = size;
    while (remaining > 0) {
        uint8_t* pkt_data = nullptr;
        int pkt_size = 0;
        const int chunk = static_cast<int>(
            std::min<size_t>(remaining, static_cast<size_t>(std::numeric_limits<int>::max())));
        const int used = av_parser_parse2(parser_, codec_ctx_, &pkt_data, &pkt_size, cursor, chunk,
                                          AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (used < 0) {
            return;
        }
        if (used == 0 && pkt_size == 0) break;

        cursor += used;
        remaining -= static_cast<size_t>(used);

        if (pkt_size > 0 && !SendPacket(pkt_data, pkt_size, out)) return;
    }
}

void FfmpegVideoDecoder::Flush(std::deque<DecodedPicture>& out) {
    if (!initialized_ || drained_) return;

    if (parser_) {
        uint8_t* pkt_data = nullptr;
        int pkt_size = 0;
        (void)av_parser_parse2(parser_, codec_ctx_, &pkt_data, &pkt_size, nullptr, 0, AV_NOPTS_VALUE,
                               AV_NOPTS_VALUE, 0);
        if (pkt_size > 0 && !SendPacket(pkt_data, pkt_size, out)) return;
    }

    const int ret = avcodec_send_packet(codec_ctx_, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) {
        return;
    }

    (void)ReceiveFrames(out);
    drained_ = true;
}

void FfmpegVideoDecoder::Shutdown() {
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }
    if (frame_) av_frame_free(&frame_);
    if (packet_) av_packet_free(&packet_);
    if (codec_ctx_) avcodec_free_context(&codec_ctx_);
    if (parser_) {
        av_parser_close(parser_);
        parser_ = nullptr;
    }
    sws_w_ = 0;
    sws_h_ = 0;
    sws_fmt_ = AV_PIX_FMT_NONE;
    authored_fps_ = 0.0;
    initialized_ = false;
    drained_ = false;
}

bool FfmpegVideoDecoder::EnsureInitialized() {
    if (initialized_) return true;

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
    if (!codec) {
        return false;
    }

    av_log_set_level(AV_LOG_QUIET);
    parser_ = av_parser_init(AV_CODEC_ID_MPEG2VIDEO);
    if (!parser_) {
        return false;
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    if (!codec_ctx_ || !frame_ || !packet_) {
        Shutdown();
        return false;
    }

    codec_ctx_->thread_count = 1;
    const int ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0) {
        Shutdown();
        return false;
    }

    initialized_ = true;
    drained_ = false;
    return true;
}

bool FfmpegVideoDecoder::SendPacket(const uint8_t* data, int size, std::deque<DecodedPicture>& out) {
    if (!data || size <= 0) return true;

    av_packet_unref(packet_);
    const int alloc = av_new_packet(packet_, size);
    if (alloc < 0) {
        return false;
    }
    std::memcpy(packet_->data, data, static_cast<size_t>(size));

    int ret = avcodec_send_packet(codec_ctx_, packet_);
    if (ret == AVERROR(EAGAIN)) {

        if (!ReceiveFrames(out)) {
            av_packet_unref(packet_);
            return false;
        }
        ret = avcodec_send_packet(codec_ctx_, packet_);
    }
    av_packet_unref(packet_);
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        return false;
    }
    return ReceiveFrames(out);
}

bool FfmpegVideoDecoder::ReceiveFrames(std::deque<DecodedPicture>& out) {
    for (;;) {
        const int ret = avcodec_receive_frame(codec_ctx_, frame_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return true;
        if (ret < 0) {
            return false;
        }
        const bool ok = ConvertFrame(out);
        av_frame_unref(frame_);
        if (!ok) return false;
    }
}

bool FfmpegVideoDecoder::ConvertFrame(std::deque<DecodedPicture>& out) {
    const int w = frame_->width;
    const int h = frame_->height;
    const int fmt = frame_->format;
    if (w <= 0 || h <= 0 || fmt == AV_PIX_FMT_NONE) return false;

    if (planar_ && fmt == AV_PIX_FMT_YUV420P && !(w & 1) && !(h & 1)) {

        DecodedPicture pic;
        pic.width = w;
        pic.height = h;
        const int cw = w / 2, ch = h / 2;
        pic.yuv.resize(static_cast<size_t>(w) * static_cast<size_t>(h) + 2u * static_cast<size_t>(cw) * static_cast<size_t>(ch));
        uint8_t* out_plane = pic.yuv.data();
        for (int plane = 0; plane < 3; ++plane) {
            const int pw = plane ? cw : w, ph = plane ? ch : h;
            const uint8_t* in = frame_->data[plane];
            for (int y = 0; y < ph; ++y, out_plane += pw)
                std::memcpy(out_plane, in + static_cast<ptrdiff_t>(y) * frame_->linesize[plane], static_cast<size_t>(pw));
        }
        return Deliver(std::move(pic), out);
    }

    if (!sws_ctx_ || sws_w_ != w || sws_h_ != h || sws_fmt_ != fmt) {
        if (sws_ctx_) sws_freeContext(sws_ctx_);
        sws_ctx_ = sws_getContext(w, h, static_cast<AVPixelFormat>(fmt), w, h, AV_PIX_FMT_RGBA,
                                  SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_ctx_) {
            return false;
        }
        sws_w_ = w;
        sws_h_ = h;
        sws_fmt_ = fmt;
    }

    DecodedPicture pic;
    pic.width = w;
    pic.height = h;
    pic.rgba.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);

    uint8_t* dst[4] = {pic.rgba.data(), nullptr, nullptr, nullptr};
    int dst_ls[4] = {w * 4, 0, 0, 0};
    const int rows = sws_scale(sws_ctx_, frame_->data, frame_->linesize, 0, h, dst, dst_ls);
    if (rows <= 0) {
        return false;
    }

    return Deliver(std::move(pic), out);
}

bool FfmpegVideoDecoder::Deliver(DecodedPicture&& pic, std::deque<DecodedPicture>& out) {
    out.push_back(std::move(pic));

    if (authored_fps_ == 0.0 && codec_ctx_) {
        const AVRational fr = codec_ctx_->framerate;
        if (fr.num > 0 && fr.den > 0) {
            authored_fps_ = static_cast<double>(fr.num) / static_cast<double>(fr.den);
        }
    }
    return true;
}

}
