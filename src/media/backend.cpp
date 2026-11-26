#include "decoder_common.hpp"

namespace rummage::isolate::backend {
namespace {
picture::Loaded failure(picture::Failure reason, const char* detail) {
    picture::Loaded value;
    value.failure = reason;
    value.detail = detail;
    return value;
}

bool dark(const picture::Image& image) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
        count += image.rgba[i] < 12 && image.rgba[i + 1] < 12 && image.rgba[i + 2] < 12;
    }
    return count * 100 >= (image.rgba.size() / 4) * 98;
}

}  // namespace

void warm_up() {
    av_log_set_level(AV_LOG_QUIET);
    // Cache FFmpeg CPU detection and prevent automatic threading before sandboxing.
    static_cast<void>(av_get_cpu_flags());
    av_cpu_force_count(1);
}

picture::Loaded make_picture(int fd, std::uint32_t width, std::uint32_t height,
                             std::uint32_t page) {
    if (page != 0)
        return failure(picture::Failure::unknown_format, "video previews do not have pages");
    if (width == 0 || height == 0) {
        return failure(picture::Failure::too_large, "empty video preview size");
    }
    width = std::min(width, 1024U);
    height = std::min(height, 1024U);
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        return failure(picture::Failure::unreadable, "video input is not a regular file");
    }
    Source source{.fd = fd, .size = st.st_size};
    Input input;
    if (!open_input(input, source))
        return failure(picture::Failure::damaged, "unreadable video container");
    AVStream* stream = nullptr;
    for (unsigned i = 0; i < input.format->nb_streams; ++i) {
        auto* candidate = input.format->streams[i];
        if (candidate->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
            (candidate->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0) {
            stream = candidate;
            break;
        }
    }
    if (stream == nullptr) {
        return failure(picture::Failure::unknown_format, "no video stream");
    }
    if (stream->time_base.num <= 0 || stream->time_base.den <= 0) {
        return failure(picture::Failure::damaged, "invalid video time base");
    }
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (codec == nullptr) {
        return failure(picture::Failure::unavailable, "video codec unavailable");
    }
    Decoder decoder;
    decoder.context = avcodec_alloc_context3(codec);
    if (decoder.context == nullptr || decoder.packet == nullptr || decoder.frame == nullptr ||
        avcodec_parameters_to_context(decoder.context, stream->codecpar) < 0) {
        return failure(picture::Failure::unavailable, "could not allocate video decoder");
    }
    decoder.context->thread_count = 1;
    decoder.context->thread_type = 0;
    decoder.context->max_pixels = kMaxPixels;
    if (avcodec_open2(decoder.context, codec, nullptr) < 0) {
        return failure(picture::Failure::damaged, "could not open video decoder");
    }
    const auto duration = input.format->duration;
    const std::array<std::int64_t, 3> positions{duration > AV_TIME_BASE ? duration / 10 : 0,
                                                duration > AV_TIME_BASE ? duration / 3 : 0, 0};
    picture::Image first_dark;
    std::pair<std::uint32_t, std::uint32_t> first_dark_whole{0, 0};
    for (std::size_t attempt = 0; attempt < positions.size(); ++attempt) {
        if (attempt > 0 && positions[0] == 0) {
            break;
        }
        const auto start =
            input.format->start_time == AV_NOPTS_VALUE ? 0 : input.format->start_time;
        if (start > 0 && positions[attempt] > std::numeric_limits<std::int64_t>::max() - start) {
            continue;
        }
        const auto timestamp =
            av_rescale_q(positions[attempt] + start, AV_TIME_BASE_Q, stream->time_base);
        if (av_seek_frame(input.format, stream->index, timestamp, AVSEEK_FLAG_BACKWARD) < 0 &&
            positions[attempt] != 0) {
            continue;
        }
        avcodec_flush_buffers(decoder.context);
        bool flushing = false;
        for (unsigned packets = 0; packets < 1024 && Clock::now() < source.deadline; ++packets) {
            const int received = avcodec_receive_frame(decoder.context, decoder.frame);
            if (received == 0) {
                const auto pts = decoder.frame->best_effort_timestamp;
                if (positions[attempt] > 0 && pts != AV_NOPTS_VALUE && pts < timestamp) {
                    av_frame_unref(decoder.frame);
                    continue;
                }
                std::pair<std::uint32_t, std::uint32_t> whole{0, 0};
                auto image = convert(*decoder.frame, *stream, width, height, whole);
                av_frame_unref(decoder.frame);
                if (image.empty()) {
                    break;
                }
                if (!dark(image)) {
                    picture::Loaded loaded;
                    loaded.image = std::move(image);
                    loaded.full_width = whole.first;
                    loaded.full_height = whole.second;
                    return loaded;
                }
                if (first_dark.empty()) {
                    first_dark = std::move(image);
                    first_dark_whole = whole;
                }
                break;
            }
            if (flushing || (received != AVERROR(EAGAIN) && received != AVERROR_EOF)) {
                break;
            }
            av_packet_unref(decoder.packet);
            if (av_read_frame(input.format, decoder.packet) < 0) {
                static_cast<void>(avcodec_send_packet(decoder.context, nullptr));
                flushing = true;
            } else if (decoder.packet->stream_index == stream->index) {
                if (avcodec_send_packet(decoder.context, decoder.packet) < 0) {
                    break;
                }
            }
        }
    }
    if (!first_dark.empty()) {
        picture::Loaded loaded;
        loaded.image = std::move(first_dark);
        loaded.full_width = first_dark_whole.first;
        loaded.full_height = first_dark_whole.second;
        return loaded;
    }
    return failure(Clock::now() >= source.deadline || source.read >= kReadBudget
                       ? picture::Failure::unavailable
                       : picture::Failure::damaged,
                   "no video frame within the decoding budget");
}

