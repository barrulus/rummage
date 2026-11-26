#include "backend.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace rummage::isolate::backend {
namespace {
FT_Library library = nullptr;
constexpr std::size_t kMaxFontBytes = 32 * 1024 * 1024;

picture::Loaded failed(picture::Failure reason, std::string detail) {
    picture::Loaded result;
    result.failure = reason;
    result.detail = std::move(detail);
    return result;
}

struct Face {
    FT_Face value{nullptr};

    ~Face() {
        if (value)
            FT_Done_Face(value);
    }
};
}  // namespace

void warm_up() {
    if (!library)
        static_cast<void>(FT_Init_FreeType(&library));
}

picture::Loaded make_picture(int fd, std::uint32_t width, std::uint32_t height,
                             std::uint32_t page) {
    if (!library)
        return failed(picture::Failure::unavailable, "FreeType did not initialize");
    if (page != 0)
        return failed(picture::Failure::unknown_format, "font collections show their first face");
    struct stat stat{};
    if (::fstat(fd, &stat) != 0 || !S_ISREG(stat.st_mode) || stat.st_size <= 0)
        return failed(picture::Failure::unreadable, "font is not a readable regular file");
    if (static_cast<std::uint64_t>(stat.st_size) > kMaxFontBytes)
        return failed(picture::Failure::too_large, "font exceeds 32 MiB input limit");
    std::vector<unsigned char> bytes(static_cast<std::size_t>(stat.st_size));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto got =
            ::pread(fd, bytes.data() + offset, bytes.size() - offset, static_cast<off_t>(offset));
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            return failed(picture::Failure::unreadable, "font read failed");
        offset += static_cast<std::size_t>(got);
    }
    Face face;
    if (FT_New_Memory_Face(library, bytes.data(), static_cast<FT_Long>(bytes.size()), 0,
                           &face.value) != 0)
        return failed(picture::Failure::damaged, "FreeType could not read font");
    if (!FT_IS_SCALABLE(face.value))
        return failed(picture::Failure::unknown_format, "bitmap-only fonts are not previewed");
    width = std::clamp(width, 1U, 1024U);
    height = std::clamp(height, 1U, 1024U);
    // A fixed specimen aspect ratio makes thumbnails and Quick Look share the same content.
    if (width * 3 > height * 5)
        width = std::max(1U, height * 5 / 3);
    else
        height = std::max(1U, width * 3 / 5);
    picture::Loaded result;
    result.image = {
        .width = width,
        .height = height,
        .rgba = std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4, 255)};
    const auto size = std::max(1U, width / 14);
    if (FT_Set_Pixel_Sizes(face.value, 0, size) != 0)
        return failed(picture::Failure::damaged, "font size could not be set");
    const std::array<std::string_view, 4> lines{"Aa Bb Cc", "0123456789", "The quick", "brown fox"};
    bool ink = false;
    for (std::size_t line = 0; line < lines.size(); ++line) {
        std::int64_t x = width / 20;
        const auto baseline = static_cast<std::int64_t>(height / 8 + (line + 1) * size * 5 / 4);
        for (const char c : lines[line]) {
            if (FT_Load_Char(face.value, static_cast<unsigned char>(c),
                             FT_LOAD_RENDER | FT_LOAD_NO_BITMAP) != 0)
                continue;
            const auto& glyph = *face.value->glyph;
            const auto& bitmap = glyph.bitmap;
            if (bitmap.pixel_mode != FT_PIXEL_MODE_GRAY || bitmap.pitch < 0)
                continue;
            for (unsigned row = 0; row < bitmap.rows; ++row) {
                const auto y = baseline - glyph.bitmap_top + static_cast<std::int64_t>(row);
                if (y < 0 || y >= static_cast<int>(height))
                    continue;
                for (unsigned col = 0; col < bitmap.width; ++col) {
                    const auto px = x + glyph.bitmap_left + static_cast<std::int64_t>(col);
                    if (px < 0 || px >= static_cast<int>(width))
                        continue;
                    const auto coverage = bitmap.buffer[static_cast<std::size_t>(row) *
                                                            static_cast<unsigned>(bitmap.pitch) +
                                                        col];
                    const auto index =
                        (static_cast<std::size_t>(y) * width + static_cast<unsigned>(px)) * 4;
                    const auto shade = static_cast<std::uint8_t>(255 - coverage);
                    for (std::size_t channel = 0; channel < 3; ++channel)
                        result.image.rgba[index + channel] =
                            std::min(result.image.rgba[index + channel], shade);
                    ink = ink || coverage != 0;
                }
            }
            x += glyph.advance.x / 64;
        }
    }
    if (!ink)
        return failed(picture::Failure::no_readable_preview,
                      "font contains no usable specimen glyphs");
    return result;
}

picture::PhotoInfo photo_info(int) { return {}; }

text::Loaded read_text(int, const text::Limits&) {
    text::Loaded result;
    result.failure = text::Failure::unreadable;
    return result;
}

void colour(text::Text&, std::string_view, std::string_view) {}
}  // namespace rummage::isolate::backend
