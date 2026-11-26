// Reuse the still helper's descriptor-only demuxing, codec limits, orientation and scaling.
// This executable has its own persistent session; no playback state enters the still pool.
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <sys/mman.h>

#include "decoder_common.hpp"
#include "playback_protocol.hpp"
#include "protocol.hpp"
#include "sandbox.hpp"
extern "C" {
#include <libswresample/swresample.h>
}

namespace wire = rummage::isolate::playback_wire;
namespace protocol = rummage::isolate::protocol;

namespace {
using namespace rummage::isolate::backend;

struct PacketDeleter {
    void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};

using SavedPacket = std::unique_ptr<AVPacket, PacketDeleter>;
constexpr std::size_t kMaxPendingBytes = 32 * 1024 * 1024;

std::size_t packet_bytes(const AVPacket& value) {
    if (value.size < 0 || value.side_data_elems < 0 || value.side_data_elems > 64)
        return kMaxPendingBytes + 1;
    auto bytes = static_cast<std::size_t>(value.size);
    for (int i = 0; i < value.side_data_elems; ++i) {
        if (bytes > kMaxPendingBytes || value.side_data[i].size > kMaxPendingBytes - bytes)
            return kMaxPendingBytes + 1;
        bytes += value.side_data[i].size;
    }
    return bytes;
}

struct Track {
    AVStream* stream{nullptr};
    Decoder decoder;
    double end_ms{-1};
    std::deque<SavedPacket> pending;
    bool flushed{false};

    bool open(AVStream* value) {
        stream = value;
        const AVCodec* codec = avcodec_find_decoder(value->codecpar->codec_id);
        if (!codec)
            return false;
        decoder.context = avcodec_alloc_context3(codec);
        if (!decoder.context || !decoder.frame || !decoder.packet ||
            avcodec_parameters_to_context(decoder.context, value->codecpar) < 0)
            return false;
        decoder.context->thread_count = 1;
        decoder.context->thread_type = 0;
        decoder.context->max_pixels = kMaxPixels;
        return avcodec_open2(decoder.context, codec, nullptr) >= 0;
    }
};

struct Session {
    Source source;
    Input input;
    Track video, audio;
    SwrContext* resampler{nullptr};
    int audio_rate{0}, audio_format{-1};
    AVChannelLayout audio_layout{};

    struct VideoFrame {
        double time;
        rummage::picture::Image image;
    };

    std::deque<VideoFrame> frames;
    std::size_t queued_bytes{0};
    rummage::picture::Image image;
    std::vector<std::int16_t> pcm;

    struct AudioFrame {
        std::int64_t start;
        std::vector<std::int16_t> samples;
    };

    std::deque<AudioFrame> audio_frames;
    std::size_t queued_samples{0}, pending_bytes{0}, pending_packets{0};
    std::int64_t audio_sent{0};
    bool input_eof{false};
    bool eof{false};
    double discard_before{0};
    std::int64_t duration{0};

    explicit Session(int fd, std::int64_t size) : source{.fd = fd, .size = size} {}

    ~Session() {
        swr_free(&resampler);
        av_channel_layout_uninit(&audio_layout);
    }

    bool open() {
        if (!open_input(input, source))
            return false;
        for (unsigned i = 0; i < input.format->nb_streams; ++i) {
            auto* stream = input.format->streams[i];
            if (stream->time_base.num <= 0 || stream->time_base.den <= 0)
                continue;
            if (!video.stream && stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
                !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
                if (!video.open(stream))
                    return false;
            }
            if (!audio.stream && stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                if (!audio.open(stream))
                    return false;
                auto* context = audio.decoder.context;
                if (context->sample_rate <= 0 || context->sample_rate > 384000 ||
                    context->ch_layout.nb_channels <= 0 || context->ch_layout.nb_channels > 32)
                    return false;
                audio_rate = context->sample_rate;
                audio_format = context->sample_fmt;
                if (av_channel_layout_copy(&audio_layout, &context->ch_layout) < 0)
                    return false;
                AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
                if (swr_alloc_set_opts2(&resampler, &stereo, AV_SAMPLE_FMT_S16, 48000,
                                        &context->ch_layout, context->sample_fmt,
                                        context->sample_rate, 0, nullptr) < 0 ||
                    swr_init(resampler) < 0)
                    return false;
            }
        }
        duration = input.format->duration > 0 ? input.format->duration / 1000 : 0;
        return video.stream || audio.stream;
    }

