#pragma once

#include <algorithm>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "rummage/ui/renderer.hpp"
#include "rummage/ui/seat.hpp"
#include "rummage/views/text_selection.hpp"

namespace rummage::views {

// Read-only, wrapping text with ordinary text selection and no input-field decoration.
// Soft wraps retain source byte offsets: copying never inserts presentation line breaks.
class SelectableText {
public:
    struct GlyphSpan {
        std::size_t begin, end;
        float left, right;
        bool rtl;
    };

    // Source ranges and visual advance bounds from the same shaping used for drawing.
    // Keeping these separate also handles reverse-order Hebrew and Arabic clusters.
    static std::vector<GlyphSpan> glyph_spans(ui::FontSystem& fonts, std::string_view value,
                                              float size, float scale,
                                              ui::Typeface typeface = ui::Typeface::proportional) {
        const auto& shaped = fonts.layout(value, size, scale, 0, ui::Ellipsis::end, typeface);
        return glyph_spans(value, shaped, scale);
    }

    // Reuse an existing layout when measuring a preview: offscreen lines need selection
    // geometry, but need not also occupy the font system's drawing cache.
    static std::vector<GlyphSpan> glyph_spans(std::string_view value, const ui::TextLayout& shaped,
                                              float scale) {
        struct Cluster {
            GlyphSpan span;
            std::uint32_t font;
        };

        std::vector<Cluster> clusters;
        clusters.reserve(shaped.glyphs.size());
        float pen = 0;
        for (const auto& glyph : shaped.glyphs) {
            const float end = pen + glyph.advance / scale;
            if (!clusters.empty() && clusters.back().span.begin == glyph.cluster) {
                clusters.back().span.left = std::min(clusters.back().span.left, std::min(pen, end));
                clusters.back().span.right =
                    std::max(clusters.back().span.right, std::max(pen, end));
            } else
                clusters.push_back(
                    {{glyph.cluster, 0, std::min(pen, end), std::max(pen, end), false},
                     glyph.font});
            pen = end;
        }
        for (std::size_t index = 0; index < clusters.size(); ++index) {
            auto& cluster = clusters[index];
            const bool previous = index > 0 && clusters[index - 1].font == cluster.font;
            const bool next =
                index + 1 < clusters.size() && clusters[index + 1].font == cluster.font;
            cluster.span.rtl = (previous && clusters[index - 1].span.begin > cluster.span.begin) ||
                               (next && clusters[index + 1].span.begin < cluster.span.begin);
            if (!previous && !next) {
                // A one-cluster run has no neighbour to reveal its direction.
                const auto first = static_cast<unsigned char>(value[cluster.span.begin]);
                char32_t cp = first;
                const auto bytes = first < 0x80 ? 1U : first < 0xE0 ? 2U : first < 0xF0 ? 3U : 4U;
                if (bytes > 1 && cluster.span.begin + bytes <= value.size()) {
                    cp = first & (0x7FU >> bytes);
                    for (unsigned offset = 1; offset < bytes; ++offset)
                        cp = (cp << 6U) |
                             (static_cast<unsigned char>(value[cluster.span.begin + offset]) &
                              0x3FU);
                }
                cluster.span.rtl =
                    (cp >= 0x0590 && cp <= 0x08FF) || (cp >= 0xFB1D && cp <= 0xFDFF) ||
                    (cp >= 0xFE70 && cp <= 0xFEFF) || (cp >= 0x10800 && cp <= 0x10FFF) ||
                    (cp >= 0x1E800 && cp <= 0x1EEFF);
            }
        }
        std::ranges::sort(clusters, {}, [](const Cluster& cluster) { return cluster.span.begin; });
        std::vector<GlyphSpan> result;
        result.reserve(clusters.size());
        for (std::size_t index = 0; index < clusters.size(); ++index) {
            auto span = clusters[index].span;
            span.end = index + 1 < clusters.size() ? clusters[index + 1].span.begin : value.size();
            result.push_back(span);
        }
        return result;
    }

    void set_copy_handler(std::function<void(std::string)> handler) { copy_ = std::move(handler); }

    void begin_frame(ui::Rect clip) {
        clip_ = clip;
        fields_.clear();
    }

    void end_frame() {
        if (focused() && !active_field())
            clear_focus();
    }

    void clear_focus() {
        active_.clear();
        active_lines_.clear();
        selection_.clear();
        dragging_ = false;
    }

    void reset() {
        clear_focus();
        fields_.clear();
    }

    [[nodiscard]] bool focused() const { return !active_.empty(); }

