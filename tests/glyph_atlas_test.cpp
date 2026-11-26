#include <GLES3/gl3.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <doctest/doctest.h>

#include "rummage/ui/offscreen.hpp"
#include "rummage/ui/renderer.hpp"
#include "rummage/ui/text.hpp"

namespace ui = rummage::ui;

TEST_CASE("renderer: glyph atlases allocate on use and preserve draws across growth") {
    constexpr std::size_t initial_bytes = 1024 * 1024;
    constexpr std::size_t grown_bytes = 2048 * 2048;
    const auto baseline = ui::Renderer::atlas_bytes();
    for (const float scale : {1.0F, 2.0F}) {
        const int width = static_cast<int>(300 * scale);
        const int height = static_cast<int>(60 * scale);
        ui::Offscreen gl{width, height};
        ui::FontSystem fonts;
        {
            ui::Renderer renderer{fonts};
            CHECK(ui::Renderer::atlas_bytes() == baseline);
            const auto capture = [&] {
                std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width * height * 4));
                glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
                return pixels;
            };
            renderer.begin_frame(width, height, scale, ui::Color::rgb(0));
            renderer.draw_text("Atlas growth", 4, 4, 24, ui::Color::rgb(0xFFFFFF));
            renderer.end_frame();
            const auto before = capture();
            CHECK(std::ranges::count(before, std::uint8_t{255}) > width * height);
            CHECK(ui::Renderer::atlas_bytes() == baseline + initial_bytes);

            renderer.begin_frame(width, height, scale, ui::Color::rgb(0));
            // Keep this draw queued when another glyph forces growth: it must be flushed
            // against the old atlas before storage is replaced and normalized UVs discarded.
            renderer.draw_text("Atlas growth", 4, 4, 24, ui::Color::rgb(0xFFFFFF));
            for (int size = 25; size < 180 && ui::Renderer::atlas_bytes() < baseline + grown_bytes;
                 ++size) {
                renderer.draw_text("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
                                   0, 1000, static_cast<float>(size), ui::Color::rgb(0xFFFFFF));
            }
            renderer.end_frame();
            CHECK(ui::Renderer::atlas_bytes() == baseline + grown_bytes);
            CHECK(capture() == before);
            // Cached text from before the resize is correctly rasterized with the new UVs.
            renderer.begin_frame(width, height, scale, ui::Color::rgb(0));
            renderer.draw_text("Atlas growth", 4, 4, 24, ui::Color::rgb(0xFFFFFF));
            renderer.end_frame();
            CHECK(capture() == before);
            CHECK(glGetError() == GL_NO_ERROR);
        }
        CHECK(ui::Renderer::atlas_bytes() == baseline);
    }
}

TEST_CASE("renderer: the color glyph atlas remains absent until a color glyph is drawn") {
    ui::Offscreen gl{180, 80};
    ui::FontSystem fonts{"Noto Color Emoji"};
    const auto layout = fonts.layout_uncached("😀", 36, 1);
    bool has_color = false;
    for (const auto& glyph : layout.glyphs)
        has_color = has_color || fonts.rasterize(glyph.font, glyph.glyph, layout.pixel_size).color;
    if (!has_color) {
        MESSAGE("Color-atlas check requires a color emoji font; no color glyph available");
        return;
    }
    const auto baseline = ui::Renderer::atlas_bytes();
    {
        ui::Renderer renderer{fonts};
        CHECK(ui::Renderer::atlas_bytes() == baseline);
        renderer.begin_frame(180, 80, 1, ui::Color::rgb(0));
        renderer.draw_layout(layout, 4, 4, ui::Color::rgb(0xFFFFFF));
        renderer.end_frame();
        CHECK(ui::Renderer::atlas_bytes() == baseline + 2048 * 2048 * 4);
        std::vector<std::uint8_t> pixels(180 * 80 * 4);
        glReadPixels(0, 0, 180, 80, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        std::size_t colored = 0;
        for (std::size_t i = 0; i < pixels.size(); i += 4)
            colored += pixels[i] != pixels[i + 1] || pixels[i + 1] != pixels[i + 2] ? 1U : 0U;
        CHECK(colored > 50);
        CHECK(glGetError() == GL_NO_ERROR);
    }
    CHECK(ui::Renderer::atlas_bytes() == baseline);
}
