#pragma once

#include <array>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "rummage/fs/batch_rename.hpp"
#include "rummage/theme/palette.hpp"
#include "rummage/ui/renderer.hpp"
#include "rummage/ui/seat.hpp"
#include "rummage/ui/text_field.hpp"

namespace rummage::views {

class BatchRenameDialog {
public:
    using Apply = std::function<void(const std::vector<fs::RenameChange>&)>;
    explicit BatchRenameDialog(std::function<void()> redraw);
    void open(std::vector<fs::RenameSource> directory, std::vector<std::string> selection,
              Apply apply);
    void close();
    void open_busy(std::string message);

    [[nodiscard]] bool busy() const { return busy_; }

    [[nodiscard]] bool is_open() const { return busy_ || static_cast<bool>(apply_); }

    [[nodiscard]] std::size_t generation() const { return generation_; }

    [[nodiscard]] const fs::RenamePlan& preview() const { return preview_; }

    [[nodiscard]] bool can_apply() const {
        return preview_.error.empty() && preview_.changes_anything();
    }

    bool key(const ui::KeyEvent& event);
    void paste(std::string_view text);
    std::string copy_selection(bool cut);
    void pointer(const ui::PointerEvent& event);
    void draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette);
    [[nodiscard]] ui::CursorShape cursor_at(float x, float y) const;

private:
    void update();
    void submit();
    void scroll(float delta);
    std::function<void()> redraw_;
    Apply apply_;
    bool busy_{false};
    std::string busy_message_;
    std::vector<fs::RenameSource> directory_;
    std::vector<std::string> selection_;
    std::array<ui::TextField, 6> fields_;
    fs::RenamePlan preview_;
    std::size_t generation_{0};
    std::size_t focus_{0};  // six fields, preview, Cancel, Rename
    float offset_{0};
    float max_scroll_{0};
    float form_offset_{0};
    float max_form_scroll_{0};
    ui::Rect body_{};
    ui::Rect list_{};
    ui::Rect cancel_{};
    ui::Rect apply_button_{};
};

}  // namespace rummage::views
