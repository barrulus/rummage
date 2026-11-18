#pragma once

#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include "rummage/picture/photo_info.hpp"

namespace rummage::views {
// Only metadata present in the file. The same labels appear in both preview surfaces.
inline std::vector<std::pair<std::string, std::string>> media_details(
    const picture::PhotoInfo& info) {
    std::vector<std::pair<std::string, std::string>> rows;
    const auto add = [&](const char* label, const std::string& value) {
        if (!value.empty())
            rows.emplace_back(label, value);
    };
    if (info.width && info.height) {
        const auto divisor = std::gcd(info.width, info.height);
        add("Aspect ratio",
            std::to_string(info.width / divisor) + ":" + std::to_string(info.height / divisor));
    }
    add("Camera", info.camera);
    add("Lens", info.lens);
    add("Shutter speed", info.exposure_time);
    add("Aperture", info.aperture);
    add("ISO", info.iso);
    add("Focal length", info.focal_length);
    add("Recorded", info.taken);
    add("Exposure program", info.program);
    add("Exposure compensation", info.exposure_bias);
    add("Metering", info.metering);
    add("White balance", info.white_balance);
    add("Flash", info.flash);
    add("Container", info.container);
    add("Duration", info.duration);
    add("Video codec", info.video_codec);
    add("Frame rate", info.frame_rate);
    add("Bitrate", info.bitrate);
    add("Pixel format", info.pixel_format);
    add("Rotation", info.rotation);
    add("Audio codec", info.audio_codec);
    add("Audio channels", info.audio_channels);
    add("Sample rate", info.sample_rate);
    add("Title", info.media_title);
    add("Artist", info.artist);
    add("Software", info.software);
    if (info.rating)
        add("Rating", *info.rating == -1 ? "Rejected" : std::to_string(*info.rating));
    rows.insert(rows.end(), info.details.begin(), info.details.end());
    if (info.details_truncated)
        add("Metadata", "Some metadata was truncated by the preview limits");
    return rows;
}
}  // namespace rummage::views
