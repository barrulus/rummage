#include "rummage/views/info_pane.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "draw_helpers.hpp"
#include "rummage/core/i18n.hpp"
#include "rummage/fs/icon.hpp"
#include "rummage/views/format.hpp"
#include "rummage/views/media_details.hpp"

namespace rummage::views {
namespace {

constexpr float kPadding = 20.0F;     // either side of the content
constexpr float kMaxColumn = 360.0F;  // the content is at most this wide, centered
constexpr float kTop = 24.0F;
constexpr float kIconSize = 64.0F;
constexpr float kMaxPreview = 0.5F;  // of the pane's height, for a thumbnail
constexpr float kRowHeight = 22.0F;
constexpr float kTextSize = 13.0F;
constexpr float kSmallSize = 12.0F;
constexpr float kSectionSize = 11.0F;
constexpr float kLineSize = 11.0F;  // a text file's lines
constexpr float kLineHeight = InfoPane::kTextLineHeight;
constexpr float kPagePadding = 8.0F;
constexpr std::size_t kPageLines = 30;  // at most, under the name; more scroll
constexpr float kScrollStep = 3.0F;     // lines, for Alt+J and Alt+K
constexpr float kScrollbarWidth = 4.0F;
constexpr float kScrollbarMinThumb = 24.0F;

// "1 file", "3 files".
std::string count_of(std::size_t count, std::string_view noun) {
    if (noun == "folder") {
        return count == 1 ? std::string{i18n::tr("1 folder")} : i18n::trf("{} folders", count);
    }
    if (noun == "file") {
        return count == 1 ? std::string{i18n::tr("1 file")} : i18n::trf("{} files", count);
    }
    return count == 1 ? std::string{i18n::tr("1 item")} : i18n::trf("{} items", count);
}

// "1/2000 s · F5.6 · ISO 200", leaving out what is missing.
std::string exposure_of(const picture::PhotoInfo& photo) {
    std::string out;
    const auto add = [&](const std::string& part) {
        if (!part.empty()) {
            out += (out.empty() ? "" : " · ") + part;
        }
    };
    add(photo.exposure_time);
    add(photo.aperture);
    add(photo.iso.empty() ? photo.iso : "ISO " + photo.iso);
    return out;
}

std::string rating_of(int rating) {
    if (rating < 0) {
        return std::string{i18n::tr("Rejected")};
    }
    std::string stars;
    for (int i = 0; i < 5; ++i) {
        stars += i < rating ? "★" : "☆";
    }
    return stars;
}

// Lays the pane out top to bottom: one column of centered content.
class Writer {
public:
    Writer(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
           SelectableText& text)
        : renderer_{renderer},
          palette_{palette},
          text_{text},
          width_{std::min(bounds.width - (2.0F * kPadding), kMaxColumn)},
          left_{bounds.x + ((bounds.width - width_) / 2.0F)},
          y_{bounds.y + kTop} {}

    [[nodiscard]] float left() const { return left_; }

    [[nodiscard]] float width() const { return width_; }

    [[nodiscard]] float y() const { return y_; }

    void skip(float height) { y_ += height; }

    void centered(std::string_view id, std::string_view text, float size, ui::Color color,
                  float advance) {
        const float w = std::min(text_width(renderer_, text, size), width_);
        const float height = text_.draw(renderer_, std::string{id}, text,
                                        {left_ + ((width_ - w) / 2.0F), y_, w, 0.0F}, size, color,
                                        palette_.secondary_container);
        y_ += std::max(advance, height + 4.0F);
    }

    // A section: a thin line, then its title.
    void section(std::string_view title) {
        y_ += 10.0F;
        renderer_.fill_rect({left_, y_, width_, 1.0F / renderer_.scale()},
                            palette_.outline_variant);
        y_ += 12.0F;
        renderer_.draw_text(i18n::tr(title), left_, y_, kSectionSize, palette_.on_surface_variant);
        y_ += 20.0F;
    }

