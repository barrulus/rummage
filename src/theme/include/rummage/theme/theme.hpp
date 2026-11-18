#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/file_watcher.hpp"
#include "rummage/theme/desktop.hpp"
#include "rummage/theme/palette.hpp"

namespace rummage::theme {

// Palette sources stay current independently of the selected source. Invalid file updates
// retain the last usable file palette; removing the file makes that source unavailable.
class Theme {
public:
    using ChangeHandler = std::function<void()>;
    enum class Source { automatic, desktop, file, builtin };

    enum class FileState { missing, valid, invalid };

    struct Status {
        Source requested;
        Source active;  // never automatic; the source before any light/dark override
        std::optional<bool> dark_override;
        FileState file_state{FileState::missing};
        std::vector<std::string> file_problems{};
        bool retaining_last_valid_file{false};
    };

    explicit Theme(std::filesystem::path path);

    // Starts following the file. The theme must not outlive the loop.
    void watch(core::EventLoop& loop);

    // What the desktop says about its look; nullopt when it says nothing (no portal).
    void set_desktop(std::optional<Desktop> desktop);
    // Automatic prefers file, then desktop, then built-in. Explicit sources fall back to
    // built-in when unavailable, without changing the preference or touching colors.toml.
    void set_source(Source source);
    [[nodiscard]] Source active_source() const;

    [[nodiscard]] Status status() const {
        return {source_,        active_source(),
                dark_override_, file_state_,
                file_problems_, file_state_ == FileState::invalid && file_palette_.has_value()};
    }

    // A Settings preference. nullopt restores the selected source's exact palette.
    void set_dark_override(std::optional<bool> dark);

    // Called after the palette or source diagnostics change.
    void set_change_handler(ChangeHandler handler) { change_handler_ = std::move(handler); }

    [[nodiscard]] const Palette& palette() const noexcept {
        return dark_override_ ? override_palette_ : palette_;
    }

    // The active source's accent: file primary, desktop accent, or nullopt for built-in.
    [[nodiscard]] std::optional<Color> accent() const;

    // Reads the file again; public so the reload rules can be tested without inotify.
    void reload();

private:
    void changed();

    std::filesystem::path path_;
    Source source_{Source::automatic};
    FileState file_state_{FileState::missing};
    std::vector<std::string> file_problems_;
    std::optional<Palette> file_palette_;
    std::optional<Desktop> desktop_;
    Palette palette_;
    Palette override_palette_;
    std::optional<bool> dark_override_;
    ChangeHandler change_handler_;
    std::unique_ptr<core::FileWatcher> watcher_;
};

}  // namespace rummage::theme
