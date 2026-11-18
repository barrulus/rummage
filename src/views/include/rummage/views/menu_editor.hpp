#pragma once

#include "rummage/core/config.hpp"
#include "rummage/core/config_edit.hpp"
#include "rummage/ui/scrollbar.hpp"
#include "rummage/ui/text_field.hpp"
#include "rummage/views/context_menu.hpp"

namespace rummage::views {
// Edits the existing declarative layout; runtime menus still own applicability and callbacks.
class MenuEditor {
public:
    using Apply =
        std::function<std::string(const std::vector<core::ConfigEdit>&, std::string_view)>;
    MenuEditor(std::function<void()> redraw, Apply apply);
    void sync(const core::Config& config, bool editable);
    void context(std::size_t index);
    bool add(core::MenuNode node);
    bool remove();
    bool move(int direction);
    bool nest();
    bool unnest();
    bool group(std::string label);
    bool rename(std::string label);
    bool reset();
    void select(std::size_t row);

    const std::vector<core::MenuNode>& nodes() const { return nodes_; }

    const std::string& error() const { return error_; }

    bool key(const ui::KeyEvent& event);
    void pointer(const ui::PointerEvent& event);
    void paste(std::string_view text);
    std::string copy_selection(bool cut);
    void draw(ui::Renderer&, const ui::Rect&, const theme::Palette&);
    bool has_draft() const;
    bool commit();
    void discard();
    void dismiss();
    void enter_focus(bool reverse = false);
    void blur();

    bool popup_open() const { return popup_.is_open(); }

    // Read-only representatives: labels match the browser; preview items never execute commands.
    std::vector<MenuItem> command_catalog() const;
    std::vector<MenuItem> preview_items() const;
    static std::string serialize(const std::vector<core::MenuNode>& nodes);

private:
    using Path = std::vector<std::size_t>;

    struct Row {
        Path path;
        ui::Rect bounds;
    };

    struct Button {
        std::string id, label;
        ui::Rect bounds;
        bool enabled{true};
    };

    void flatten();
    std::vector<core::MenuNode>* siblings(const Path& path);
    core::MenuNode* selected();
    const core::MenuNode* selected() const;
    bool renameable() const;
    void select_node(const core::MenuNode& node);
    void layout_controls();
    void reveal_focus();
    std::vector<std::string> focus_order() const;
    ui::Rect anchor(std::string_view id) const;
    bool publish(std::vector<core::MenuNode> previous);
    void activate(std::string_view id);
    void picker(bool keyboard = false);
    std::vector<MenuItem> available() const;
    std::function<void()> redraw_;
    Apply apply_;
    core::Config config_;
    std::vector<core::MenuNode> nodes_;
    std::vector<Row> rows_;
    std::vector<Button> buttons_;
    std::size_t context_{0}, selected_{0};
    bool editable_{false}, inherited_{true}, label_focus_{false}, initialized_{false};
    bool reveal_pending_{false};
    std::string error_, original_label_, focus_{"context"};
    ui::TextField label_;
    ui::Scroller scroll_;
    ui::Scrollbar scrollbar_;
    ui::Rect bounds_{}, list_{}, label_bounds_{};
    float note_y_{0}, error_y_{0};
    ContextMenu popup_;
};
}  // namespace rummage::views
