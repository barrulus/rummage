#pragma once

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
    return rows;
}
}  // namespace rummage::views
