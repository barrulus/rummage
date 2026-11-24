#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "rummage/core/event_loop.hpp"
#include "rummage/picture/image.hpp"

namespace rummage::ui {

// Desktop settings and icon search roots. Explicit roots make lookup testable.
[[nodiscard]] std::string desktop_icon_theme();
[[nodiscard]] std::vector<std::filesystem::path> icon_theme_roots();

// Installed, visible icon themes, sorted and deduplicated. Call on a worker.
[[nodiscard]] std::vector<std::string> installed_icon_themes(
    const std::vector<std::filesystem::path>& roots);

// Follows the same GTK/KDE/qt6ct sources as desktop_icon_theme(), without creating files or
// directories. Handles missing sources, atomic replacement and symlinks. Loop thread only;
// the loop must outlive the watcher. The handler runs only when the resolved name changes.
class DesktopIconThemeWatcher {
public:
    DesktopIconThemeWatcher(core::EventLoop& loop, std::function<void(const std::string&)> changed);
    ~DesktopIconThemeWatcher();
    DesktopIconThemeWatcher(const DesktopIconThemeWatcher&) = delete;
    DesktopIconThemeWatcher& operator=(const DesktopIconThemeWatcher&) = delete;

    [[nodiscard]] const std::string& theme() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Freedesktop icon themes: size selection, split roots, inheritance and hicolor fallback.
// No GL resources. SVG/PNG rendering is for theme assets and explicit Places configuration.
class IconTheme {
public:
    IconTheme(std::string name, std::vector<std::filesystem::path> roots);
    [[nodiscard]] std::filesystem::path find(std::string_view icon, int pixels) const;
    // Names of available folder icons, deduplicated across sizes, roots and inheritance.
    [[nodiscard]] std::vector<std::string> folder_icons() const;
    [[nodiscard]] static picture::Image render(const std::filesystem::path& file, int pixels);

private:
    struct Directory {
        std::string name;
        int min{0};
        int max{0};
    };

    struct Theme {
        std::string name;
        std::vector<Directory> directories;
    };

    void add_theme(const std::string& name);
    std::vector<std::filesystem::path> roots_;
    std::vector<Theme> themes_;
};

}  // namespace rummage::ui
