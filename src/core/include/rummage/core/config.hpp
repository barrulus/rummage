#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "rummage/core/actions.hpp"
#include "rummage/core/menu_layout.hpp"
#include "rummage/core/motion.hpp"

// User settings from config.toml. Every key is optional: a missing key keeps its default, a bad
// value (wrong type, out of range) is reported and keeps its default, an unknown key is reported
// and ignored, and a file that does not parse is ignored as a whole. Reading a config never fails.
namespace rummage::core {

struct Config {
    struct Extensions {
        std::vector<std::string> enabled{};
        bool git{false};
        std::vector<std::string> git_tool{};
        bool operator==(const Extensions&) const = default;
    };

    struct Interface {
        std::string language{"system"};  // Compiled catalog code; applied at process startup.
    };

    struct Startup {
        enum class Mode { home, folder, last, session };
        Mode mode{Mode::session};
        std::string folder{"~"};
    };

    struct Font {
        static constexpr float kUsualSize = 13.0F;
        float size{kUsualSize};

        [[nodiscard]] float zoom() const noexcept { return size / kUsualSize; }

        std::string family{"sans-serif"};  // fontconfig pattern, e.g. "Inter" or "Inter:weight=500"
        std::string monospace{"monospace"};  // text previews, e.g. "JetBrains Mono"
    };

    struct Open {
        bool programs{true};
    };

    struct Photos {
        bool enabled{true};
    };

    struct Previews {
        bool media{true};
        bool pdf{true};
    };

    struct Terminal {
        std::string program;  // Empty: use desktop/environment defaults.
        std::vector<std::string> args;
    };

    struct Keyboard {
        bool type_to_jump{true};
        bool vim{false};
        bool wrap{false};
    };

    struct Windows {
        bool reuse{true};
        bool separate_settings{true};
    };

    struct Pointer {
        int double_click_ms{400};      // presses closer in time count as a double click
        int double_click_distance{4};  // ... and closer than this, in logical pixels
    };

    struct Icon {
        std::string theme{"system"};  // desktop theme, installed name, or "builtin"
        // rummage's icon in the desktop's accent, written to ~/.local/share/icons (ROADMAP 9.2);
        // false takes it away and leaves the installed one.
        bool follow_accent{true};
        // Per-place theme names, builtin:<glyph>, or absolute SVG/PNG paths.
        std::map<std::string, std::string> places;
    };

    struct Appearance {
        bool transparent_settings{false};       // Settings follows background opacity when enabled
        float background_opacity{1.0F};         // background only; foreground content stays solid
        std::string palette_source{"auto"};     // auto, desktop, file, builtin, kde
        std::string color_scheme{"system"};     // system (selected source's mode), light, dark
        std::string preview_position{"right"};  // right or sidebar
    };

    struct Sidebar {
        // Explicit lists override remembered organization at startup or on config reload.
        std::optional<std::vector<std::string>> order;
        std::optional<std::vector<std::string>> hidden;
        bool operator==(const Sidebar&) const = default;
    };

    struct StatusBar {
        float text_size{12.0F};  // logical pixels, independent of file-list zoom
    };

    std::map<std::string, std::string> keybindings;
    std::vector<Action> actions;
    MenuLayout context_menu;

    Appearance appearance;
    Interface interface;
    Extensions extensions;
    Sidebar sidebar;
    MotionConfig animations;
    Startup startup;
    StatusBar status_bar;
    Font font;
    Terminal terminal;
    Open open;
    Previews previews;
    Photos photos;
    Pointer pointer;
    Keyboard keyboard;
    Windows windows;
    Icon icon;
};

struct ConfigResult {
    Config config;
    bool from_file{false};     // the file existed and parsed; false means pure defaults
    bool actions_valid{true};  // invalid action sets are rejected together, including on reload
    bool context_menu_valid{true};
    // One line per problem, ready to print: "<file>:<line>:<column>: <what>".
    std::vector<std::string> problems;
    // Blocking subset, in the same file order. Ignored unknown keys/sections are warnings:
    // editors can preserve them while changing supported settings. Invalid values, TOML,
    // action/menu definitions, and file-read errors remain blocking.
    std::vector<std::string> validation_errors;
};

// $XDG_CONFIG_HOME/rummage/config.toml, else $HOME/.config/rummage/config.toml; empty when
// neither variable is set.
[[nodiscard]] std::filesystem::path default_config_path();

// Creates the complete reference at path only if no entry exists. Existing files and
// symlinks are untouched; no path (no HOME/XDG_CONFIG_HOME) is a no-op.
[[nodiscard]] std::error_code create_default_config(const std::filesystem::path& path);

// Parses TOML text. source names the file in problem messages.
[[nodiscard]] ConfigResult parse_config(std::string_view text, std::string_view source);

// Reads and parses a file. A missing file is not a problem: defaults, from_file = false.
[[nodiscard]] ConfigResult load_config(const std::filesystem::path& path);

// Absolute paths and ~/ paths only; no shell expansion. Empty means invalid or no HOME.
[[nodiscard]] std::filesystem::path expand_folder_path(std::string_view path);

}  // namespace rummage::core
