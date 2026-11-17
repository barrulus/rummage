#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rummage::ui {

enum class Ellipsis : std::uint8_t {
    end,     // "a very long na…"
    middle,  // "a very lo…me.jpg": keeps the extension of a file name visible
};

// Which fallback chain text is set in: the interface font, or a monospace one (text previews).
enum class Typeface : std::uint8_t { proportional, monospace, bold, italic, bold_italic };

// One positioned glyph. Positions are physical pixels relative to the start of the baseline.
struct ShapedGlyph {
    std::uint32_t font;     // FontSystem font id
    std::uint32_t glyph;    // glyph index in that font
    std::uint32_t cluster;  // byte offset of the source text this glyph comes from
    float x;
    float y;
    float advance;
};

// A single line of shaped text, in physical pixels.
struct TextLayout {
    std::vector<ShapedGlyph> glyphs;
    float pixel_size{0.0F};
    float width{0.0F};
    float ascent{0.0F};   // above the baseline, positive
    float descent{0.0F};  // below the baseline, positive
    bool truncated{false};
};

// Raster of one glyph for the atlas: R8 coverage, or RGBA8 premultiplied for color glyphs
// (emoji). left/top place the bitmap relative to the glyph origin on the baseline (top is
// positive upward, FreeType's convention).
struct GlyphBitmap {
    std::int32_t width{0};
    std::int32_t height{0};
    std::int32_t left{0};
    std::int32_t top{0};
    bool color{false};
    bool subpixel{false};  // outline accepts fractional placement; bitmap/color glyphs do not
    std::vector<std::uint8_t> pixels;
};

// Font discovery (fontconfig), shaping (harfbuzz) and rasterization (FreeType).
//
// Text is split into runs by font coverage: each character uses the first font of the
// fontconfig fallback chain for the family that has it, so emoji and CJK work inside a Latin
// name. Invalid UTF-8 (possible in Linux file names) becomes U+FFFD. Bidirectional text is not
// reordered yet: right-to-left scripts render in logical order.
//
// Fallback chains: family for the interface, monospace for code, and bold/italic variants
// for rendered text (looked up on first use). A font in multiple chains is loaded once.
//
// Not thread-safe; owned by the UI thread. Throws std::runtime_error if no font can be loaded.
class FontSystem {
public:
    explicit FontSystem(const std::string& family = "sans-serif",
                        std::string monospace = "monospace");
    ~FontSystem();

    FontSystem(const FontSystem&) = delete;
    FontSystem& operator=(const FontSystem&) = delete;
    FontSystem(FontSystem&&) = delete;
    FontSystem& operator=(FontSystem&&) = delete;

    // Shapes utf8 at size (logical pixels) times scale. With max_width > 0 (logical pixels) a
    // wider text is truncated at whole characters with an ellipsis. Results are cached; the
    // reference stays valid until the next layout() call.
    const TextLayout& layout(std::string_view utf8, float size, float scale, float max_width = 0.0F,
                             Ellipsis ellipsis = Ellipsis::end,
                             Typeface typeface = Typeface::proportional);

    // Full-line geometry for one-off measurements. Does not retain the text or glyphs in
    // the drawing cache, or invalidate references returned by layout().
    [[nodiscard]] TextLayout layout_uncached(std::string_view utf8, float size, float scale,
                                             Typeface typeface = Typeface::proportional);

    // Loads a font file outside the fontconfig fallback chain (an icon font): text never falls
    // back to it. Returns its font id, or nullopt when the file cannot be loaded.
    std::optional<std::uint32_t> add_font_file(const std::string& path);

    // The same for a font in memory (compiled into the program), named name in the log. data
    // must outlive the FontSystem.
    std::optional<std::uint32_t> add_font_data(std::span<const unsigned char> data,
                                               const std::string& name);

    // Index of a named glyph (TrueType post table names, e.g. Tabler's "folder"); 0 if absent.
    [[nodiscard]] std::uint32_t glyph_by_name(std::uint32_t font, std::string_view name);

    // One glyph centered in a size x size box (logical pixels), for draw_layout at the box's
    // top-left: icons.
    [[nodiscard]] TextLayout glyph_layout(std::uint32_t font, std::uint32_t glyph, float size,
                                          float scale);

    // Renders one glyph at pixel_size (physical pixels). Outlines move right by x_offset
    // (0-1 px); bitmap/color glyphs ignore it and return subpixel=false so the caller can
    // round the original pen position. Hinting and antialiasing follow fontconfig.
    [[nodiscard]] GlyphBitmap rasterize(std::uint32_t font, std::uint32_t glyph, float pixel_size,
                                        float x_offset = 0.0F);

    // Whether glyphs may sit between pixels: not when fontconfig asks for full hinting or no
    // antialiasing, whose glyphs are made for whole pixels (Pango and Qt place them so too).
    [[nodiscard]] bool subpixel_positioning() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rummage::ui
