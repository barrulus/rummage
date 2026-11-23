#include "rummage/views/info_pane.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "archive_text.hpp"
#include "content_details.hpp"
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
constexpr float kScrollStep = 3.0F;  // lines, for Alt+J and Alt+K

// "1 file", "3 files".
std::string count_of(std::size_t count, std::string_view noun) {
    if (noun == "folder") {
        return i18n::trn("{} folder", "{} folders", count);
    }
    if (noun == "file") {
        return i18n::trn("{} file", "{} files", count);
    }
    return i18n::trn("{} item", "{} items", count);
}

// Lays the pane out top to bottom: one column of centered content.
class Writer {
public:
    Writer(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
           SelectableText& text, std::string& exported)
        : renderer_{renderer},
          palette_{palette},
          text_{text},
          exported_{exported},
          width_{std::min(bounds.width - (2.0F * kPadding), kMaxColumn)},
          left_{bounds.x + ((bounds.width - width_) / 2.0F)},
          y_{bounds.y + kTop} {}

    [[nodiscard]] float left() const { return left_; }

    [[nodiscard]] float width() const { return width_; }

    [[nodiscard]] float y() const { return y_; }

    void skip(float height) { y_ += height; }

    void centered(std::string_view id, std::string_view text, float size, ui::Color color,
                  float advance) {
        if (id != "preview-hint")
            exported_ += std::string{text} + "\n";
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
             ui::Typeface typeface = ui::Typeface::proportional, bool visible = true) {
        if (value.empty()) {
            return;
        }
        const std::string_view shown_label = i18n::tr(label);
        exported_ += std::string{shown_label} + ": " + value + "\n";
        if (!visible)
            return;
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
    std::string& exported_;
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
    if (hold_timer_ != 0) {
        loop_.remove(hold_timer_);
    }
}

void InfoPane::drop_held() {
    if (hold_timer_ != 0) {
        loop_.remove(hold_timer_);
        hold_timer_ = 0;
    }
    if (held_) {
        held_.reset();
        scroller_.jump_to(0.0F);
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
    if (!expanded)
        scroller_.jump_to(0.0F);
    redraw_();
}

bool InfoPane::text_at(float x, float y) const {
    return bounds_.contains(x, y) && !holding() &&
           metadata_.cursor_at(x, y) == ui::CursorShape::text;
}

bool InfoPane::text_focused() const noexcept { return !bounds_.empty() && metadata_.focused(); }

void InfoPane::clear_text_focus() { metadata_.clear_focus(); }

std::string InfoPane::selected_text() const {
    return text_focused() ? metadata_.selected_text() : std::string{};
}

bool InfoPane::pointer(const ui::PointerEvent& event, Clock::time_point /*now*/) {
    using Kind = ui::PointerEvent::Kind;
    if (event.kind == Kind::leave) {
        metadata_.pointer(event);
        return false;
    }
    if (event.kind == Kind::press && !bounds_.contains(event.x, event.y)) {
        clear_text_focus();
        return false;
    }
    if (event.kind == Kind::press && event.button == ui::PointerButton::left &&
        copy_button_.contains(event.x, event.y))
        return click(event.x, event.y);
    if (!holding() && metadata_.pointer(event)) {
        redraw_();
        return true;
    }
    return false;
}

bool InfoPane::text_key(const ui::KeyEvent& event, Clock::time_point /*now*/) {
    if (!text_focused() || !metadata_.key(event))
        return false;
    redraw_();
    return true;
}

bool InfoPane::scroll(const ui::PointerEvent& event, Clock::time_point now) {
    if (!bounds_.contains(event.x, event.y))
        return false;
    if (event.wheel_y != 0.0F)
        scroller_.wheel(event.wheel_y, now);
    else if (event.scroll_y != 0.0F)
        scroller_.finger(event.scroll_y, now);
    if (event.scroll_finger && event.scroll_stop)
        scroller_.release(now);
    redraw_();
    return true;
}

bool InfoPane::scroll_text(TextScroll how, Clock::time_point now) {
    if (bounds_.empty() || scroller_.max_offset() <= 0.0F) {
        return false;
    }
    // A page: what is shown less a line, so that one stays in sight.
    const float page = std::max(kLineHeight, bounds_.height - kLineHeight);
    const float from = scroller_.destination();
    switch (how) {
        case TextScroll::line_up:
            scroller_.glide_to(from - (kScrollStep * kLineHeight), now);
            break;
        case TextScroll::line_down:
            scroller_.glide_to(from + (kScrollStep * kLineHeight), now);
            break;
        case TextScroll::page_up:
            scroller_.glide_to(from - page, now);
            break;
        case TextScroll::page_down:
            scroller_.glide_to(from + page, now);
            break;
    }
    redraw_();
    return true;
}

bool InfoPane::click(float x, float y) {
    if (bounds_.contains(x, y) && insights_button_.contains(x, y) && insights_) {
        insights_(insights_path_);
        return true;
    }
    if (!bounds_.contains(x, y))
        return false;
    if (copy_ && !copy_text_.empty() && copy_button_.contains(x, y)) {
        copy_(copy_text_);
        copied_ = true;
        redraw_();
        return true;
    }
    if (toggle_.width <= 0.0F || x < toggle_.x || x >= toggle_.right() || y < toggle_.y ||
        y >= toggle_.bottom()) {
        return false;
    }
    set_expanded(!expanded_);
    return true;
}

void InfoPane::request(const std::filesystem::path& path, std::int64_t modified_ns,
                       std::string mime, preview::Kind kind,
                       std::shared_ptr<const fs::MimeDatabase> types) {
    std::string key = std::format("{}\n{}\n{}", path.string(), modified_ns, static_cast<int>(kind));
    if (key == key_) {
        return;
    }
    key_ = std::move(key);
    if (summary_) {
        held_ = std::move(summary_);
        summary_.reset();
    }
    if (held_ && hold_timer_ == 0) {
        // Not restarted by the next file: moving on and on shows the icons after kHold.
        hold_timer_ = loop_.add_timer(kHold, std::chrono::milliseconds{0}, [this] {
            hold_timer_ = 0;
            if (held_) {
                drop_held();
                redraw_();
            }
        });
    }
    clear_text_focus();
    metadata_.reset();
    if (!held_) {
        // Another entry starts at the top; a held summary keeps its place until it is replaced.
        scroller_.jump_to(0.0F);
    }
    if (job_) {
        job_->cancel();
    }
    job_ = pool_.submit(
        core::Priority::visible,
        [path, mime = std::move(mime), kind,
         types = std::move(types)](const core::CancelToken& token) {
            return std::make_shared<const preview::Summary>(
                preview::summarize(path, mime, kind, token, types, true));
        },
        [this, alive = alive_, key = key_](std::shared_ptr<const preview::Summary> summary) {
            if (!*alive || key != key_) {
                return;
            }
            job_.reset();
            drop_held();
            summary_ = std::move(summary);
            redraw_();
        });
}

void InfoPane::forget_summary() {
    key_.clear();
    summary_.reset();
    drop_held();
    if (job_) {
        job_->cancel();
        job_.reset();
    }
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
    insights_button_ = {};
    thumbnail_ = {};
    copy_button_ = {};
    copy_text_.clear();
    bounds_ = bounds;
    const float header = copy_ ? 38.0F : 0.0F;
    const ui::Rect content{bounds.x, bounds.y + header, bounds.width,
                           std::max(0.0F, bounds.height - header)};
    // Scrolls when the content (as tall as at the last draw) does not fit.
    scroller_.set_extent(content_height_, content.height);
    scroller_.advance(now);
    const float offset = scroller_.offset();
    renderer.push_clip(content);
    metadata_.begin_frame(content);
    Writer out{renderer,
               {content.x, content.y - offset, content.width, content.height},
               palette,
               metadata_,
               copy_text_};
    const auto finish = [&] {
        metadata_.end_frame();
        content_height_ = out.y() - (content.y - offset) + kTop;
        renderer.pop_clip();
        if (holding() || job_)
            copy_text_.clear();
        if (copy_) {
            copy_button_ = {out.left(), bounds.y + 6, out.width(), 28};
            renderer.push_clip(bounds);
            renderer.draw_box(copy_button_, {.fill = palette.surface_container_high});
            const auto color = palette.on_surface.with_alpha(copy_text_.empty() ? 0.4F : 1.0F);
            icons.draw(renderer, copied_ ? "check" : "files", copy_button_.x + 6,
                       copy_button_.y + 5, 18, color);
            renderer.draw_text(copied_ ? i18n::tr("Copied") : i18n::tr("Copy all properties"),
                               copy_button_.x + 30, copy_button_.y + 6, 12, color,
                               std::max(0.0F, copy_button_.width - 36));
            renderer.pop_clip();
        }
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
        copied_ = false;
        clear_text_focus();
        metadata_.reset();
    }
    if (single == nullptr && folder.selected_count() > 1) {
        // Several entries: how many, of which kind, and how big (with folders, added up on a
        // worker as for one).
        const SelectionSummary& selection = folder.summary();
        if (selection.folders > 0 && !folder.is_archive()) {
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
    if (single != nullptr && single->is_directory() && !folder.is_archive()) {
        measure({path}, std::format("{}\n{}\n{}", path.string(), single->modified_ns,
                                    folder.model().revision()));
    } else {
        measure({}, {});
    }
    if (folder.is_archive()) {
        forget_summary();
    } else {
        request(path, single == nullptr ? 0 : single->modified_ns,
                single == nullptr ? std::string{} : std::string{mime.name(typed.mime)}, kind,
                folder.shared_mime());
    }
    // While the next summary loads, the one before (kHold at most): drawn only, not selectable.
    const bool from_before = holding();
    const preview::Summary* const summary = from_before ? held_.get() : summary_.get();
    const preview::Info* const info = summary == nullptr ? nullptr : &summary->info;
    const picture::PhotoInfo* const photo =
        summary == nullptr || !summary->photo ? nullptr : &*summary->photo;
    const preview::Archive* const held =
        summary == nullptr || !summary->archive ? nullptr : &*summary->archive;

    // The preview takes the pane's whole width (a wider pane, a larger photo); the rows keep to
    // the column.
    const float preview_width = bounds.width - (2.0F * kPadding);
    const float preview_left = bounds.x + kPadding;
    if (single == nullptr) {
        icon("folder", theme::readable_foreground(palette.primary, palette.surface_container));
    } else if (kind == preview::Kind::image) {
        // A square as wide as the pane (at most half its height), always reserved: nothing
        // moves when the thumbnail arrives.
        const float box = std::min(preview_width, bounds.height * kMaxPreview);
        const ui::Texture* thumbnail = nullptr;
        picture::Failure failure = picture::Failure::none;
        if (thumbnails_ != nullptr && !folder.is_archive()) {
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
        // A thumbnail of the beginning; reading, selection and scrolling live in Quick Look.
        const auto& lines = summary->text->lines;
        const auto& spans = summary->text->spans;
        const auto colours = theme::syntax_colors(palette);
        const float view =
            std::min(static_cast<float>(std::min(lines.size(), std::size_t{12})) * kLineHeight,
                     std::max(0.0F, bounds.height * 0.35F));
        const ui::Rect box{preview_left, out.y(), preview_width, view + 2 * kPagePadding};
        renderer.draw_box(box, {.fill = palette.surface_container, .radius = {}});
        const ui::Rect inner{box.x + kPagePadding, box.y + kPagePadding,
                             std::max(0.0F, box.width - 2 * kPagePadding), view};
        renderer.push_clip(inner);
        for (std::size_t i = 0; i < lines.size() && static_cast<float>(i) * kLineHeight < view;
             ++i) {
            const auto runs = i < spans.size() ? colour_runs(spans[i], colours, 1.0F)
                                               : std::vector<ui::ColorRun>{};
            renderer.draw_text_runs(lines[i], inner.x,
                                    inner.y + static_cast<float>(i) * kLineHeight, kLineSize,
                                    palette.on_surface_variant, runs, inner.width,
                                    ui::Ellipsis::end, ui::Typeface::monospace);
        }
        renderer.pop_clip();
        thumbnail_ = from_before ? ui::Rect{} : box;
        out.skip(box.height + 8);
        out.centered("preview-hint", i18n::tr("Space to preview"), kSmallSize,
                     palette.on_surface_variant, 20);
        out.skip(10);
    } else {
        icons.draw_file(renderer, mime.name(typed.mime), fs::icon_name(mime, typed),
                        out.left() + ((out.width() - kIconSize) / 2.0F), out.y(), kIconSize,
                        single->is_directory()
                            ? theme::readable_foreground(palette.primary, palette.surface_container)
                            : palette.on_surface_variant,
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
        if (const archive::Entry* const packed = folder.model().archive_entry(single->name);
            packed != nullptr && packed->packed != packed->size) {
            out.row(i18n::tr("Compressed"), format_size(packed->packed));
        }
        // An archive: what it holds, once listed.
        if (held != nullptr) {
            const ArchiveRows rows = archive_rows(*held);
            out.row("Contains", rows.contains);
            out.row("Unpacked", rows.unpacked);
            out.row("Encrypted", rows.encrypted);
            out.row("Not readable yet", rows.unsupported);
        }
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

    if (photo != nullptr && !media_details(*photo, MediaDetails::summary).empty()) {
        out.section(photo->video_codec.empty() ? i18n::tr("MEDIA") : i18n::tr("VIDEO"));
        for (const auto& [label, value] : media_details(*photo, MediaDetails::summary))
            out.row(label, value);
    }

    if (summary && summary->text) {
        for (const auto& [label, value] : content_details(*summary))
            out.row(label, value);
    }

    if (insights_ && !folder.is_archive() && !folder.is_trash() &&
        (single == nullptr || (single->is_directory() && !single->is_link))) {
        insights_path_ = path;
        out.skip(10.0F);
        insights_button_ = {out.left(), out.y(), out.width(), 30};
        renderer.draw_box(insights_button_, {.fill = palette.primary.with_alpha(0.12F),
                                             .radius = ui::Corners::all(4)});
        renderer.draw_text(i18n::tr("Folder insights…"), out.left() + 10, out.y() + 6, kTextSize,
                           palette.primary, out.width() - 20);
        out.skip(40.0F);
        insights_button_ = insights_button_.intersect(content);
    }

    // The rest, on request.
    if (info != nullptr || photo != nullptr) {
        out.skip(8.0F);
        const std::string_view label =
            expanded_ ? i18n::tr("Fewer details") : i18n::tr("More details");
        const float label_width = text_width(renderer, label, kTextSize);
        const float total = label_width + 6.0F + 12.0F;
        const float x = out.left() + ((out.width() - total) / 2.0F);
        renderer.draw_text(label, x, out.y(), kTextSize,
                           theme::readable_foreground(palette.primary, palette.surface_container));
        icons.draw(renderer, expanded_ ? "chevron-up" : "chevron-down", x + label_width + 6.0F,
                   out.y() + 2.0F, 12.0F,
                   theme::readable_foreground(palette.primary, palette.surface_container));
        toggle_ = {x - 8.0F, out.y() - 4.0F, total + 16.0F, kRowHeight};
        out.skip(kRowHeight);
        if (photo != nullptr) {
            const auto additional = media_details(*photo, MediaDetails::additional);
            if (expanded_ && !additional.empty())
                out.section("Media details");
            for (const auto& [field, value] : additional)
                out.row(field, value, ui::Typeface::proportional, expanded_);
        }
        if (info != nullptr) {
            if (expanded_)
                out.section("MORE");
            out.row("Permissions", info->permissions, ui::Typeface::monospace, expanded_);
            if (!info->owner.empty())
                out.row("Owner", info->owner + " · " + info->group, ui::Typeface::proportional,
                        expanded_);
            out.row("Location", path.parent_path().string(), ui::Typeface::proportional, expanded_);
            out.row("Link to", info->link_target, ui::Typeface::proportional, expanded_);
        }
        if (info != nullptr && !info->error.empty()) {
            out.skip(6.0F);
            out.centered("error", info->error, kSmallSize, palette.error, 20.0F);
        }
    }
    finish();
}

}  // namespace rummage::views