    double origin() const {
        return input.format->start_time == AV_NOPTS_VALUE
                   ? 0
                   : static_cast<double>(input.format->start_time) / 1000;
    }

    bool seek_to(std::int64_t ms) {
        const auto start =
            input.format->start_time == AV_NOPTS_VALUE ? 0 : input.format->start_time;
        if (av_seek_frame(input.format, -1, ms * 1000 + start, AVSEEK_FLAG_BACKWARD) < 0)
            return false;
        for (auto* track : {&video, &audio}) {
            if (track->stream)
                avcodec_flush_buffers(track->decoder.context);
            track->end_ms = -1;
            track->pending.clear();
            track->flushed = false;
        }
        if (resampler) {
            swr_close(resampler);
            if (swr_init(resampler) < 0)
                return false;
        }
        eof = false;
        input_eof = false;
        pending_bytes = pending_packets = queued_samples = 0;
        audio_frames.clear();
        audio_sent = ms * 48;
        discard_before = static_cast<double>(ms);
        image = {};
        frames.clear();
        queued_bytes = 0;
        return true;
    }

    bool drain(Track& track) {
        auto* frame = track.decoder.frame;
        int status = 0;
        while ((status = avcodec_receive_frame(track.decoder.context, frame)) >= 0) {
            const auto pts = frame->best_effort_timestamp;
            const double ms =
                pts == AV_NOPTS_VALUE
                    ? std::max(0.0, track.end_ms)
                    : static_cast<double>(pts) * av_q2d(track.stream->time_base) * 1000 - origin();
            if (!std::isfinite(ms) || std::abs(ms) > 7.0 * 24 * 60 * 60 * 1000)
                return false;
            if (&track == &video) {
                const auto fps = av_guess_frame_rate(input.format, track.stream, frame);
                const double length = fps.num > 0 && fps.den > 0 ? 1000 / av_q2d(fps) : 40;
                track.end_ms = ms + length;
                if (track.end_ms >= discard_before) {
                    std::pair<std::uint32_t, std::uint32_t> whole;
                    auto converted = convert(*frame, *track.stream, wire::kMaxSide, 720, whole);
                    if (converted.empty() || frames.size() >= 32 ||
                        queued_bytes + converted.rgba.size() > 64 * 1024 * 1024)
                        return false;
                    queued_bytes += converted.rgba.size();
                    frames.push_back({ms, std::move(converted)});
                }
            } else {
                if (frame->nb_samples < 0 || frame->nb_samples > 192000 ||
                    frame->sample_rate != audio_rate || frame->format != audio_format ||
                    av_channel_layout_compare(&frame->ch_layout, &audio_layout) != 0)
                    return false;
                const auto capacity = swr_get_out_samples(resampler, frame->nb_samples);
                if (capacity < 0 || capacity > 48000)
                    return false;
                std::vector<std::int16_t> output(static_cast<std::size_t>(capacity) * 2);
                auto* destination = reinterpret_cast<std::uint8_t*>(output.data());
                const auto samples = swr_convert(
                    resampler, &destination, capacity,
                    const_cast<const std::uint8_t**>(frame->extended_data), frame->nb_samples);
                if (samples < 0)
                    return false;
                track.end_ms = ms + static_cast<double>(samples) * 1000 / 48000;
                const auto skip =
                    static_cast<std::size_t>(std::clamp((discard_before - ms) * 48.0, 0.0,
                                                        static_cast<double>(samples))) *
                    2;
                output.resize(static_cast<std::size_t>(samples) * 2);
                if (queued_samples + output.size() - skip > wire::kMaxSamples)
                    return false;
                output.erase(output.begin(), output.begin() + static_cast<std::ptrdiff_t>(skip));
                queued_samples += output.size();
                if (!output.empty())
                    audio_frames.push_back({static_cast<std::int64_t>(std::llround(ms * 48)) +
                                                static_cast<std::int64_t>(skip / 2),
                                            std::move(output)});
            }
            av_frame_unref(frame);
        }
        return status == AVERROR(EAGAIN) || status == AVERROR_EOF;
    }

    bool packet(Track& track, AVPacket* value) {
        int sent = avcodec_send_packet(track.decoder.context, value);
        if (sent == AVERROR(EAGAIN)) {
            if (!drain(track))
                return false;
            sent = avcodec_send_packet(track.decoder.context, value);
        }
        return (sent >= 0 || sent == AVERROR_EOF) && drain(track);
    }

