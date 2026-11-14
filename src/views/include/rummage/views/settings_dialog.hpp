#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "rummage/core/config.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/theme/palette.hpp"
#include "rummage/ui/text_field.hpp"

namespace rummage::views {

class SettingsDialog {
public:
    SettingsDialog(core::WorkerPool& pool, std::function<void()> redraw);
    ~SettingsDialog();
    void open(std::filesystem::path path, std::function<void()> open_file,
              std::function<void(const core::ConfigResult&)> applied);
    void close();

    bool is_open() const { return open_; }

    bool busy() const { return busy_; }

    std::uint64_t generation() const { return generation_; }

    const std::string& error() const { return error_; }

    bool key(const ui::KeyEvent& event);
    void pointer(const ui::PointerEvent& event);
    ui::CursorShape cursor_at(float x, float y) const;
    void paste(std::string_view text);
    std::string copy_selection(bool cut);
    void draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette);
    void save();
    // Shared by controls and focused tests.
    bool edit(std::string_view table, std::string_view key, std::string value);
    void add_action();

private:
    struct Field {
        int section;
        std::string table, key, label, original;
        bool raw{false};
        ui::TextField input;
        std::vector<std::string> choices;
        ui::Rect bounds{};
    };

    struct Button {
        std::string id, label;
        ui::Rect bounds;
    };

    void populate(const core::ConfigResult& config);
    void activate(std::string_view id);
    void change_section(int section);
    void reveal_focus();
    core::WorkerPool& pool_;
    std::function<void()> redraw_, open_file_;
    std::function<void(const core::ConfigResult&)> applied_;
    core::CancelToken job_;
    std::filesystem::path path_;
    std::string original_, error_;
    bool open_{false}, busy_{false}, existed_{false}, valid_{false};
    std::uint64_t generation_{0};
    std::size_t action_count_{0}, new_actions_{0};
    std::vector<Field> fields_;
    std::vector<Button> buttons_;
    int section_{0};
    std::string focus_{"section0"};
    float scroll_{0}, max_scroll_{0};
    ui::Rect list_{};
};
}  // namespace rummage::views
