#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include <doctest/doctest.h>

#include "rummage/ui/text.hpp"

namespace ui = rummage::ui;

namespace {

// Horizontal centre of a glyph's ink in physical pixels, from the pen position.
double ink_centre(const ui::GlyphBitmap& bitmap) {
    double weight = 0.0;
    double sum = 0.0;
    for (std::int32_t y = 0; y < bitmap.height; ++y) {
        for (std::int32_t x = 0; x < bitmap.width; ++x) {
            const double alpha =
                bitmap
                    .pixels[(static_cast<std::size_t>(y) * static_cast<std::size_t>(bitmap.width)) +
                            static_cast<std::size_t>(x)];
            weight += alpha;
            sum += alpha * (x + 0.5);
        }
    }
    REQUIRE(weight > 0.0);
    return bitmap.left + (sum / weight);
}

}  // namespace

TEST_CASE("glyphs: a fractional pen position moves the glyph's ink by that fraction") {
    ui::FontSystem fonts;
    const ui::TextLayout& layout = fonts.layout("l", 24.0F, 1.0F);
    REQUIRE(layout.glyphs.size() == 1);
    const ui::ShapedGlyph glyph = layout.glyphs.front();
    const ui::GlyphBitmap whole = fonts.rasterize(glyph.font, glyph.glyph, layout.pixel_size);
    REQUIRE(whole.width > 0);
    REQUIRE_FALSE(whole.color);
    for (const float offset : {0.25F, 0.5F, 0.75F}) {
        CAPTURE(offset);
        const ui::GlyphBitmap moved =
            fonts.rasterize(glyph.font, glyph.glyph, layout.pixel_size, offset);
        if (!fonts.subpixel_positioning()) {
            // Full hinting or monochrome (from fontconfig): glyphs sit on whole pixels, and a
            // 1-bit glyph's ink is too coarse to measure a quarter.
            continue;
        }
        CHECK(std::abs(ink_centre(moved) - ink_centre(whole) - static_cast<double>(offset)) < 0.1);
    }
}

TEST_CASE("glyphs: the same pixel size at another scale is its own layout (point size)") {
    // harfbuzz is given the point size, so 24 px at scale 1 and 12 px at scale 2 may track
    // differently (AAT trak); the cache must not hand one out for the other.
    ui::FontSystem fonts;
    const ui::TextLayout& one = fonts.layout("Tracking", 24.0F, 1.0F);
    const float width_one = one.width;
    const float pixels_one = one.pixel_size;
    const ui::TextLayout& two = fonts.layout("Tracking", 12.0F, 2.0F);
    CHECK(two.pixel_size == pixels_one);
    CHECK(&one != &two);
    CHECK(width_one > 0.0F);
    CHECK(two.width > 0.0F);
}

TEST_CASE("glyphs: monochrome rendering honors disabled hinting") {
    ui::FontSystem hinted{"DejaVu Sans:antialias=false:hinting=true:hintstyle=hintfull"};
    ui::FontSystem disabled{"DejaVu Sans:antialias=false:hinting=false:hintstyle=hintfull"};
    ui::FontSystem none{"DejaVu Sans:antialias=false:hinting=true:hintstyle=hintnone"};
    CHECK_FALSE(hinted.subpixel_positioning());
    CHECK_FALSE(disabled.subpixel_positioning());
    CHECK_FALSE(none.subpixel_positioning());
    const auto raster = [](ui::FontSystem& fonts, const char* text) {
        const auto& layout = fonts.layout(text, 13.0F, 1.0F);
        REQUIRE(layout.glyphs.size() == 1);
        const auto& glyph = layout.glyphs.front();
        return fonts.rasterize(glyph.font, glyph.glyph, layout.pixel_size);
    };
    const auto same = [](const ui::GlyphBitmap& a, const ui::GlyphBitmap& b) {
        return a.width == b.width && a.height == b.height && a.left == b.left && a.top == b.top &&
               a.pixels == b.pixels;
    };
    bool differs_from_hinted = false;
    for (const auto* text : {"m", "W", "a", "l", "i"}) {
        CAPTURE(text);
        const auto unhinted = raster(disabled, text);
        REQUIRE(unhinted.width > 0);
        CHECK_FALSE(unhinted.color);
        CHECK(std::ranges::all_of(unhinted.pixels,
                                  [](std::uint8_t alpha) { return alpha == 0 || alpha == 255; }));
        CHECK(same(unhinted, raster(none, text)));
        differs_from_hinted |= !same(unhinted, raster(hinted, text));
    }
    CHECK(differs_from_hinted);
}
