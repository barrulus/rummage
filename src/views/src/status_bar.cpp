#include "rummage/views/status_bar.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <string_view>
#include <sys/statvfs.h>
#include <utility>

#include "rummage/core/i18n.hpp"
#include "rummage/views/format.hpp"
#include "rummage/views/shortcuts.hpp"

namespace rummage::views {
namespace {

constexpr float kPadding = 16.0F;
constexpr float kButtonWidth = 30.0F;
constexpr float kButtonHeight = 22.0F;
constexpr float kButtonGap = 2.0F;
constexpr float kHelpGap = 10.0F;  // before transfer cancellation
constexpr float kIconSize = 16.0F;
constexpr float kProgressHeight = 2.0F;
constexpr std::array<std::string_view, 2> kIcons{"settings", "help"};

}  // namespace

std::optional<FreeSpace> free_space_of(const std::filesystem::path& path) {
    struct statvfs info{};
    if (statvfs(path.c_str(), &info) != 0) {
        return std::nullopt;
    }
    return FreeSpace{.free = static_cast<std::uint64_t>(info.f_bavail) * info.f_frsize,
                     .total = static_cast<std::uint64_t>(info.f_blocks) * info.f_frsize};
}

std::string status_of(const Folder& folder) {
    const fs::DirectoryModel& model = folder.model();
    const std::size_t count = folder.entries().size();
    if (model.error()) {
        return model.problem();
    }
    const std::string prefix = model.is_archive()
                                   ? i18n::trf("{} archive  ·  read-only  ·  ",
                                               archive::format_name(model.archive_format()))
                                   : std::string{};
    if (folder.selected_count() > 1) {
        return prefix + i18n::trf("{} of {} selected", folder.selected_count(), count);
    }
    if (model.loading()) {
        return prefix + i18n::trn("{} item, loading…", "{} items, loading…", count);
    }
    std::string status = prefix;
    if (const auto* selected = folder.single_selection()) {
        status += selected->name;
    } else if (folder.selected_count() == 1) {
        // Ctrl+navigation can move the cursor away from the only selected entry.
        const auto names = folder.selected_names();
        if (!names.empty())
            status += names.front();
    } else {
        status += i18n::trn("{} item", "{} items", count);
        if (const std::size_t hidden = folder.hidden_count(); hidden > 0)
            status += i18n::trf(", {} hidden", hidden);
    }
    if (const auto problem = model.problem(); !problem.empty())
        status += "  ·  " + problem;
    return status;
}

StatusBar::StatusBar(core::EventLoop& loop, core::WorkerPool& pool, std::function<void()> redraw,
                     std::function<void()> settings, std::function<void()> help)
    : loop_{loop},
      pool_{pool},
      redraw_{std::move(redraw)},
      settings_{std::move(settings)},
      help_{std::move(help)} {}

StatusBar::~StatusBar() {
    *alive_ = false;
    loop_.remove(message_timer_);
    if (job_) {
        job_->cancel();
    }
}

void StatusBar::update(const Folder& folder) {
    if (job_ || (known_ && path_ == folder.path() && revision_ == folder.model().revision())) {
        return;  // on its way, or known: a draw after the result checks again
    }
    if (folder.is_archive()) {
        // No free space inside an archive: it means nothing there.
        path_ = folder.path();
        revision_ = folder.model().revision();
        known_ = true;
        free_.reset();
        return;
    }
    path_ = folder.path();
    revision_ = folder.model().revision();
    known_ = true;
    job_ = pool_.submit(
        core::Priority::visible,
        [path = path_](const core::CancelToken& /*token*/) { return free_space_of(path); },
        [this, alive = alive_](std::optional<FreeSpace> space) {
            if (!*alive) {
                return;
            }
            job_.reset();
            free_ = space;
            redraw_();
        });
}

void StatusBar::show_message(std::string text, bool error) {
    loop_.remove(message_timer_);
    message_ = std::move(text);
    message_error_ = error;
    message_timer_ = loop_.add_timer(kMessageTime, std::chrono::milliseconds{0}, [this] {
        message_timer_ = 0;
        message_.clear();
        redraw_();
    });
    redraw_();
}

void StatusBar::set_transfer(std::optional<TransferStatus> transfer) {
    transfer_ = std::move(transfer);
    if (!transfer_) {
        cancel_button_ = {};
        cancel_hovered_ = false;
    }
    redraw_();
}

void StatusBar::set_text_size(float size) {
    if (!std::isfinite(size) || size < 8.0F || size > 32.0F || size == text_size_) {
        return;
    }
    text_size_ = size;
    redraw_();
}

float StatusBar::height(ui::Renderer& renderer) const {
    const auto& line = renderer.fonts().layout("Mg", text_size_, renderer.scale());
    return std::max(kHeight, (line.ascent + line.descent) / renderer.scale() + 10.0F);
}

void StatusBar::draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
                     ui::IconSet& icons, const Folder& folder, bool settings_enabled) {
    bounds_ = bounds;
    renderer.push_clip(bounds);
    update(folder);
    renderer.fill_rect({bounds.x, bounds.y, bounds.width, 1.0F / renderer.scale()},
                       palette.outline_variant);

    // One shared Settings button and shortcut help, aligned to the bottom right.
    const float top = bounds.y + ((bounds.height - kButtonHeight) / 2.0F);
    float x = bounds.right() - kPadding + kButtonGap;
    for (std::size_t i = buttons_.size(); i-- > 0;) {
        buttons_[i] = {};
        if (i == 0 && !settings_enabled)
            continue;
        x -= kButtonWidth + kButtonGap;
        const ui::Rect button{x, top, kButtonWidth, kButtonHeight};
        buttons_[i] = button;
        if (hovered_ == i)
            renderer.draw_box(button, {.fill = palette.surface_container_high, .radius = {}});
        const auto color = hovered_ == i ? palette.on_surface : palette.on_surface_variant;
        icons.draw(renderer, kIcons.at(i), button.x + ((kButtonWidth - kIconSize) / 2.0F),
                   button.y + ((kButtonHeight - kIconSize) / 2.0F), kIconSize, color);
    }

    // A transfer: its progress along the top edge, a button to stop it before Settings.
    float text_right = x - kPadding;
    if (transfer_) {
        if (transfer_->fraction >= 0.0F) {
            renderer.fill_rect(
                {bounds.x, bounds.y, bounds.width * std::clamp(transfer_->fraction, 0.0F, 1.0F),
                 kProgressHeight},
                palette.primary);
        }
        cancel_button_ = {x - kHelpGap - kButtonWidth, top, kButtonWidth, kButtonHeight};
        if (cancel_hovered_) {
            renderer.draw_box(cancel_button_,
                              {.fill = palette.surface_container_high, .radius = {}});
        }
        icons.draw(renderer, "x", cancel_button_.x + ((kButtonWidth - kIconSize) / 2.0F),
                   cancel_button_.y + ((kButtonHeight - kIconSize) / 2.0F), kIconSize,
                   cancel_hovered_ ? palette.error : palette.on_surface_variant);
        text_right = cancel_button_.x - kPadding;
        const std::string stop_key = i18n::keys(keys_for(Command::stop_transfer));
        if (!stop_key.empty()) {
            const auto& key_line = renderer.fonts().layout(stop_key, text_size_, renderer.scale());
            const float key_width = key_line.width / renderer.scale();
            const float key_height = (key_line.ascent + key_line.descent) / renderer.scale();
            const float key_x = cancel_button_.x - 6.0F - key_width;
            renderer.draw_layout(key_line, key_x, bounds.y + (bounds.height - key_height) / 2.0F,
                                 palette.outline);
            text_right = key_x - kPadding;
        }
    }

    // The message, else a transfer, else the status and the free space; cut in the middle when
    // the buttons leave too little room.
    const float room = std::max(1.0F, text_right - (bounds.x + kPadding));
    std::string text = message_;
    ui::Color color = message_error_ ? palette.error : palette.on_surface;
    if (message_.empty() && transfer_) {
        text = transfer_->text;
        color = palette.on_surface;
    } else if (message_.empty()) {
        text = status_of(folder);
        if (free_ && !folder.model().error()) {
            const auto with_space = text + i18n::trf("  ·  {} free of {}", format_size(free_->free),
                                                     format_size(free_->total));
            // Give the selected filename the full bar before truncating it for disk usage.
            if (folder.selected_count() != 1 ||
                renderer.fonts().layout(with_space, text_size_, renderer.scale()).width /
                        renderer.scale() <=
                    room)
                text = with_space;
        }
        color = !folder.model().problem().empty() ? palette.error : palette.on_surface_variant;
    }
    const auto& line =
        renderer.fonts().layout(text, text_size_, renderer.scale(), room, ui::Ellipsis::middle);
    const float line_height = (line.ascent + line.descent) / renderer.scale();
    renderer.draw_layout(line, bounds.x + kPadding, bounds.y + (bounds.height - line_height) / 2.0F,
                         color);
    renderer.pop_clip();
}