    [[nodiscard]] std::string selected_text() const { return selection_.text(active_lines_); }

    [[nodiscard]] ui::Rect bounds(std::string_view id) const {
        for (const auto& field : fields_)
            if (field.id == id)
                return field.rect;
        return {};
    }

    void select_all() {
        if (focused())
            selection_.select_all(active_lines_);
    }

    bool key(const ui::KeyEvent& event) {
        if (!focused() || !event.modifiers.ctrl || event.modifiers.alt || event.modifiers.super)
            return false;
        if (event.keysym == XKB_KEY_a || event.keysym == XKB_KEY_A) {
            select_all();
            return true;
        }
        if (event.keysym == XKB_KEY_c || event.keysym == XKB_KEY_C) {
            if (copy_ && !selection_.empty())
                copy_(selected_text());
            return true;
        }
        return false;
    }

    bool pointer(const ui::PointerEvent& event) {
        if (event.kind == ui::PointerEvent::Kind::leave) {
            const bool was_dragging = dragging_;
            dragging_ = false;
            return was_dragging;
        }
        if (event.kind == ui::PointerEvent::Kind::press &&
            event.button == ui::PointerButton::left) {
            for (const auto& field : fields_) {
                if (!field.rect.intersect(clip_).contains(event.x, event.y))
                    continue;
                const bool same_field = active_ == field.id;
                if (!same_field) {
                    selection_.clear();
                    active_ = field.id;
                    active_lines_ = field.lines;
                }
                selection_.press(point(field, event.x, event.y), event.click_count,
                                 event.modifiers.shift && same_field, active_lines_);
                dragging_ = true;
                return true;
            }
            clear_focus();
        } else if (event.kind == ui::PointerEvent::Kind::motion && dragging_) {
            if (const auto* field = active_field())
                selection_.drag(point(*field, event.x, event.y), active_lines_);
            return true;
        } else if (event.kind == ui::PointerEvent::Kind::release &&
                   event.button == ui::PointerButton::left && dragging_) {
            if (const auto* field = active_field())
                selection_.drag(point(*field, event.x, event.y), active_lines_);
            dragging_ = false;
            return true;
        }
        return false;
    }

    [[nodiscard]] ui::CursorShape cursor_at(float x, float y) const {
        if (dragging_)
            return ui::CursorShape::text;
        for (const auto& field : fields_)
            if (field.rect.intersect(clip_).contains(x, y))
                return ui::CursorShape::text;
        return ui::CursorShape::standard;
    }

    static float height(ui::Renderer& renderer, std::string_view value, float width, float size,
                        float line_height = 0, ui::Typeface typeface = ui::Typeface::proportional) {
        return layout(renderer, value, width, size, line_height, typeface).rect.height;
    }

    float draw(ui::Renderer& renderer, std::string id, std::string_view value, ui::Rect rect,
               float size, ui::Color color, ui::Color selection, float line_height = 0,
               ui::Typeface typeface = ui::Typeface::proportional) {
        auto field = layout(renderer, value, rect.width, size, line_height, typeface);
        field.id = std::move(id);
        field.rect.x = rect.x;
        field.rect.y = rect.y;
        if (field.id == active_ && field.lines != active_lines_) {
            active_lines_ = field.lines;
            selection_.clear();
            dragging_ = false;
        }
        const auto [first, last] = selection_.range();
        renderer.push_clip(field.rect.intersect(clip_));
        for (std::size_t row = 0; row < field.visual.size(); ++row) {
            const auto& visual = field.visual[row];
            const float y = rect.y + static_cast<float>(row) * field.line_height;
            if (field.id == active_ && !selection_.empty() && first.line <= visual.line &&
                last.line >= visual.line) {
                const auto from = first.line == visual.line ? first.byte : 0;
                const auto to =
                    last.line == visual.line ? last.byte : field.lines[visual.line].size();
                for (const auto& span : visual.spans)
                    if (to > span.begin && from < span.end)
                        renderer.fill_rect(
                            {rect.x + span.left, y, span.right - span.left, field.line_height},
                            selection);
            }
            renderer.draw_text(std::string_view{field.lines[visual.line]}.substr(
                                   visual.begin, visual.end - visual.begin),
                               rect.x, y, size, color, 0, ui::Ellipsis::end, typeface);
        }
        renderer.pop_clip();
        const float result = field.rect.height;
        fields_.push_back(std::move(field));
        return result;
    }

private:
    struct Caret {
        std::size_t byte;
        float x;
    };

