#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "rummage/core/event_loop.hpp"

namespace rummage::platform {
class EmbeddedTerminal {
public:
    enum class Key {
        none,
        enter,
        tab,
        backspace,
        escape,
        up,
        down,
        left,
        right,
        home,
        end,
        insert,
        del,
        page_up,
        page_down,
        f1,
        f2,
        f3,
        f4,
        f5,
        f6,
        f7,
        f8,
        f9,
        f10,
        f11,
        f12
    };

    enum class ColorRole : std::uint8_t { explicit_rgb, foreground, background };

    struct Cell {
        std::string text;
        std::uint32_t fg{0xd8dee9}, bg{0x11131a};
        // Preserve defaults independently of their RGB values so live themes and reverse
        // video can resolve them without changing explicit ANSI colors.
        ColorRole fg_role{ColorRole::foreground}, bg_role{ColorRole::background};
        int width{1};
        bool underline{false};
    };

    struct Screen {
        int rows{}, columns{}, cursor_row{}, cursor_column{};
        bool cursor_visible{};
        std::vector<Cell> cells;
    };

    EmbeddedTerminal(core::EventLoop& loop, std::function<void()> changed);
    ~EmbeddedTerminal();
    EmbeddedTerminal(const EmbeddedTerminal&) = delete;
    EmbeddedTerminal& operator=(const EmbeddedTerminal&) = delete;
    static bool available();
    std::string start(const std::filesystem::path& directory, std::filesystem::path shell = {},
                      std::filesystem::path helper = {});
    void close();
    void resize(int rows, int columns);
    void key(Key key, std::string_view text, bool ctrl = false, bool alt = false,
             bool shift = false);
    void paste(std::string_view text);
    void scroll(int lines);
    [[nodiscard]] Screen screen() const;
    [[nodiscard]] std::string text() const;
    [[nodiscard]] bool running() const;
    [[nodiscard]] bool ready() const;
    [[nodiscard]] bool directory_pending() const;
    [[nodiscard]] bool supports_sync() const;
    [[nodiscard]] std::filesystem::path directory() const;
    // Latest browser directory is queued, but entered only at a clean, idle supported prompt.
    void request_directory(const std::filesystem::path& directory);
    // Executes explicit user-requested shell code only at that same clean idle prompt.
    bool command(std::string_view script);
    [[nodiscard]] std::filesystem::path session_directory() const;
    [[nodiscard]] const std::string& error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Shell-safe argument quoting; commands and selections never share a parsing layer.
std::string shell_argument(std::string_view value);
}  // namespace rummage::platform