    bool ready(const Track& track, double target) const {
        if (!track.stream || track.flushed || track.end_ms >= target)
            return true;
        const double start = track.stream->start_time == AV_NOPTS_VALUE
                                 ? 0
                                 : static_cast<double>(track.stream->start_time) *
                                           av_q2d(track.stream->time_base) * 1000 -
                                       origin();
        // MP4 edit lists can delay a track. Do not decode seconds of video ahead just to
        // find its first audio packet: that overflows the bounded video queue on valid files.
        if (track.end_ms < 0 && target <= start)
            return true;
        const auto track_duration = track.stream->duration;
        if (track_duration <= 0 || track_duration == AV_NOPTS_VALUE)
            return false;
        const double end =
            start + static_cast<double>(track_duration) * av_q2d(track.stream->time_base) * 1000;
        return track.end_ms >= end - 1.0;
    }

    void advance_image(std::int64_t ms) {
        while (!frames.empty() &&
               (image.empty() || frames.front().time <= static_cast<double>(ms))) {
            queued_bytes -= frames.front().image.rgba.size();
            image = std::move(frames.front().image);
            frames.pop_front();
        }
    }

    bool ended(std::int64_t ms) const {
        return eof && static_cast<double>(ms) >=
                          std::max({static_cast<double>(duration), video.end_ms, audio.end_ms});
    }

    bool audio_window(std::int64_t ms) {
        if (!audio.stream)
            return true;
        const auto target = std::min<std::int64_t>(
            (ms + wire::kStepMs) * 48,
            eof ? static_cast<std::int64_t>(std::llround(std::max(0.0, audio.end_ms) * 48))
                : std::numeric_limits<std::int64_t>::max());
        if (target <= audio_sent)
            return true;
        if (target - audio_sent > wire::kMaxSamples / 2)
            return false;
        // PCM follows the media timeline, including timestamp gaps. Omitting a gap sends
        // later samples behind PulseAudio's read cursor after an underrun, losing sound.
        pcm.assign(static_cast<std::size_t>(target - audio_sent) * 2, 0);
        while (!audio_frames.empty()) {
            auto& frame = audio_frames.front();
            const auto end = frame.start + static_cast<std::int64_t>(frame.samples.size() / 2);
            if (frame.start >= target)
                break;
            const auto begin = std::max(audio_sent, frame.start);
            const auto stop = std::min(target, end);
            if (stop > begin)
                std::copy_n(frame.samples.begin() + (begin - frame.start) * 2, (stop - begin) * 2,
                            pcm.begin() + (begin - audio_sent) * 2);
            if (end > target)
                break;
            queued_samples -= frame.samples.size();
            audio_frames.pop_front();
        }
        audio_sent = target;
        return true;
    }

