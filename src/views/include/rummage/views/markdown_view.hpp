#pragma once

#include "rummage/text/markdown.hpp"
#include "rummage/theme/palette.hpp"
#include "rummage/ui/renderer.hpp"
#include "rummage/ui/seat.hpp"
#include "rummage/views/selectable_text.hpp"
#include "rummage/views/text_selection.hpp"

namespace rummage::views {

// Native presentation of the helper's bounded document. Layout never parses markup or loads
// resources. Source ranges remain attached to wrapped pieces for selection and copying.
class MarkdownView {
public:
    void reset();
    float layout(ui::Renderer& renderer, const text::Markdown& document, float width);
    void draw(ui::Renderer& renderer, ui::Rect viewport, float offset,
              const theme::Palette& palette, float fade);
    bool pointer(const ui::PointerEvent& event);
    void select_all();
    [[nodiscard]] std::string selected_text() const;

    [[nodiscard]] bool truncated() const { return truncated_; }

    [[nodiscard]] float height() const { return height_; }

    [[nodiscard]] std::size_t piece_count() const { return pieces_.size(); }

private:
    struct Piece {
        std::size_t block, begin, end;
        ui::Rect rect;
        float size;
        ui::Typeface face;
        std::uint16_t style;
        std::vector<SelectableText::GlyphSpan> glyphs;
    };

    struct Decoration {
        text::MarkdownKind kind;
        ui::Rect rect;
    };

    const text::Markdown* document_{nullptr};
    float width_{0}, scale_{0}, height_{0};
    std::vector<std::string> lines_;
    std::vector<Piece> pieces_;
    std::vector<Decoration> decorations_;
    TextSelection selection_;
    bool dragging_{false};
    bool truncated_{false};
    ui::Rect viewport_;
    float offset_{0};
    [[nodiscard]] TextPoint point(float x, float y) const;
};
}  // namespace rummage::views