picture::PhotoInfo photo_info(int fd) {
    picture::PhotoInfo info;
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0)
        return info;
    Source source{.fd = fd, .size = st.st_size};
    Input input;
    if (!open_input(input, source))
        return info;
    AVStream* video = nullptr;
    AVStream* audio = nullptr;
    for (unsigned i = 0; i < input.format->nb_streams; ++i) {
        auto* stream = input.format->streams[i];
        if (!video && stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
            !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC))
            video = stream;
        if (!audio && stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
            audio = stream;
    }
    if (!video && !audio)
        return info;
    const auto tag = [&](const char* key) {
        const AVDictionaryEntry* value = av_dict_get(input.format->metadata, key, nullptr, 0);
        if (!value)
            value = av_dict_get((video ? video : audio)->metadata, key, nullptr, 0);
        std::string text;
        if (value && value->value) {
            for (const char* at = value->value; *at && text.size() < 240; ++at)
                if (static_cast<unsigned char>(*at) >= 32 && *at != 127)
                    text += *at;
        }
        return text;
    };
    info.container = input.format->iformat->long_name ? input.format->iformat->long_name
                                                      : input.format->iformat->name;
    if (input.format->duration > 0) {
        const auto milliseconds = input.format->duration / (AV_TIME_BASE / 1000);
        info.duration_ms = static_cast<std::uint64_t>(milliseconds);
        const auto seconds = milliseconds / 1000;
        info.duration = std::format("{}:{:02}:{:02}.{:03}", seconds / 3600, seconds / 60 % 60,
                                    seconds % 60, milliseconds % 1000);
    }
    if (input.format->bit_rate > 0)
        info.bitrate_bps = static_cast<std::uint64_t>(input.format->bit_rate);
    if (input.format->bit_rate > 0)
        info.bitrate =
            std::format("{:.0f} kbit/s", static_cast<double>(input.format->bit_rate) / 1000.0);
    if (video) {
        info.video_codec = avcodec_get_name(video->codecpar->codec_id);
        info.width = static_cast<std::uint32_t>(std::max(0, video->codecpar->width));
        info.height = static_cast<std::uint32_t>(std::max(0, video->codecpar->height));
        const auto rate = video->avg_frame_rate;
        if (rate.num > 0 && rate.den > 0)
            info.frame_rate = std::format("{:.3f} fps", av_q2d(rate));
        if (const char* name =
                av_get_pix_fmt_name(static_cast<AVPixelFormat>(video->codecpar->format)))
            info.pixel_format = name;
        if (const auto* matrix = av_packet_side_data_get(video->codecpar->coded_side_data,
                                                         video->codecpar->nb_coded_side_data,
                                                         AV_PKT_DATA_DISPLAYMATRIX);
            matrix && matrix->size >= 9 * sizeof(std::int32_t)) {
            const double angle =
                av_display_rotation_get(reinterpret_cast<const std::int32_t*>(matrix->data));
            if (std::isfinite(angle)) {
                info.rotation = std::format("{:.0f}°", -angle);
                const int turn = (static_cast<int>(std::lround(-angle / 90.0)) % 4 + 4) % 4;
                if (turn == 1 || turn == 3)
                    std::swap(info.width, info.height);
            }
        }
    }
    if (audio) {
        info.audio_codec = avcodec_get_name(audio->codecpar->codec_id);
        if (audio->codecpar->ch_layout.nb_channels > 0)
            info.audio_channels = std::to_string(audio->codecpar->ch_layout.nb_channels);
        if (audio->codecpar->sample_rate > 0)
            info.sample_rate = std::format("{} Hz", audio->codecpar->sample_rate);
    }
    const auto tags = [&](const std::string& prefix, AVDictionary* dictionary) {
        const AVDictionaryEntry* item = nullptr;
        while ((item = av_dict_get(dictionary, "", item, AV_DICT_IGNORE_SUFFIX))) {
            if (info.details.size() >= picture::PhotoInfo::max_details) {
                info.details_truncated = true;
                break;
            }
            info.add_detail(prefix + item->key, item->value);
        }
    };
    tags("Container · ", input.format->metadata);
    info.add_detail("Streams", std::to_string(input.format->nb_streams));
    info.add_detail("Chapters", std::to_string(input.format->nb_chapters));
    for (unsigned i = 0; i < input.format->nb_streams; ++i) {
        if (info.details.size() >= picture::PhotoInfo::max_details) {
            info.details_truncated = true;
            break;
        }
        auto* stream = input.format->streams[i];
        const auto* codec = stream->codecpar;
        const auto prefix = std::format("Stream {} · ", i + 1);
        const auto add = [&](const char* name, const char* value) {
            if (value)
                info.add_detail(prefix + name, value);
        };
        add("Type", av_get_media_type_string(codec->codec_type));
        add("Codec", avcodec_get_name(codec->codec_id));
        add("Profile", avcodec_profile_name(codec->codec_id, codec->profile));
        if (codec->bit_rate > 0)
            info.add_detail(prefix + "Bitrate", std::format("{} bit/s", codec->bit_rate));
        if (codec->bits_per_raw_sample > 0)
            info.add_detail(prefix + "Bit depth",
                            std::format("{} bits", codec->bits_per_raw_sample));
        if (stream->duration != AV_NOPTS_VALUE)
            info.add_detail(prefix + "Duration",
                            std::format("{:.6f} s", static_cast<double>(stream->duration) *
                                                        av_q2d(stream->time_base)));
        if (stream->nb_frames > 0)
            info.add_detail(prefix + "Frames", std::to_string(stream->nb_frames));
        if (codec->codec_type == AVMEDIA_TYPE_VIDEO) {
            info.add_detail(prefix + "Coded dimensions",
                            std::format("{} × {}", codec->width, codec->height));
            const auto sar = av_guess_sample_aspect_ratio(input.format, stream, nullptr);
            if (sar.num > 0 && sar.den > 0)
                info.add_detail(prefix + "Pixel aspect ratio",
                                std::format("{}:{}", sar.num, sar.den));
            add("Color range", av_color_range_name(codec->color_range));
            add("Color primaries", av_color_primaries_name(codec->color_primaries));
            add("Transfer characteristics", av_color_transfer_name(codec->color_trc));
            add("Color space", av_color_space_name(codec->color_space));
            add("Chroma location", av_chroma_location_name(codec->chroma_location));
        }
        if (codec->codec_type == AVMEDIA_TYPE_AUDIO) {
            info.add_detail(prefix + "Sample rate", std::format("{} Hz", codec->sample_rate));
            std::array<char, 256> layout{};
            if (av_channel_layout_describe(&codec->ch_layout, layout.data(), layout.size()) >= 0)
                add("Channel layout", layout.data());
        }
        tags(prefix, stream->metadata);
    }
    for (unsigned i = 0;
         i < input.format->nb_chapters && info.details.size() < picture::PhotoInfo::max_details;
         ++i) {
        const auto* chapter = input.format->chapters[i];
        const auto prefix = std::format("Chapter {} · ", i + 1);
        info.add_detail(
            prefix + "Time",
            std::format("{:.3f}–{:.3f} s",
                        static_cast<double>(chapter->start) * av_q2d(chapter->time_base),
                        static_cast<double>(chapter->end) * av_q2d(chapter->time_base)));
        tags(prefix, chapter->metadata);
    }
    info.media_title = tag("title");
    info.artist = tag("artist");
    info.taken = tag("creation_time");
    info.software = tag("encoder");
    return info;
}

text::Loaded read_text(int, const text::Limits&) {
    text::Loaded value;
    value.failure = text::Failure::unavailable;
    return value;
}

void colour(text::Text&, std::string_view, std::string_view) {}
}  // namespace rummage::isolate::backend
