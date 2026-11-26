// Descriptor-only FFmpeg input and bounded frame conversion shared by isolated helpers.
#pragma once
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#include "backend.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/cpu.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace rummage::isolate::backend {
namespace {
constexpr std::int64_t kMaxPixels = 32'000'000;
constexpr std::uint64_t kReadBudget = 128U * 1024U * 1024U;
using Clock = std::chrono::steady_clock;

struct Source {
    int fd;
    std::int64_t size;
    std::int64_t position{0};
    std::uint64_t read{0};
    Clock::time_point deadline{Clock::now() + std::chrono::seconds{7}};
};

int read_bytes(void* opaque, std::uint8_t* buffer, int count) {
    if (count <= 0) {
        return AVERROR(EINVAL);
    }
    auto& source = *static_cast<Source*>(opaque);
    if (Clock::now() >= source.deadline || source.read >= kReadBudget) {
        return AVERROR_EXIT;
    }
    const auto wanted =
        std::min<std::uint64_t>(static_cast<unsigned>(count), kReadBudget - source.read);
    ssize_t got;
    do {
        got = ::pread(source.fd, buffer, wanted, source.position);
    } while (got < 0 && errno == EINTR);
    if (got <= 0) {
        return got == 0 ? AVERROR_EOF : AVERROR(errno);
    }
    source.position += got;
    source.read += static_cast<std::uint64_t>(got);
    return static_cast<int>(got);
}

std::int64_t seek(void* opaque, std::int64_t offset, int whence) {
    auto& source = *static_cast<Source*>(opaque);
    if (whence == AVSEEK_SIZE) {
        return source.size;
    }
    whence &= ~AVSEEK_FORCE;
    const auto base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? source.position : source.size;
    if ((whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) || offset < -base ||
        offset > source.size - base) {
        return AVERROR(EINVAL);
    }
    source.position = base + offset;
    return source.position;
}

int interrupted(void* opaque) { return Clock::now() >= static_cast<Source*>(opaque)->deadline; }

int deny_open(AVFormatContext*, AVIOContext**, const char*, int, AVDictionary**) {
    return AVERROR(EACCES);
}

struct Input {
    AVFormatContext* format{avformat_alloc_context()};
    AVIOContext* io{nullptr};

    ~Input() {
        avformat_close_input(&format);
        if (io != nullptr) {
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
    }
};

bool open_input(Input& input, Source& source) {
    auto* buffer = static_cast<unsigned char*>(av_malloc(32768));
    if (input.format == nullptr || buffer == nullptr) {
        av_free(buffer);
        return false;
    }
    input.io = avio_alloc_context(buffer, 32768, 0, &source, read_bytes, nullptr, seek);
    if (input.io == nullptr) {
        av_free(buffer);
        return false;
    }
    input.format->pb = input.io;
    input.format->flags |= AVFMT_FLAG_CUSTOM_IO;
    input.format->io_open = deny_open;  // Containers must never open another path or URL.
    input.format->interrupt_callback = {.callback = interrupted, .opaque = &source};
    input.format->probesize = 1024 * 1024;
    input.format->max_analyze_duration = AV_TIME_BASE;
    AVDictionary* options = nullptr;
    av_dict_set(&options, "format_whitelist",
                "mov,matroska,webm,avi,mpeg,mpegts,ogg,flv,asf,mjpeg,yuv4mpegpipe,mp3,flac,wav,aac",
                0);
    const int opened = avformat_open_input(&input.format, nullptr, nullptr, &options);
    av_dict_free(&options);
    if (opened < 0) {
        return false;
    }
    // Keep stream discovery single-threaded too; no decoder may create threads inside the cage.
    std::vector<AVDictionary*> stream_options(input.format->nb_streams, nullptr);
    for (auto& option : stream_options) {
        av_dict_set(&option, "threads", "1", 0);
        av_dict_set(&option, "max_pixels", "32000000", 0);
    }
    const int probed = avformat_find_stream_info(input.format, stream_options.data());
    for (auto*& option : stream_options) {
        av_dict_free(&option);
    }
    if (probed < 0) {
        return false;
    }
    return true;
}

struct Decoder {
    AVCodecContext* context{nullptr};
    AVPacket* packet{av_packet_alloc()};
    AVFrame* frame{av_frame_alloc()};

    ~Decoder() {
        avcodec_free_context(&context);
        av_packet_free(&packet);
        av_frame_free(&frame);
    }
};

// The frame as RGBA, upright, fitted to width x height; whole gets the video's own size, upright
// and with its pixel aspect (what the frame would be unscaled).
picture::Image convert(const AVFrame& frame, AVStream& stream, std::uint32_t width,
                       std::uint32_t height, std::pair<std::uint32_t, std::uint32_t>& whole) {
    if (frame.width <= 0 || frame.height <= 0 ||
        static_cast<std::int64_t>(frame.width) * frame.height > kMaxPixels) {
        return {};
    }
    std::uint8_t orientation = 1;
    const AVPacketSideData* matrix =
        av_packet_side_data_get(stream.codecpar->coded_side_data,
                                stream.codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
    if (matrix != nullptr && matrix->size >= 9 * sizeof(std::int32_t)) {
        const double angle =
            av_display_rotation_get(reinterpret_cast<const std::int32_t*>(matrix->data));
        if (std::isfinite(angle)) {
            const int turn = (static_cast<int>(std::lround(-angle / 90.0)) % 4 + 4) % 4;
            constexpr std::array<std::uint8_t, 4> orientations{1, 6, 3, 8};
            orientation = orientations[static_cast<std::size_t>(turn)];
        }
    }
    if (orientation == 6 || orientation == 8) {
        std::swap(width, height);
    }
    AVRational sar = frame.sample_aspect_ratio;
    if (sar.num <= 0 || sar.den <= 0) {
        sar = stream.sample_aspect_ratio;
    }
    double aspect = sar.num > 0 && sar.den > 0 ? av_q2d(sar) : 1.0;
    aspect = std::clamp(aspect, 1.0 / 100.0, 100.0);
    const double display_width = frame.width * aspect;
    const double fit =
        std::min({1.0, width / display_width, static_cast<double>(height) / frame.height});
    whole = {static_cast<std::uint32_t>(std::max(1.0, std::floor(display_width))),
             static_cast<std::uint32_t>(frame.height)};
    if (orientation == 6 || orientation == 8) {
        std::swap(whole.first, whole.second);
    }
    const int w = std::max(1, static_cast<int>(std::floor(display_width * fit)));
    const int h = std::max(1, static_cast<int>(std::floor(frame.height * fit)));
    SwsContext* scale =
        sws_getContext(frame.width, frame.height, static_cast<AVPixelFormat>(frame.format), w, h,
                       AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (scale == nullptr) {
        return {};
    }
    picture::Image image{.width = static_cast<std::uint32_t>(w),
                         .height = static_cast<std::uint32_t>(h),
                         .rgba = std::vector<std::uint8_t>(static_cast<std::size_t>(w) *
                                                           static_cast<std::size_t>(h) * 4)};
    std::array<std::uint8_t*, 4> output{image.rgba.data(), nullptr, nullptr, nullptr};
    std::array<int, 4> strides{w * 4, 0, 0, 0};
    const int lines = sws_scale(scale, frame.data, frame.linesize, 0, frame.height, output.data(),
                                strides.data());
    sws_freeContext(scale);
    return lines == h ? picture::apply_orientation(std::move(image), orientation)
                      : picture::Image{};
}
}  // namespace

}  // namespace rummage::isolate::backend
