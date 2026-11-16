#include <doctest/doctest.h>

#include "rummage/formats/text/syntax.hpp"

namespace {
rummage::text::Text parse(std::vector<std::string> lines) {
    rummage::text::Text text;
    text.lines = std::move(lines);
    rummage::formats::text::colour(text, "README.md", "text/markdown");
    return text;
}
}  // namespace

#include <cstdlib>
#include <fstream>

#include "rummage/ui/offscreen.hpp"
#include "rummage/views/markdown_view.hpp"

TEST_CASE("markdown: rendered layout wraps at narrow widths and copies presentation text") {
    rummage::ui::Offscreen gl{960, 720};
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    auto source = parse({"# Project notes",
                         "",
                         "A **clear heading** and *emphasis*, with a [styled "
                         "link](https://example.com) and `inline code`.",
                         "",
                         "## Ready to ship",
                         "- [x] Preview Markdown without opening an editor",
                         "- [ ] Review the release notes",
                         "",
                         "> File previews stay inside the existing sandbox. Links and images never "
                         "fetch resources.",
                         "",
                         "| Feature | Status |",
                         "| --- | --- |",
                         "| Tables | **Ready** |",
                         "| Narrow windows | Wrap text |",
                         "",
                         "```cpp",
                         "int main() {",
                         "    return 42;",
                         "}",
                         "```",
                         "",
                         "---",
                         "Images remain placeholders: ![Architecture](diagram.png)"});
    rummage::views::MarkdownView view;
    renderer.begin_frame(960, 720, 1, {});
    const float wide = view.layout(renderer, source.markdown, 800);
    CHECK(wide > 200);
    CHECK(view.piece_count() > 20);
    const float narrow = view.layout(renderer, source.markdown, 220);
    CHECK(narrow > wide);
    CHECK_FALSE(view.truncated());
    view.layout(renderer, source.markdown, 800);
    const rummage::theme::Palette palette;
    renderer.fill_rect({0, 0, 960, 720}, palette.surface_container);
    view.draw(renderer, {60, 30, 800, 660}, 0, palette, 1);
    renderer.end_frame();
    view.select_all();
    const auto selected = view.selected_text();
    CHECK(selected.find("Project notes") != std::string::npos);
    CHECK(selected.find("**") == std::string::npos);
    CHECK(selected.find("https://") == std::string::npos);
    CHECK(selected.find("return 42;") != std::string::npos);
    // Optional review artifact from the exact renderer exercised above.
    if (const char* path = std::getenv("RUMMAGE_MARKDOWN_SNAPSHOT")) {
        std::vector<unsigned char> pixels(960 * 720 * 4);
        glReadPixels(0, 0, 960, 720, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        std::ofstream out{path, std::ios::binary};
        out << "P6\n960 720\n255\n";
        for (std::size_t y = 720; y-- > 0;)
            for (std::size_t x = 0; x < 960; ++x)
                out.write(reinterpret_cast<const char*>(pixels.data() + (y * 960 + x) * 4), 3);
    }
}
