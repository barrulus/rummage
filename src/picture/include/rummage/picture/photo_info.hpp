#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace rummage::picture {

// Media metadata: photo EXIF ("1/2000 s", "F5.6") or video stream/container details.
// The historical name is retained for the shared helper API. Missing fields are empty.
struct PhotoInfo {
    // The image's size upright (the EXIF orientation applied); 0 when unknown.
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::string camera;         // "OM-1", or "Canon EOS 5DS" (the maker added when missing)
    std::string lens;           // "OLYMPUS M.12-100mm F4.0"
    std::string exposure_time;  // "1/2000 s"
    std::string aperture;       // "F5.6"
    std::string iso;            // "200"
    std::string focal_length;   // "25.0 mm"
    std::string taken;          // "2026-08-31 16:34"
    std::string program;        // "Aperture priority"
    std::string exposure_bias;  // "+7/10 EV"
    std::string metering;       // "Spot"
    std::string flash;          // "No, compulsory"
    std::string white_balance;  // "Manual"
    std::string software;       // "Version 1.9"
    // Video fields, supplied only by the optional media helper. Empty means unavailable.
    std::string container;
    std::string duration;
    std::string video_codec;
    std::string frame_rate;
    std::string bitrate;
    std::string audio_codec;
    std::string audio_channels;
    std::string sample_rate;
    std::string media_title;
    std::string artist;
    std::string rotation;
    std::string pixel_format;
    // rummage's own rating (ROADMAP 5, not yet read: nullopt): 1-5 stars, -1 rejected. Editors'
    // sidecars (darktable's FILE.xmp) are left to a plugin (decided with the author, 2026-11-08).
    std::optional<int> rating;
};

}  // namespace rummage::picture
