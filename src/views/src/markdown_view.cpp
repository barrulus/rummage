#include "rummage/views/markdown_view.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "rummage/theme/syntax.hpp"

namespace rummage::views {
namespace {
using Kind = text::MarkdownKind;

ui::Typeface face_of(std::uint16_t style) {
    if ((style & text::md_code) != 0)
        return ui::Typeface::monospace;
    if ((style & (text::md_bold | text::md_italic)) == (text::md_bold | text::md_italic))
        return ui::Typeface::bold_italic;
    if ((style & text::md_bold) != 0)
        return ui::Typeface::bold;
    return (style & text::md_italic) != 0 ? ui::Typeface::italic : ui::Typeface::proportional;
}
}  // namespace

void MarkdownView::reset() {
    document_ = nullptr;
    lines_.clear();
    pieces_.clear();
    decorations_.clear();
    selection_.clear();
    dragging_ = false;
    height_ = 0;
    truncated_ = false;
}

float MarkdownView::layout(ui::Renderer& renderer, const text::Markdown& document, float width) {
    width = std::max(1.0F, width);
    if (document_ == &document && width_ == width && scale_ == renderer.scale())
        return height_;
    if (document_ != &document) {
        selection_.clear();
        dragging_ = false;
    }
    document_ = &document;
    width_ = width;
    scale_ = renderer.scale();
    pieces_.clear();
    decorations_.clear();
    lines_.clear();
    height_ = 0;
    truncated_ = false;
    // Keep even hostile narrow layouts bounded; normal documents stay far below this.
    constexpr std::size_t max_pieces = 65536;
    for (std::size_t index = 0; index < document.blocks.size(); ++index) {
        const auto& block = document.blocks[index];
        lines_.emplace_back();
        auto& line = lines_.back();
        if (block.kind == Kind::gap) {
            height_ += 8;
            continue;
        }
        if (block.kind == Kind::rule) {
            decorations_.push_back({block.kind, {0, height_ + 7, width, 1}});
            height_ += 16;
            continue;
        }
        const bool heading = block.kind == Kind::heading;
        const bool code = block.kind == Kind::code;
        const bool table = block.kind == Kind::table_header || block.kind == Kind::table_row;
        const float size = heading ? std::max(15.0F, 29.0F - static_cast<float>(block.level) * 2.5F)
                           : code  ? 13.0F
                                   : 15.0F;
        const float row_height = size * 1.5F;
        const float top = height_ + (heading ? 10.0F : 0.0F);
        const float indent = block.kind == Kind::quote ? 18.0F
                             : block.kind == Kind::list
                                 ? 24.0F + static_cast<float>(block.level) * 14.0F
                             : code ? 10.0F
                                    : 0.0F;
        const auto cells =
            table && !block.runs.empty() ? std::size_t{block.runs.back().cell} + 1 : 1;
        const float cell_width = width / static_cast<float>(cells);
        float bottom = top + row_height;
        std::size_t run_index = 0;
        if (block.kind == Kind::list) {
            line = block.marker + " ";
            const auto glyphs =
                SelectableText::glyph_spans(renderer.fonts(), block.marker, size, scale_);
            pieces_.push_back({index,
                               0,
                               block.marker.size(),
                               {std::min(indent - 22, width - 1), top, 22, row_height},
                               size,
                               ui::Typeface::proportional,
                               0,
                               glyphs});
        }
        for (std::size_t cell = 0; cell < cells; ++cell) {
            if (cell > 0)
                line += " | ";
            const float left =
                std::min(width - 1, static_cast<float>(cell) * cell_width + (table ? 8 : indent));
            const float right = std::max(
                left + 1, static_cast<float>(cell + 1) * cell_width - (table || code ? 8 : 0));
            float x = left;
            float y = top;
            for (; run_index < block.runs.size() && block.runs[run_index].cell == cell;
                 ++run_index) {
                const auto& run = block.runs[run_index];
                const std::size_t run_begin = line.size();
                line += run.text;
                const auto style =
                    static_cast<std::uint16_t>(run.style | (heading ? text::md_bold : 0));
                const auto face = face_of(style);
                // Shape words once. Very long words split at shaped glyph clusters, never bytes.
                for (std::size_t start = 0; start < run.text.size();) {
                    std::size_t end = run.text.find(' ', start);
                    if (end == std::string::npos)
                        end = run.text.size();
                    else
                        ++end;
                    const std::string_view token =
                        std::string_view{run.text}.substr(start, end - start);
                    auto glyphs =
                        SelectableText::glyph_spans(renderer.fonts(), token, size, scale_, face);
                    const float token_width =
                        renderer.fonts()
                            .layout(token, size, scale_, 0, ui::Ellipsis::end, face)
                            .width /
                        scale_;
                    if (x > left && x + token_width > right) {
                        y += row_height;
                        x = left;
                    }
                    std::size_t first = 0;
                    while (first < glyphs.size()) {
                        if (pieces_.size() >= max_pieces) {
                            truncated_ = true;
                            lines_.back().resize(run_begin + start + glyphs[first].begin);
                            return height_ = std::max(bottom, y) + row_height;
                        }
                        std::size_t last = first;
                        float advance = 0;
                        while (last < glyphs.size()) {
                            const float next = glyphs[last].right - glyphs[last].left;
                            if (last > first && advance + next > right - x)
                                break;
                            advance += next;
                            ++last;
                        }
                        const auto begin_byte = glyphs[first].begin;
                        const auto end_byte = glyphs[last - 1].end;
                        const auto part = token.substr(begin_byte, end_byte - begin_byte);
                        auto positions =
                            SelectableText::glyph_spans(renderer.fonts(), part, size, scale_, face);
                        const float measured =
                            renderer.fonts()
                                .layout(part, size, scale_, 0, ui::Ellipsis::end, face)
                                .width /
                            scale_;
                        pieces_.push_back({index,
                                           run_begin + start + begin_byte,
                                           run_begin + start + end_byte,
                                           {x, y, measured, row_height},
                                           size,
                                           face,
                                           style,
                                           std::move(positions)});
                        x += measured;
                        first = last;
                        if (first < glyphs.size()) {
                            x = left;
                            y += row_height;
                        }
                    }
                    start = end;
                }
            }
            bottom = std::max(bottom, y + row_height);
        }
        if (code || table || block.kind == Kind::quote)
            decorations_.push_back({block.kind, {0, top - 2, width, bottom - top + 4}});
        height_ = bottom + (code || table ? 2.0F : 6.0F);
    }
    return height_;
}

void MarkdownView::draw(ui::Renderer& renderer, ui::Rect viewport, float offset,
                        const theme::Palette& palette, float fade) {
    viewport_ = viewport;
    offset_ = offset;
    renderer.push_clip(viewport);
    const auto place = [&](ui::Rect rect) {
        rect.x += viewport.x;
        rect.y += viewport.y - offset;
        return rect;
    };
    for (const auto& decoration : decorations_) {
        auto rect = place(decoration.rect);
        if (rect.bottom() < viewport.y || rect.y > viewport.bottom())
            continue;
        if (decoration.kind == Kind::quote) {
            rect.width = 3;
            renderer.fill_rect(rect, palette.primary.with_alpha(0.65F * fade));
        } else if (decoration.kind == Kind::rule)
            renderer.fill_rect(rect, palette.on_surface_variant.with_alpha(0.3F * fade));
        else {
            renderer.fill_rect(rect, palette.surface_container_high.with_alpha(fade));
            if (decoration.kind == Kind::table_header || decoration.kind == Kind::table_row)
                renderer.fill_rect({rect.x, rect.bottom() - 1, rect.width, 1},
                                   palette.on_surface_variant.with_alpha(0.2F * fade));
        }
    }
    const auto colours = theme::syntax_colors(palette);
    const auto [from, to] = selection_.range();
    for (const auto& piece : pieces_) {
        const auto rect = place(piece.rect);
        if (rect.bottom() < viewport.y || rect.y > viewport.bottom())
            continue;
        if ((piece.style & text::md_code) != 0)
            renderer.fill_rect(rect, palette.surface_container_high.with_alpha(fade));
        if (!selection_.empty() && from.line <= piece.block && to.line >= piece.block) {
            const auto begin = from.line == piece.block ? from.byte : 0;
            const auto end = to.line == piece.block ? to.byte : lines_[piece.block].size();
            for (const auto& glyph : piece.glyphs)
                if (end > piece.begin + glyph.begin && begin < piece.begin + glyph.end)
                    renderer.fill_rect(
                        {rect.x + glyph.left, rect.y, glyph.right - glyph.left, rect.height},
                        palette.secondary_container.with_alpha(fade));
        }
        const auto kind = document_->blocks[piece.block].kind;
        const bool raised = (piece.style & text::md_code) != 0 || kind == Kind::code ||
                            kind == Kind::table_header || kind == Kind::table_row;
        const auto background = raised ? palette.surface_container_high : palette.surface_container;
        auto color = (piece.style & text::md_link) != 0
                         ? theme::readable_foreground(palette.primary, background)
                         : palette.on_surface;
        const unsigned syntax = piece.style >> 8U;
        if (syntax > 0 && syntax <= theme::kSyntaxColors)
            color = colours.at(syntax - 1);
        renderer.draw_text(
            std::string_view{lines_[piece.block]}.substr(piece.begin, piece.end - piece.begin),
            rect.x, rect.y, piece.size, color.with_alpha(fade), 0, ui::Ellipsis::end, piece.face);
        if ((piece.style & text::md_link) != 0)
            renderer.fill_rect({rect.x, rect.y + piece.size * 1.2F, rect.width, 1},
                               color.with_alpha(0.5F * fade));
    }
    renderer.pop_clip();
}

TextPoint MarkdownView::point(float x, float y) const {
    x -= viewport_.x;
    y += offset_ - viewport_.y;
    const Piece* nearest = nullptr;
    float distance = std::numeric_limits<float>::max();
    for (const auto& piece : pieces_) {
        const float dy = std::max({piece.rect.y - y, y - piece.rect.bottom(), 0.0F});
        const float dx = std::max({piece.rect.x - x, x - piece.rect.right(), 0.0F});
        const float score = dy * 10000 + dx;
        if (score < distance) {
            distance = score;
            nearest = &piece;
        }
    }
    if (!nearest)
        return {};
    const float local = x - nearest->rect.x;
    for (const auto& glyph : nearest->glyphs)
        if (local < (glyph.left + glyph.right) / 2)
            return {nearest->block, nearest->begin + (glyph.rtl ? glyph.end : glyph.begin)};
    return {nearest->block, nearest->end};
}

bool MarkdownView::pointer(const ui::PointerEvent& event) {
    using Kind = ui::PointerEvent::Kind;
    if (event.kind == Kind::press && event.button == ui::PointerButton::left &&
        viewport_.contains(event.x, event.y)) {
        selection_.press(point(event.x, event.y), event.click_count, event.modifiers.shift, lines_);
        dragging_ = true;
        return true;
    }
    if (event.kind == Kind::motion && dragging_) {
        selection_.drag(point(event.x, event.y), lines_);
        return true;
    }
    if (event.kind == Kind::release && event.button == ui::PointerButton::left) {
        const bool handled = dragging_;
        dragging_ = false;
        return handled;
    }
    if (event.kind == Kind::leave)
        dragging_ = false;
    return false;
}

void MarkdownView::select_all() { selection_.select_all(lines_); }

std::string MarkdownView::selected_text() const { return selection_.text(lines_); }
}  // namespace rummage::views
