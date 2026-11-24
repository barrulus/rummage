#pragma once

#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include "rummage/core/i18n.hpp"
#include "rummage/picture/photo_info.hpp"

namespace rummage::views {
namespace detail {
// Helpers exchange locale-neutral metadata. Translate only known application-owned fields
// here; embedded tags, profile names, titles and codec identifiers remain literal.
inline std::string_view media_detail_label(std::string_view label) {
    if (label == "Format")
        return i18n::tr("Format");
    if (label == "Color model")
        return i18n::tr("Color model");
    if (label == "Bit depth")
        return i18n::tr("Bit depth");
    if (label == "Compression")
        return i18n::tr("Compression");
    if (label == "Compression mode")
        return i18n::tr("Compression mode");
    if (label == "Byte order")
        return i18n::tr("Byte order");
    if (label == "Interlacing")
        return i18n::tr("Interlacing");
    if (label == "Filter method")
        return i18n::tr("Filter method");
    if (label == "Gamma")
        return i18n::tr("Gamma");
    if (label == "Color profile")
        return i18n::tr("Color profile");
    if (label == "Rendering intent")
        return i18n::tr("Rendering intent");
    if (label == "ICC profile")
        return i18n::tr("ICC profile");
    if (label == "Pixel density")
        return i18n::tr("Pixel density");
    if (label == "Image modified")
        return i18n::tr("Image modified");
    if (label == "Frames")
        return i18n::tr("Frames");
    if (label == "Loop count")
        return i18n::tr("Loop count");
    if (label == "Transparency")
        return i18n::tr("Transparency");
    if (label == "Components")
        return i18n::tr("Components");
    if (label == "Encoding")
        return i18n::tr("Encoding");
    if (label == "Component sampling")
        return i18n::tr("Component sampling");
    if (label == "Alpha")
        return i18n::tr("Alpha");
    if (label == "Images")
        return i18n::tr("Images");
    if (label == "Color primaries")
        return i18n::tr("Color primaries");
    if (label == "Transfer characteristics")
        return i18n::tr("Transfer characteristics");
    if (label == "Matrix coefficients")
        return i18n::tr("Matrix coefficients");
    if (label == "Color range")
        return i18n::tr("Color range");
    if (label == "Color resolution")
        return i18n::tr("Color resolution");
    if (label == "Global palette entries")
        return i18n::tr("Global palette entries");
    if (label == "Animation")
        return i18n::tr("Animation");
    if (label == "Color channels")
        return i18n::tr("Color channels");
    if (label == "Extra channels")
        return i18n::tr("Extra channels");
    if (label == "Alpha depth")
        return i18n::tr("Alpha depth");
    if (label == "Intensity target")
        return i18n::tr("Intensity target");
    if (label == "Metadata error")
        return i18n::tr("Metadata error");
    if (label == "Streams")
        return i18n::tr("Streams");
    if (label == "Chapters")
        return i18n::tr("Chapters");
    if (label == "Pages")
        return i18n::tr("Pages");
    if (label == "First page size")
        return i18n::tr("First page size");
    if (label == "Page rotation")
        return i18n::tr("Page rotation");
    return label;
}

inline std::string media_detail_value(std::string_view label, const std::string& value) {
    if ((label == "Bit depth" || label == "Alpha depth" || label == "Color resolution") &&
        value.ends_with(" bits"))
        return i18n::trf("{} bits", value.substr(0, value.size() - 5));
    if (label != "Color model" && label != "Compression" && label != "Compression mode" &&
        label != "Byte order" && label != "Interlacing" && label != "Rendering intent" &&
        label != "Loop count" && label != "Transparency" && label != "Encoding" &&
        label != "Alpha" && label != "Color range" && label != "Animation")
        return value;
    if (value == "None")
        return std::string{i18n::tr("None")};
    if (value == "Unknown")
        return std::string{i18n::tr("Unknown")};
    if (value == "Uncompressed")
        return std::string{i18n::tr("Uncompressed")};
    if (value == "Lossless")
        return std::string{i18n::tr("Lossless")};
    if (value == "Lossy")
        return std::string{i18n::tr("Lossy")};
    if (value == "Mixed / animated")
        return std::string{i18n::tr("Mixed / animated")};
    if (value == "White is zero")
        return std::string{i18n::tr("White is zero")};
    if (value == "Grayscale")
        return std::string{i18n::tr("Grayscale")};
    if (value == "Indexed")
        return std::string{i18n::tr("Indexed")};
    if (value == "Mask")
        return std::string{i18n::tr("Mask")};
    if (value == "Grayscale + alpha")
        return std::string{i18n::tr("Grayscale + alpha")};
    if (value == "Indexed RGB")
        return std::string{i18n::tr("Indexed RGB")};
    if (value == "Little endian")
        return std::string{i18n::tr("Little endian")};
    if (value == "Big endian")
        return std::string{i18n::tr("Big endian")};
    if (value == "Perceptual")
        return std::string{i18n::tr("Perceptual")};
    if (value == "Relative colorimetric")
        return std::string{i18n::tr("Relative colorimetric")};
    if (value == "Saturation")
        return std::string{i18n::tr("Saturation")};
    if (value == "Absolute colorimetric")
        return std::string{i18n::tr("Absolute colorimetric")};
    if (value == "Infinite")
        return std::string{i18n::tr("Infinite")};
    if (value == "Yes (transparency chunk)")
        return std::string{i18n::tr("Yes (transparency chunk)")};
    if (value == "Baseline DCT")
        return std::string{i18n::tr("Baseline DCT")};
    if (value == "Progressive DCT")
        return std::string{i18n::tr("Progressive DCT")};
    if (value == "Yes")
        return std::string{i18n::tr("Yes")};
    if (value == "No")
        return std::string{i18n::tr("No")};
    if (value == "Full")
        return std::string{i18n::tr("Full")};
    if (value == "Limited")
        return std::string{i18n::tr("Limited")};
    return value;
}
}  // namespace detail

enum class MediaDetails { all, summary, additional };

// Only metadata present in the file. The same labels appear in both preview surfaces.
inline std::vector<std::pair<std::string, std::string>> media_details(
    const picture::PhotoInfo& info, MediaDetails section = MediaDetails::all) {
    std::vector<std::pair<std::string, std::string>> rows;
    const auto add = [&](std::string_view label, const std::string& value, bool summary = false) {
        if ((section == MediaDetails::summary && !summary) ||
            (section == MediaDetails::additional && summary))
            return;
        if (!value.empty())
            rows.emplace_back(label, value);
    };
    if (info.width && info.height) {
        const auto divisor = std::gcd(info.width, info.height);
        add(i18n::tr("Aspect ratio"),
            std::to_string(info.width / divisor) + ":" + std::to_string(info.height / divisor));
    }
    add(i18n::tr("Camera"), info.camera);
    add(i18n::tr("Lens"), info.lens);
    add(i18n::tr("Shutter speed"), info.exposure_time);
    add(i18n::tr("Aperture"), info.aperture);
    add(i18n::tr("ISO"), info.iso);
    add(i18n::tr("Focal length"), info.focal_length);
    add(i18n::tr("Recorded"), info.taken);
    add(i18n::tr("Exposure program"), info.program);
    add(i18n::tr("Exposure compensation"), info.exposure_bias);
    add(i18n::tr("Metering"), info.metering);
    add(i18n::tr("White balance"), info.white_balance);
    add(i18n::tr("Flash"), info.flash);
    add(i18n::tr("Container"), info.container, true);
    add(i18n::tr("Duration"), info.duration, true);
    add(i18n::tr("Video codec"), info.video_codec, true);
    add(i18n::tr("Frame rate"), info.frame_rate);
    add(i18n::tr("Bitrate"), info.bitrate);
    add(i18n::tr("Pixel format"), info.pixel_format);
    add(i18n::tr("Rotation"), info.rotation);
    add(i18n::tr("Audio codec"), info.audio_codec);
    add(i18n::tr("Audio channels"), info.audio_channels);
    add(i18n::tr("Sample rate"), info.sample_rate);
    add(i18n::tr("Title"), info.media_title);
    add(i18n::tr("Artist"), info.artist);
    add(i18n::tr("Software"), info.software);
    if (info.rating)
        add(i18n::tr("Rating"),
            *info.rating == -1 ? std::string{i18n::tr("Rejected")} : std::to_string(*info.rating));
    if (section != MediaDetails::summary)
        for (const auto& [label, value] : info.details)
            rows.emplace_back(detail::media_detail_label(label),
                              detail::media_detail_value(label, value));
    if (info.details_truncated)
        add(i18n::tr("Metadata"),
            std::string{i18n::tr("Some metadata was truncated by the preview limits")});
    return rows;
}
}  // namespace rummage::views
