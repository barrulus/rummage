#include "rummage/theme/theme.hpp"

#include <string>
#include <utility>

#include "rummage/core/log.hpp"

namespace rummage::theme {
namespace {

void log_problems(const PaletteResult& result) {
    for (const std::string& problem : result.problems) {
        log::warn("theme: {}", problem);
    }
}

}  // namespace

Theme::Theme(std::filesystem::path path) : path_{std::move(path)} {
    const PaletteResult result = load_palette(path_);
    log_problems(result);
    file_problems_ = result.problems;
    file_state_ = result.from_file          ? FileState::valid
                  : result.problems.empty() ? FileState::missing
                                            : FileState::invalid;
    if (result.from_file)
        file_palette_ = result.palette;
    changed();
    log::info("theme: {}", result.from_file ? path_.string() : "default palette");
}

void Theme::set_desktop(std::optional<Desktop> desktop) {
    if (desktop == desktop_) {
        return;
    }
    const bool contrast_changed =
        (desktop_ && desktop_->high_contrast) != (desktop && desktop->high_contrast);
    desktop_ = desktop;
    if (desktop_) {
        log::info("theme: desktop is {}{}", desktop_->dark ? "dark" : "light",
                  desktop_->accent ? ", with an accent colour" : "");
    }
    if (source_ == Source::desktop || (source_ == Source::automatic && !file_palette_) ||
        (dark_override_ && contrast_changed))
        changed();
}

std::optional<Color> Theme::accent() const {
    if (active_source() == Source::file)
        return file_palette_->primary.with_alpha(1.0F);
    return active_source() == Source::desktop ? desktop_->accent : std::nullopt;
}

Theme::Source Theme::active_source() const {
    if ((source_ == Source::automatic || source_ == Source::file) && file_palette_)
        return Source::file;
    if ((source_ == Source::automatic || source_ == Source::desktop) && desktop_)
        return Source::desktop;
    return Source::builtin;
}

void Theme::set_source(Source source) {
    if (source == source_)
        return;
    source_ = source;
    changed();
}

void Theme::set_dark_override(std::optional<bool> dark) {
    if (dark == dark_override_)
        return;
    dark_override_ = dark;
    changed();
}

void Theme::changed() {
    switch (active_source()) {
        case Source::file:
            palette_ = *file_palette_;
            break;
        case Source::desktop:
            palette_ = desktop_palette(*desktop_);
            break;
        case Source::builtin:
        case Source::automatic:
            palette_ = Palette{};
            break;
    }
    if (dark_override_)
        override_palette_ = desktop_palette({.dark = *dark_override_,
                                             .accent = accent(),
                                             .high_contrast = desktop_ && desktop_->high_contrast});
    if (change_handler_) {
        change_handler_();
    }
}

void Theme::watch(core::EventLoop& loop) {
    if (path_.empty()) {
        return;
    }
    watcher_ = std::make_unique<core::FileWatcher>(loop, path_, [this] { reload(); });
}

void Theme::reload() {
    const PaletteResult result = load_palette(path_);
    log_problems(result);
    file_problems_ = result.problems;
    file_state_ = result.from_file          ? FileState::valid
                  : result.problems.empty() ? FileState::missing
                                            : FileState::invalid;
    if (result.from_file) {
        log::info("theme: reloaded {}", path_.string());
    } else if (result.problems.empty()) {
        log::info("theme: {} removed; palette file unavailable", path_.string());
    } else {
        log::warn("theme: keeping the current palette");
        if (change_handler_)
            change_handler_();
        return;
    }
    file_palette_ = result.from_file ? std::optional{result.palette} : std::nullopt;
    if (source_ == Source::automatic || source_ == Source::file)
        changed();
    else if (change_handler_)
        change_handler_();
}

}  // namespace rummage::theme