    // Labels and values stay readable in narrow panes; soft wraps preserve copied text.
    void row(std::string_view label, const std::string& value,
             ui::Typeface typeface = ui::Typeface::proportional) {
        if (value.empty()) {
            return;
        }
        const std::string_view shown_label = i18n::tr(label);
        const float label_width =
            std::min(text_width(renderer_, shown_label, kTextSize), width_ * 0.48F);
        const float room = std::max(1.0F, width_ - label_width - 12.0F);
        const std::string id{label};
        const float label_height =
            text_.draw(renderer_, id + ".label", shown_label, {left_, y_, label_width, 0.0F},
                       kTextSize, palette_.on_surface_variant, palette_.secondary_container);
        const float full =
            renderer_.fonts()
                .layout(value, kTextSize, renderer_.scale(), 0.0F, ui::Ellipsis::end, typeface)
                .width /
            renderer_.scale();
        const float shown = std::min(full, room);
        const float height = text_.draw(
            renderer_, id + ".value", value, {left_ + width_ - shown, y_, shown, 0.0F}, kTextSize,
            palette_.on_surface, palette_.secondary_container, 0.0F, typeface);
        y_ += std::max(kRowHeight, std::max(height, label_height) + 5.0F);
    }

private:
    ui::Renderer& renderer_;
    const theme::Palette& palette_;
    SelectableText& text_;
    float width_;
    float left_;
    float y_;
};

}  // namespace

InfoPane::InfoPane(core::EventLoop& loop, core::WorkerPool& pool, Thumbnails* thumbnails,
                   std::function<void()> redraw)
    : loop_{loop}, pool_{pool}, thumbnails_{thumbnails}, redraw_{std::move(redraw)} {}

InfoPane::~InfoPane() {
    *alive_ = false;
    if (job_) {
        job_->cancel();
    }
    if (size_job_) {
        size_job_->cancel();
    }
    if (size_timer_ != 0) {
        loop_.remove(size_timer_);
    }
}

void InfoPane::set_width(float width) {
    width_ = std::clamp(width, kMinWidth, kMaxWidth);
    redraw_();
}

float InfoPane::width_in(float view_width) const {
    const float width = std::min(width_, view_width - kMinContent);
    return width >= kMinWidth ? width : 0.0F;
}

void InfoPane::set_expanded(bool expanded) {
    expanded_ = expanded;
    redraw_();
}

const preview::Text* InfoPane::shown_text() const {
    return summary_ && summary_->text && !page_.empty() ? &*summary_->text : nullptr;
}

bool InfoPane::text_at(float x, float y) const {
    return bounds_.contains(x, y) &&
           (page_.contains(x, y) || metadata_.cursor_at(x, y) == ui::CursorShape::text);
}

bool InfoPane::text_focused() const noexcept {
    return !bounds_.empty() && (page_focused_ || metadata_.focused());
}

void InfoPane::clear_text_focus() {
    page_focused_ = false;
    selecting_ = false;
    selection_.clear();
    metadata_.clear_focus();
}

std::string InfoPane::selected_text() const {
    if (!text_focused()) {
        return {};
    }
    const preview::Text* const text = shown_text();
    return page_focused_ && text != nullptr ? selection_.text(text->lines)
                                            : metadata_.selected_text();
}

TextPoint InfoPane::point_at(float x, float y) const {
    const preview::Text* const text = shown_text();
    if (text == nullptr || text->lines.empty() || page_clusters_.empty()) {
        return {};
    }
    const float from_top = y - page_.y - kPagePadding + page_scroller_.offset();
    if (from_top < 0.0F) {
        return {};
    }
    const auto line = static_cast<std::size_t>(from_top / kLineHeight);
    if (line >= text->lines.size()) {
        return {text->lines.size() - 1, text->lines.back().size()};
    }
    const float local = x - page_.x - kPagePadding + page_sideways_.offset();
    const auto& clusters = page_clusters_[line];
    float nearest = std::numeric_limits<float>::max();
    std::size_t byte = 0;
    for (const SelectableText::GlyphSpan& cluster : clusters) {
        const float start = cluster.rtl ? cluster.right : cluster.left;
        const float end = cluster.rtl ? cluster.left : cluster.right;
        if (std::abs(local - start) < nearest) {
            nearest = std::abs(local - start);
            byte = cluster.begin;
        }
        if (std::abs(local - end) < nearest) {
            nearest = std::abs(local - end);
            byte = cluster.end;
        }
    }
    return {line, byte};
}

void InfoPane::measure_text(ui::Renderer& renderer, const preview::Text& text) {
    if (!page_clusters_.empty() && page_metrics_scale_ == renderer.scale()) {
        return;
    }
    page_clusters_.clear();
    page_text_width_ = 0.0F;
    page_metrics_scale_ = renderer.scale();
    page_clusters_.reserve(text.lines.size());
    for (const std::string& line : text.lines) {
        const ui::TextLayout& shaped = renderer.fonts().layout(
            line, kLineSize, renderer.scale(), 0.0F, ui::Ellipsis::end, ui::Typeface::monospace);
        page_text_width_ = std::max(page_text_width_, shaped.width / renderer.scale());
        page_clusters_.push_back(SelectableText::glyph_spans(
            renderer.fonts(), line, kLineSize, renderer.scale(), ui::Typeface::monospace));
    }
}

bool InfoPane::autoscrolling() const noexcept {
    return selecting_ && !page_.empty() &&
           ((page_scroller_.max_offset() > 0.0F &&
             (pointer_y_ < page_.y + kPagePadding || pointer_y_ > page_.bottom() - kPagePadding)) ||
            (page_sideways_.max_offset() > 0.0F &&
             (pointer_x_ < page_.x + kPagePadding || pointer_x_ > page_.right() - kPagePadding)));
}

bool InfoPane::pointer(const ui::PointerEvent& event, Clock::time_point now) {
    using Kind = ui::PointerEvent::Kind;
    if (event.kind == Kind::leave) {
        selecting_ = false;
        metadata_.pointer(event);
        return false;
    }
    if (event.kind == Kind::press) {
        if (event.button != ui::PointerButton::left) {
            return false;
        }
        if (!bounds_.contains(event.x, event.y)) {
            clear_text_focus();
            return false;
        }
        if (const preview::Text* const text = shown_text();
            text != nullptr && page_.contains(event.x, event.y)) {
            metadata_.clear_focus();
            page_focused_ = true;
            selecting_ = true;
            pointer_x_ = event.x;
            pointer_y_ = event.y;
            autoscrolled_ = now;
            selection_.press(point_at(event.x, event.y), event.click_count, event.modifiers.shift,
                             text->lines);
            redraw_();
            return true;
        }
        page_focused_ = false;
        selecting_ = false;
        selection_.clear();
        const bool handled = metadata_.pointer(event);
        redraw_();
        return handled;
    }
    if (selecting_ && (event.kind == Kind::motion || event.kind == Kind::release)) {
        if (event.kind == Kind::release && event.button != ui::PointerButton::left) {
            return false;
        }
        pointer_x_ = event.x;
        pointer_y_ = event.y;
        if (const preview::Text* const text = shown_text()) {
            selection_.drag(point_at(event.x, event.y), text->lines);
        }
        selecting_ = event.kind != Kind::release;
        autoscrolled_ = now;
        redraw_();
        return true;
    }
    if (metadata_.pointer(event)) {
        redraw_();
        return true;
    }
    return false;
}

bool InfoPane::text_key(const ui::KeyEvent& event, Clock::time_point now) {
    if (!text_focused()) {
        return false;
    }
    if (!page_focused_) {
        const bool handled = metadata_.key(event);
        if (handled) {
            redraw_();
        }
        return handled;
    }
    const preview::Text* const text = shown_text();
    if (text == nullptr) {
        return false;
    }
    if (event.modifiers == ui::Modifiers{.ctrl = true}) {
        if (event.keysym == XKB_KEY_a || event.keysym == XKB_KEY_A) {
            selection_.select_all(text->lines);
            redraw_();
            return true;
        }
        if (event.keysym == XKB_KEY_c || event.keysym == XKB_KEY_C) {
            if (copy_ && !selection_.empty()) {
                copy_(selection_.text(text->lines));
            }
            return true;
        }
    }
    if (event.modifiers != ui::Modifiers{}) {
        return false;
    }
    switch (event.keysym) {
        case XKB_KEY_Home:
            page_scroller_.glide_to(0.0F, now);
            break;
        case XKB_KEY_End:
            page_scroller_.glide_to(page_scroller_.max_offset(), now);
            break;
        case XKB_KEY_Page_Up:
            scroll_text(TextScroll::page_up, now);
            break;
        case XKB_KEY_Page_Down:
            scroll_text(TextScroll::page_down, now);
            break;
        default:
            return false;
    }
    redraw_();
    return true;
}

bool InfoPane::scroll(const ui::PointerEvent& event, Clock::time_point now) {
    if (event.x < bounds_.x || event.x >= bounds_.right() || event.y < bounds_.y ||
        event.y >= bounds_.bottom()) {
        return false;
    }
    // A page with more lines than fit takes the wheel over it; the rest of the pane scrolls the
    // pane.
    ui::Scroller& scroller =
        page_.contains(event.x, event.y) && (page_scroller_.max_offset() > 0.0F ||
                                             (event.modifiers.shift && event.wheel_y != 0.0F &&
                                              page_sideways_.max_offset() > 0.0F))
            ? page_scroller_
            : scroller_;
    if (&scroller == &page_scroller_ && event.modifiers.shift && event.wheel_y != 0.0F) {
        page_sideways_.wheel(event.wheel_y, now);
    } else if (event.wheel_y != 0.0F) {
        scroller.wheel(event.wheel_y, now);
    } else if (event.scroll_y != 0.0F) {
        scroller.finger(event.scroll_y, now);
    }
    if (page_.contains(event.x, event.y)) {
        if (event.wheel_x != 0.0F) {
            page_sideways_.wheel(event.wheel_x, now);
        } else if (event.scroll_x != 0.0F) {
            page_sideways_.finger(event.scroll_x, now);
        }
    }
    if (event.scroll_finger && event.scroll_stop) {
        scroller.release(now);
        page_sideways_.release(now);
    }
    redraw_();
    return true;
}

bool InfoPane::scroll_text(TextScroll how, Clock::time_point now) {
    if (page_.width <= 0.0F || page_scroller_.max_offset() <= 0.0F) {
        return false;
    }
    // A page: what is shown less a line, so that one stays in sight.
    const float page = std::max(kLineHeight, page_.height - (2.0F * kPagePadding) - kLineHeight);
    const float from = page_scroller_.destination();
    switch (how) {
        case TextScroll::line_up:
            page_scroller_.glide_to(from - (kScrollStep * kLineHeight), now);
            break;
        case TextScroll::line_down:
            page_scroller_.glide_to(from + (kScrollStep * kLineHeight), now);
            break;
        case TextScroll::page_up:
            page_scroller_.glide_to(from - page, now);
            break;
        case TextScroll::page_down:
            page_scroller_.glide_to(from + page, now);
            break;
    }
    redraw_();
    return true;
}

bool InfoPane::click(float x, float y) {
    if (toggle_.width <= 0.0F || x < toggle_.x || x >= toggle_.right() || y < toggle_.y ||
        y >= toggle_.bottom()) {
        return false;
    }
    set_expanded(!expanded_);
    return true;
}

void InfoPane::request(const std::filesystem::path& path, std::int64_t modified_ns,
                       std::string mime, preview::Kind kind) {
    std::string key = std::format("{}\n{}\n{}", path.string(), modified_ns, static_cast<int>(kind));
    if (key == key_) {
        return;
    }
    key_ = std::move(key);
    summary_.reset();
    clear_text_focus();
    metadata_.reset();
    scroller_.jump_to(0.0F);  // another entry starts at the top
    page_scroller_.jump_to(0.0F);
    page_sideways_.jump_to(0.0F);
    page_placed_ = false;
    page_clusters_.clear();
    if (job_) {
        job_->cancel();
    }
    job_ = pool_.submit(
        core::Priority::visible,
        [path, mime = std::move(mime), kind](const core::CancelToken& token) {
            return std::make_shared<const preview::Summary>(
                preview::summarize(path, mime, kind, token));
        },
        [this, alive = alive_, key = key_](std::shared_ptr<const preview::Summary> summary) {
            if (!*alive || key != key_) {
                return;
            }
            job_.reset();
            summary_ = std::move(summary);
            redraw_();
        });
}

void InfoPane::measure(std::vector<std::filesystem::path> roots, std::string key) {
    if (key == size_key_) {
        return;
    }
    size_key_ = std::move(key);
    size_.reset();
    size_done_ = false;
    if (size_timer_ != 0) {
        loop_.remove(size_timer_);
        size_timer_ = 0;
    }
    if (size_job_) {
        size_job_->cancel();
        size_job_.reset();
    }
    if (roots.empty()) {
        return;
    }
    size_timer_ = loop_.add_timer(
        kMeasureDelay, std::chrono::milliseconds{0},
        [this, roots = std::move(roots), key = size_key_] {
            size_timer_ = 0;
            size_ = fs::Tally{};  // "Counting…"
            redraw_();
            size_job_ = pool_.submit_streaming(
                core::Priority::nearby,
                [roots](const core::CancelToken& token, auto emit) {
                    // One measurer for all: a file hard-linked from two of them counts once.
                    fs::Measurer measurer{
                        {}, token, [&emit](const fs::Tally& tally) { emit(tally); }};
                    for (const std::filesystem::path& root : roots) {
                        measurer.add(root);
                    }
                    return measurer.tally();
                },
                [this, alive = alive_, key](const fs::Tally& tally) {
                    if (*alive && key == size_key_) {
                        size_ = tally;
                        redraw_();
                    }
                },
                [this, alive = alive_, key](const fs::Tally& tally) {
                    if (*alive && key == size_key_) {
                        size_job_.reset();
                        size_ = tally;
                        size_done_ = true;
                        redraw_();
                    }
                });
        });
}

std::string InfoPane::size_text() const {
    if (!size_) {
        return {};
    }
    if (size_->remote && size_->entries() == 0) {
        return "—";  // not walked on a network file system
    }
    if (!size_done_) {
        return size_->bytes == 0 ? std::string{i18n::tr("Counting…")}
                                 : i18n::trf("Counting… {}", format_size(size_->bytes));
    }
    return size_->complete() ? format_size(size_->bytes)
                             : i18n::trf("at least {}", format_size(size_->bytes));
}

void InfoPane::draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
                    ui::IconSet& icons, const Folder& folder, Clock::time_point now) {
    toggle_ = {};
    page_ = {};
    bounds_ = bounds;
    // Scrolls when the content (as tall as at the last draw) does not fit.
    scroller_.set_extent(content_height_, bounds.height);
    scroller_.advance(now);
    const float offset = scroller_.offset();
    renderer.push_clip(bounds);
    metadata_.begin_frame(bounds);
    Writer out{
        renderer, {bounds.x, bounds.y - offset, bounds.width, bounds.height}, palette, metadata_};
    const auto finish = [&] {
        if (page_.height <= 0.0F) {
            // Multiple selection can replace a text page without requesting another summary.
            page_scroller_.jump_to(page_scroller_.offset());
            page_sideways_.jump_to(page_sideways_.offset());
            page_focused_ = false;
            selecting_ = false;
            selection_.clear();
        }
        metadata_.end_frame();
        content_height_ = out.y() - (bounds.y - offset) + kTop;
        renderer.pop_clip();
    };
    const auto icon = [&](std::string_view name, ui::Color color) {
        icons.draw(renderer, name, out.left() + ((out.width() - kIconSize) / 2.0F), out.y(),
                   kIconSize, color);
        out.skip(kIconSize + 18.0F);
    };

    const fs::Entry* const single = folder.single_selection();
    std::string shown_key =
        std::format("{}\n{}\n{}", folder.path().string(), folder.selection_revision(),
                    single == nullptr ? std::string{} : single->name);
    if (shown_key != shown_key_) {
        shown_key_ = std::move(shown_key);
        clear_text_focus();
        metadata_.reset();
    }
    if (single == nullptr && folder.selected_count() > 1) {
        // Several entries: how many, of which kind, and how big (with folders, added up on a
        // worker as for one).
        const SelectionSummary& selection = folder.summary();
        if (selection.folders > 0) {
            std::vector<std::filesystem::path> roots;
            std::string key = folder.path().string();
            key += '\0' + std::to_string(folder.model().revision());
            roots.reserve(folder.selected_count());
            for (const fs::Entry& entry : folder.entries()) {
                if (folder.is_selected(entry.name)) {
                    roots.push_back(folder.path_of(entry));
                    key += '\0' + entry.name;
                }
            }
            measure(std::move(roots), std::move(key));
        } else {
            measure({}, {});
        }
        icon("files", palette.on_surface_variant);
        out.centered("summary", count_of(selection.folders + selection.files, "item"), 15.0F,
                     palette.on_surface, 22.0F);
        std::string kinds;
        if (selection.folders > 0) {
            kinds = count_of(selection.folders, "folder");
        }
        if (selection.files > 0) {
            kinds += (kinds.empty() ? "" : ", ") + count_of(selection.files, "file");
        }
        out.centered("kinds", kinds, kSmallSize, palette.on_surface_variant, 20.0F);
        if (selection.folders > 0) {
            out.section("INFORMATION");
            out.row("Size", size_text());
        } else if (selection.files > 0) {
            out.section("INFORMATION");
            out.row("Size", format_size(selection.bytes));
        }
        finish();
        return;
    }

    const fs::MimeDatabase& mime = folder.mime();
    const std::filesystem::path path = single == nullptr ? folder.path() : folder.path_of(*single);
    fs::Entry typed;
    preview::Kind kind = preview::Kind::info;
    if (single != nullptr) {
        typed = *single;
        typed.mime = folder.mime_of(*single);
        kind = preview::provider_for(mime, typed.mime).kind();
    }
    if (single != nullptr && single->is_directory()) {
        measure({path}, std::format("{}\n{}\n{}", path.string(), single->modified_ns,
                                    folder.model().revision()));
    } else {
        measure({}, {});
    }
    request(path, single == nullptr ? 0 : single->modified_ns,
            single == nullptr ? std::string{} : std::string{mime.name(typed.mime)}, kind);
    const preview::Summary* const summary = summary_.get();
    const preview::Info* const info = summary == nullptr ? nullptr : &summary->info;
    const picture::PhotoInfo* const photo =
        summary == nullptr || !summary->photo ? nullptr : &*summary->photo;

    // The preview takes the pane's whole width (a wider pane, a larger photo); the rows keep to
    // the column.
    const float preview_width = bounds.width - (2.0F * kPadding);
    const float preview_left = bounds.x + kPadding;
    if (single == nullptr) {
        icon("folder", palette.primary);
    } else if (kind == preview::Kind::image) {
        // A square as wide as the pane (at most half its height), always reserved: nothing
        // moves when the thumbnail arrives.
        const float box = std::min(preview_width, bounds.height * kMaxPreview);
        const ui::Texture* thumbnail = nullptr;
        picture::Failure failure = picture::Failure::none;
        if (thumbnails_ != nullptr) {
            thumbnail = thumbnails_->get(
                path, single->modified_ns,
                thumbs::size_for(static_cast<std::uint32_t>(std::lround(box * renderer.scale()))),
                mime.name(typed.mime), &failure);
        }
        if (thumbnail != nullptr) {
            const float fit =
                box / static_cast<float>(std::max(thumbnail->width(), thumbnail->height()));
            const float w = static_cast<float>(thumbnail->width()) * fit;
            const float h = static_cast<float>(thumbnail->height()) * fit;
            const ui::Rect rect{preview_left + ((preview_width - w) / 2.0F),
                                out.y() + ((box - h) / 2.0F), w, h};
            renderer.draw_shadow({rect.x, rect.y + 4.0F, rect.width, rect.height}, {}, 12.0F,
                                 palette.shadow.with_alpha(0.5F));
            renderer.draw_image(*thumbnail, rect, {});
        } else {
            icons.draw_file(renderer, (mime).name(typed.mime), fs::icon_name(mime, typed),
                            out.left() + ((out.width() - kIconSize) / 2.0F),
                            out.y() + ((box - kIconSize) / 2.0F), kIconSize,
                            palette.on_surface_variant, typed.folder_icon);
            draw_failure_badge(renderer, icons, palette, failure,
                               out.left() + ((out.width() - kIconSize) / 2.0F),
                               out.y() + ((box - kIconSize) / 2.0F), kIconSize);
        }
        out.skip(box + 18.0F);
    } else if (summary != nullptr && summary->text && !summary->text->lines.empty()) {
        // A small page of the lines; the rest scrolls.
        const std::vector<std::string>& lines = summary->text->lines;
        const std::vector<std::vector<text::Span>>& spans = summary->text->spans;
        const theme::SyntaxColors colours = theme::syntax_colors(palette);
        const std::size_t shown = std::min(lines.size(), kPageLines);
        // Fit whole lines in the pane, including the page's padding and the pane's margins.
        // Use its unscrolled height: scrolling to the metadata must not resize the text page.
        const float available =
            std::max(0.0F, bounds.height - (2.0F * kTop) - (2.0F * kPagePadding));
        const float view =
            std::min(static_cast<float>(shown), std::floor(available / kLineHeight)) * kLineHeight;
        const float height = view + (2.0F * kPagePadding);
        page_ = {preview_left, out.y(), preview_width, height};
        page_scroller_.set_extent(static_cast<float>(lines.size()) * kLineHeight, view);
        if (!page_placed_) {
            // A log opens at its newest lines.
            page_scroller_.jump_to(summary->text->tail ? page_scroller_.max_offset() : 0.0F);
            page_placed_ = true;
        }
        page_scroller_.advance(now);
        measure_text(renderer, *summary->text);
        page_sideways_.set_extent(page_text_width_ + kLineSize,
                                  page_.width - (2.0F * kPagePadding));
        page_sideways_.advance(now);
        if (autoscrolling()) {
            const float seconds =
                std::clamp(std::chrono::duration<float>(now - autoscrolled_).count(), 0.0F, 0.1F);
            const auto step = [seconds](float position, float start, float end) {
                const float beyond = position < start ? position - start
                                     : position > end ? position - end
                                                      : 0.0F;
                return std::clamp(beyond * 8.0F, -900.0F, 900.0F) * seconds;
            };
            page_scroller_.jump_to(page_scroller_.offset() + step(pointer_y_,
                                                                  page_.y + kPagePadding,
                                                                  page_.bottom() - kPagePadding));
            page_sideways_.jump_to(page_sideways_.offset() + step(pointer_x_,
                                                                  page_.x + kPagePadding,
                                                                  page_.right() - kPagePadding));
            autoscrolled_ = now;
        }
        if (selecting_) {
            selection_.drag(point_at(pointer_x_, pointer_y_), lines);
        }
        const float page_offset = page_scroller_.offset();
        const float sideways = page_sideways_.offset();
        renderer.draw_box(page_, {.fill = palette.surface_container, .radius = {}});
        const ui::Rect inner{page_.x + kPagePadding, page_.y + kPagePadding,
                             page_.width - (2.0F * kPagePadding), view};
        renderer.push_clip(inner);
        const auto first = static_cast<std::size_t>(page_offset / kLineHeight);
        const auto last = std::min(
            lines.size(), static_cast<std::size_t>(std::ceil((page_offset + view) / kLineHeight)));
        if (!selection_.empty()) {
            const auto [from, to] = selection_.range();
            for (std::size_t i = std::max(first, from.line); i < last && i <= to.line; ++i) {
                const std::size_t start = i == from.line ? from.byte : 0;
                const std::size_t end = i == to.line ? to.byte : lines[i].size();
                for (const SelectableText::GlyphSpan& cluster : page_clusters_[i]) {
                    if (cluster.end <= start || cluster.begin >= end) {
                        continue;
                    }
                    renderer.fill_rect({inner.x - sideways + cluster.left,
                                        inner.y + static_cast<float>(i) * kLineHeight - page_offset,
                                        cluster.right - cluster.left, kLineHeight},
                                       palette.secondary_container);
                }
                if (i < to.line) {
                    // Include a small mark for a selected line break, including empty lines.
                    const float end_x =
                        page_clusters_[i].empty() ? 0.0F : page_clusters_[i].back().right;
                    renderer.fill_rect({inner.x - sideways + end_x,
                                        inner.y + static_cast<float>(i) * kLineHeight - page_offset,
                                        kLineSize * 0.5F, kLineHeight},
                                       palette.secondary_container);
                }
            }
        }
        for (std::size_t i = first; i < last; ++i) {
            const std::vector<ui::ColorRun> runs = i < spans.size()
                                                       ? colour_runs(spans[i], colours, 1.0F)
                                                       : std::vector<ui::ColorRun>{};
            renderer.draw_text_runs(lines[i], inner.x - sideways,
                                    inner.y + (static_cast<float>(i) * kLineHeight) - page_offset,
                                    kLineSize, palette.on_surface_variant, runs, 0.0F,
                                    ui::Ellipsis::end, ui::Typeface::monospace);
        }
        renderer.pop_clip();
        if (page_scroller_.max_offset() > 0.0F) {
            // A thin thumb, only when there is more than the page shows.
            const float content = static_cast<float>(lines.size()) * kLineHeight;
            const float thumb = std::min(view, std::max(kScrollbarMinThumb, view * view / content));
            const float thumb_y =
                inner.y + ((view - thumb) * page_offset / page_scroller_.max_offset());
            renderer.draw_box(
                {page_.right() - kScrollbarWidth - 3.0F, thumb_y, kScrollbarWidth, thumb},
                {.fill = palette.on_surface_variant.with_alpha(0.45F), .radius = {}});
        }
        if (page_sideways_.max_offset() > 0.0F) {
            const float content = page_text_width_ + kLineSize;
            const float thumb = std::min(
                inner.width, std::max(kScrollbarMinThumb, inner.width * inner.width / content));
            const float thumb_x =
                inner.x + ((inner.width - thumb) * sideways / page_sideways_.max_offset());
            renderer.draw_box({thumb_x, page_.bottom() - 6.0F, thumb, kScrollbarWidth},
                              {.fill = palette.on_surface_variant.with_alpha(0.45F), .radius = {}});
        }
        out.skip(height + 18.0F);
    } else {
        icons.draw_file(renderer, mime.name(typed.mime), fs::icon_name(mime, typed),
                        out.left() + ((out.width() - kIconSize) / 2.0F), out.y(), kIconSize,
                        single->is_directory() ? palette.primary : palette.on_surface_variant,
                        typed.folder_icon);
        out.skip(kIconSize + 18.0F);
    }

    // Name and type.
    std::string name = single == nullptr ? folder.path().filename().string() : single->name;
    if (name.empty()) {
        name = folder.path().string();  // the root
    }
    out.centered("name", name, 15.0F, palette.on_surface, 22.0F);
    std::string type{i18n::tr("Folder")};
    if (single != nullptr) {
        type =
            typed.mime == fs::kUnknownMime ? std::string{"…"} : std::string{mime.name(typed.mime)};
    }
    out.centered("type", type, kSmallSize, palette.on_surface_variant, 20.0F);

    // What it is.
    out.section("INFORMATION");
    if (single == nullptr) {
        out.row("Items", std::format("{}", folder.entries().size()));
    } else if (single->is_directory()) {
        // As many as the folder will show when opened.
        out.row("Items",
                info != nullptr && info->items
                    ? std::format("{}", folder.shows_hidden() ? *info->items
                                                              : *info->items - info->hidden_items)
                    : std::string{});
        out.row("Size", size_text());
        if (size_done_ && size_ && !size_->remote && size_->folders > 0) {
            // What is inside: the folder itself left out.
            out.row("Contains", std::format("{}, {}", count_of(size_->files, "file"),
                                            count_of(size_->folders - 1, "folder")));
        }
    } else {
        out.row("Size", format_size(single->size));
        // The dot on the icon, in words (issue #17): for a file of a type that runs.
        if (!folder.is_trash() && folder.type_runs(*single)) {
            out.row("Runs as program",
                    std::string{i18n::tr(folder.runs_as_program(*single) ? "Yes" : "No")});
        }
    }
    if (photo != nullptr && photo->width > 0) {
        out.row("Dimensions", std::format("{} × {}", photo->width, photo->height));
    }
    if (info != nullptr && info->created_ns) {
        out.row("Created", format_time(*info->created_ns));
    }
    if (single != nullptr) {
        out.row("Modified", format_time(single->modified_ns));
        // In the trash: where it was, and when it went.
        if (const fs::TrashedItem* const item = folder.model().trashed(single->name)) {
            out.row("Original location", item->info.original.parent_path().string());
            std::string deleted = item->info.deleted;
            std::ranges::replace(deleted, 'T', ' ');
            out.row("Deleted", deleted);
        }
    }

    // The shot.
    const std::string exposure = photo == nullptr ? std::string{} : exposure_of(*photo);
    if (photo != nullptr && !photo->video_codec.empty()) {
        out.section("VIDEO");
        for (const auto& [label, value] : media_details(*photo))
            out.row(label, value);
    } else if (photo != nullptr && (!media_details(*photo).empty() || photo->rating)) {
        out.section("PHOTO");
        out.row("Camera", photo->camera);
        out.row("Lens", photo->lens);
        out.row("Exposure", exposure);
        out.row("Focal length", photo->focal_length);
        out.row("Taken", photo->taken);
        out.row("Rating", photo->rating ? rating_of(*photo->rating) : std::string{});
    }

    // The rest, on request.
    if (info != nullptr) {
        out.skip(8.0F);
        const std::string_view label =
            expanded_ ? i18n::tr("Fewer details") : i18n::tr("More details");
        const float label_width = text_width(renderer, label, kTextSize);
        const float total = label_width + 6.0F + 12.0F;
        const float x = out.left() + ((out.width() - total) / 2.0F);
        renderer.draw_text(label, x, out.y(), kTextSize, palette.primary);
        icons.draw(renderer, expanded_ ? "chevron-up" : "chevron-down", x + label_width + 6.0F,
                   out.y() + 2.0F, 12.0F, palette.primary);
        toggle_ = {x - 8.0F, out.y() - 4.0F, total + 16.0F, kRowHeight};
        out.skip(kRowHeight);
        if (expanded_) {
            out.section("MORE");
            out.row("Permissions", info->permissions, ui::Typeface::monospace);
            if (!info->owner.empty()) {
                out.row("Owner", info->owner + " · " + info->group);
            }
            out.row("Location", path.parent_path().string());
            out.row("Link to", info->link_target);
            if (photo != nullptr && photo->video_codec.empty()) {
                out.row("Program", photo->program);
                out.row("Compensation", photo->exposure_bias);
                out.row("Metering", photo->metering);
                out.row("White balance", photo->white_balance);
                out.row("Flash", photo->flash);
                out.row("Software", photo->software);
            }
        }
        if (!info->error.empty()) {
            out.skip(6.0F);
            out.centered("error", info->error, kSmallSize, palette.error, 20.0F);
        }
    }
    finish();
}

}  // namespace rummage::views