    bool step(std::int64_t ms) {
        pcm.clear();
        source.read = 0;
        source.deadline = Clock::now() + std::chrono::seconds{4};
        const auto target = static_cast<double>(ms + wire::kStepMs);
        for (unsigned n = 0; n < 20000 && Clock::now() < source.deadline; ++n) {
            if (eof || ((duration <= 0 || ms < duration) && ready(video, target) &&
                        ready(audio, target))) {
                advance_image(ms);
                return audio_window(ms);
            }
            bool consumed = false;
            for (auto* track : {&video, &audio}) {
                if (!track->pending.empty() &&
                    (!ready(*track, target) || (duration > 0 && ms >= duration))) {
                    auto value = std::move(track->pending.front());
                    track->pending.pop_front();
                    pending_bytes -= packet_bytes(*value);
                    --pending_packets;
                    if (!packet(*track, value.get()))
                        return false;
                    consumed = true;
                }
            }
            if (consumed)
                continue;
            if (input_eof) {
                for (auto* track : {&video, &audio}) {
                    if (track->stream && !track->flushed && track->pending.empty()) {
                        if (!packet(*track, nullptr))
                            return false;
                        track->flushed = true;
                    }
                }
                if ((!video.stream || video.flushed) && (!audio.stream || audio.flushed)) {
                    eof = true;
                    if (resampler) {
                        std::vector<std::int16_t> tail(8192);
                        auto* out = reinterpret_cast<std::uint8_t*>(tail.data());
                        const int count = swr_convert(resampler, &out, 4096, nullptr, 0);
                        if (count < 0 || queued_samples + static_cast<std::size_t>(count) * 2 >
                                             wire::kMaxSamples)
                            return false;
                        tail.resize(static_cast<std::size_t>(count) * 2);
                        queued_samples += tail.size();
                        audio_frames.push_back(
                            {static_cast<std::int64_t>(std::llround(audio.end_ms * 48)),
                             std::move(tail)});
                        audio.end_ms += static_cast<double>(count) / 48;
                    }
                    // Some phone containers omit timestamp gaps from their declared duration.
                    // Report the actual decoded end so the final clock/seek range stays honest.
                    duration = std::max(
                        duration,
                        static_cast<std::int64_t>(std::ceil(std::max(video.end_ms, audio.end_ms))));
                }
                continue;
            }
            auto* value = video.decoder.packet;
            const int result = av_read_frame(input.format, value);
            if (result < 0) {
                if (result != AVERROR_EOF)
                    return false;
                input_eof = true;
                continue;
            }
            bool okay = true;
            for (auto* track : {&video, &audio}) {
                if (!track->stream || value->stream_index != track->stream->index)
                    continue;
                if (ready(*track, target)) {
                    // Phone MP4s commonly interleave a whole second of one stream. Keep
                    // already-ready packets compressed instead of filling the RGBA queue.
                    const auto bytes = packet_bytes(*value);
                    if (pending_packets >= 512 || bytes > kMaxPendingBytes - pending_bytes)
                        return false;
                    SavedPacket saved{av_packet_clone(value)};
                    if (!saved)
                        return false;
                    pending_bytes += bytes;
                    ++pending_packets;
                    track->pending.push_back(std::move(saved));
                } else {
                    okay = packet(*track, value) && okay;
                }
            }
            av_packet_unref(value);
            if (!okay)
                return false;
        }
        return false;
    }
};

int payload(const Session& session) {
    int fd = ::memfd_create("rummage-playback", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0)
        return -1;
    for (auto bytes :
         {std::as_bytes(std::span{session.image.rgba}), std::as_bytes(std::span{session.pcm})}) {
        while (!bytes.empty()) {
            const auto wrote = ::write(fd, bytes.data(), bytes.size());
            if (wrote < 0 && errno == EINTR)
                continue;
            if (wrote <= 0) {
                ::close(fd);
                return -1;
            }
            bytes = bytes.subspan(static_cast<std::size_t>(wrote));
        }
    }
    if (::fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}
}  // namespace

int main() {
    av_log_set_level(AV_LOG_QUIET);
    static_cast<void>(av_get_cpu_flags());
    av_cpu_force_count(1);
    if (!rummage::isolate::enter_sandbox(rummage::isolate::Unexpected::kill).seccomp)
        return protocol::kSandboxFailedExit;
    auto first = protocol::receive_packet(3);
    if (!first || first->length != sizeof(wire::Request) || first->fds[0] < 0)
        return 1;
    const int inputfd = first->fds[0];
    first->fds[0] = -1;
    first->close_fds();
    struct stat stat{};
    if (::fstat(inputfd, &stat) < 0 || !S_ISREG(stat.st_mode) || stat.st_size <= 0)
        return 1;
    Session session{inputfd, stat.st_size};
    bool okay = session.open();
    auto message = std::move(first);
    while (message) {
        wire::Request request;
        if (message->length != sizeof(request)) {
            message->close_fds();
            return 1;
        }
        std::memcpy(&request, message->bytes.data(), sizeof(request));
        message->close_fds();
        if (request.magic != wire::kMagic || request.seek > 1 || request.position_ms < 0 ||
            request.position_ms > 7LL * 24 * 60 * 60 * 1000)
            return 1;
        wire::Response response;
        int fd = -1;
        if (okay) {
            session.source.read = 0;
            session.source.deadline = Clock::now() + std::chrono::seconds{4};
            if (request.seek && !session.seek_to(request.position_ms))
                response.status = 3;
            else if (!session.step(request.position_ms))
                response.status = 2;
            else {
                response.status = session.ended(request.position_ms) ? 1U : 0U;
                response.duration_ms = session.duration;
                response.width = session.image.width;
                response.height = session.image.height;
                response.samples = static_cast<std::uint32_t>(session.pcm.size());
                response.audio = session.audio.stream ? 1U : 0U;
                fd = payload(session);
                if (fd < 0)
                    response.status = 2;
            }
        } else
            response.status = 2;
        const std::array<int, 1> fds{fd};
        bool sent =
            protocol::send_packet(3, std::as_bytes(std::span{&response, 1}),
                                  fd >= 0 ? std::span<const int>{fds} : std::span<const int>{});
        if (fd >= 0)
            ::close(fd);
        if (!sent || response.status >= 2)
            break;
        message = protocol::receive_packet(3);
    }
    ::close(inputfd);
}