    struct Visual {
        std::size_t line, begin, end;
        std::vector<Caret> carets;
        std::vector<GlyphSpan> spans;
    };

    struct Field {
        std::string id;
        std::vector<std::string> lines;
        std::vector<Visual> visual;
        ui::Rect rect;
        float line_height;
    };

    static std::vector<Caret> carets(ui::Renderer& renderer, std::string_view value, float size,
                                     ui::Typeface typeface) {
        const auto spans = glyph_spans(renderer.fonts(), value, size, renderer.scale(), typeface);
        std::vector<Caret> result{{0, 0}};
        float pen = 0;
        for (const auto& span : spans) {
            if (span.begin > result.back().byte)
                result.push_back({span.begin, pen});
            pen += span.right - span.left;
        }
        if (result.back().byte != value.size())
            result.push_back({value.size(), pen});
        return result;
    }

    static Field layout(ui::Renderer& renderer, std::string_view value, float width, float size,
                        float line_height, ui::Typeface typeface) {
        Field field;
        field.line_height = line_height > 0 ? line_height : size + 6;
        std::size_t start = 0;
        do {
            const auto newline = value.find('\n', start);
            field.lines.emplace_back(
                value.substr(start, newline == std::string_view::npos ? newline : newline - start));
            if (newline == std::string_view::npos)
                break;
            start = newline + 1;
        } while (start <= value.size());
        for (std::size_t line = 0; line < field.lines.size(); ++line) {
            const auto& source = field.lines[line];
            const auto positions = carets(renderer, source, size, typeface);
            std::size_t begin = 0;
            do {
                std::size_t end = begin + 1;
                // Accumulating cluster advances can differ slightly from the full layout's
                // measured width at fractional scales. An exact-fit label must stay on one line.
                constexpr float fit_tolerance = 0.01F;
                while (end + 1 < positions.size() && positions[end + 1].x - positions[begin].x <=
                                                         std::max(1.0F, width) + fit_tolerance)
                    ++end;
                end = std::min(end, positions.size() - 1);
                if (end + 1 < positions.size()) {
                    // Prefer ordinary word/path boundaries; retain their bytes in the preceding
                    // line so soft wrapping never changes copied whitespace or punctuation.
                    std::size_t boundary = begin;
                    for (std::size_t index = begin + 1; index <= end; ++index) {
                        const char previous = source[positions[index].byte - 1];
                        if (previous == ' ' || previous == '\t' || previous == '-' ||
                            previous == '/')
                            boundary = index;
                    }
                    if (boundary > begin)
                        end = boundary;
                }
                const auto first_byte = positions[begin].byte;
                const auto last_byte = positions[end].byte;
                auto spans =
                    glyph_spans(renderer.fonts(),
                                std::string_view{source}.substr(first_byte, last_byte - first_byte),
                                size, renderer.scale(), typeface);
                std::vector<Caret> row_carets;
                for (auto& span : spans) {
                    span.begin += first_byte;
                    span.end += first_byte;
                    row_carets.push_back({span.begin, span.rtl ? span.right : span.left});
                    row_carets.push_back({span.end, span.rtl ? span.left : span.right});
                }
                if (row_carets.empty())
                    row_carets.push_back({first_byte, 0});
                std::ranges::sort(row_carets, {}, &Caret::x);
                field.visual.push_back(
                    {line, first_byte, last_byte, std::move(row_carets), std::move(spans)});
                begin = end;
            } while (begin + 1 < positions.size());
        }
        field.rect = {0, 0, width, static_cast<float>(field.visual.size()) * field.line_height};
        return field;
    }

    static TextPoint point(const Field& field, float x, float y) {
        const auto row =
            static_cast<std::size_t>(std::clamp((y - field.rect.y) / field.line_height, 0.0F,
                                                static_cast<float>(field.visual.size() - 1)));
        const auto& visual = field.visual[row];
        const float local = x - field.rect.x;
        for (std::size_t index = 1; index < visual.carets.size(); ++index)
            if (local < (visual.carets[index - 1].x + visual.carets[index].x) / 2)
                return {visual.line, visual.carets[index - 1].byte};
        return {visual.line, visual.carets.back().byte};
    }

    [[nodiscard]] const Field* active_field() const {
        for (const auto& field : fields_)
            if (field.id == active_)
                return &field;
        return nullptr;
    }

    ui::Rect clip_;
    std::vector<Field> fields_;
    std::string active_;
    std::vector<std::string> active_lines_;
    TextSelection selection_;
    std::function<void(std::string)> copy_;
    bool dragging_{false};
};

}  // namespace rummage::views