std::optional<std::size_t> StatusBar::button_at(float x, float y) const {
    for (std::size_t i = 0; i < buttons_.size(); ++i) {
        const ui::Rect& button = buttons_.at(i);
        if (x >= button.x && x < button.right() && y >= button.y && y < button.bottom()) {
            return i;
        }
    }
    return std::nullopt;
}

bool StatusBar::clickable_at(float x, float y) const {
    return button_at(x, y).has_value() || cancel_button_.contains(x, y);
}

bool StatusBar::claims(float x, float y) const {
    return !bounds_.empty() && x >= bounds_.x && x < bounds_.right() && y >= bounds_.y &&
           y < bounds_.bottom();
}

void StatusBar::pointer(const ui::PointerEvent& event) {
    using Kind = ui::PointerEvent::Kind;
    const bool on_cancel = event.kind != Kind::leave && !cancel_button_.empty() &&
                           event.x >= cancel_button_.x && event.x < cancel_button_.right() &&
                           event.y >= cancel_button_.y && event.y < cancel_button_.bottom();
    if (on_cancel != cancel_hovered_) {
        cancel_hovered_ = on_cancel;
        redraw_();
    }
    if (on_cancel && event.kind == Kind::press && event.button == ui::PointerButton::left) {
        if (cancel_) {
            cancel_();
        }
        return;
    }
    const std::optional<std::size_t> over =
        event.kind == Kind::leave ? std::nullopt : button_at(event.x, event.y);
    if (over != hovered_) {
        hovered_ = over;
        redraw_();
    }
    if (event.kind != Kind::press || event.button != ui::PointerButton::left || !over) {
        return;
    }
    if (*over == 1) {
        if (help_) {
            help_();
        }
    } else if (settings_) {
        settings_();
    }
}

}  // namespace rummage::views
