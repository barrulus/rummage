#include "rummage/views/menu_editor.hpp"

#include <algorithm>
#include <array>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "rummage/core/actions.hpp"
#include "rummage/core/i18n.hpp"
#include "rummage/views/details_columns.hpp"
#include "rummage/views/dialog_chrome.hpp"

namespace rummage::views {
namespace {
constexpr std::array keys{"items", "entry", "background", "trash", "sidebar"};
constexpr std::array names{"Common layout", "Files and folders", "Folder background", "Trash",
                           "Sidebar"};

enum ContextMask : unsigned { entry = 2, background = 4, trash = 8, sidebar = 16 };

struct CommandLabel {
    std::string_view id, label;
    unsigned contexts;
};

constexpr std::array commands{
    CommandLabel{"open", "Open", entry | sidebar},
    CommandLabel{"open_with", "Open With", entry},
    CommandLabel{"open_tab", "Open in New Tab", entry | sidebar},
    CommandLabel{"open_terminal", "Open in Terminal", entry | background | sidebar},
    CommandLabel{"running", "Allow Running as Program", entry},
    CommandLabel{"copy_path", "Copy Path", entry | background | sidebar},
    CommandLabel{"compress_zip", "Compress to ZIP…", entry},
    CommandLabel{"compress_tar", "Compress to tar.gz…", entry},
    CommandLabel{"add_bookmark", "Add to Sidebar", entry | background},
    CommandLabel{"copy", "Copy", entry},
    CommandLabel{"cut", "Cut", entry},
    CommandLabel{"paste", "Paste", entry | background},
    CommandLabel{"rename", "Rename…", entry},
    CommandLabel{"create_link", "Create Link", entry},
    CommandLabel{"move_into_folder", "Move into New Folder…", entry},
    CommandLabel{"reset_folder_view", "Use Default View for This Folder", entry | background},
    CommandLabel{"trash", "Move to Trash", entry},
    CommandLabel{"erase", "Delete Permanently…", entry | trash},
    CommandLabel{"restore", "Restore", trash},
    CommandLabel{"empty_trash", "Empty Trash…", trash | sidebar},
    CommandLabel{"new_folder", "New Folder", background},
    CommandLabel{"new_file", "New File", background},
    CommandLabel{"properties", "Properties…", entry | background | trash | sidebar},
    CommandLabel{"sort", "Sort By", entry | background},
    CommandLabel{"panes", "Panes", entry | background},
    CommandLabel{"actions", "Actions", entry | background | trash},
    CommandLabel{"mount", "Mount", sidebar},
    CommandLabel{"unmount", "Unmount", sidebar},
    CommandLabel{"eject", "Eject", sidebar},
    CommandLabel{"open_window", "Open in New Window", sidebar},
    CommandLabel{"sidebar_move_up", "Move Up", sidebar},
    CommandLabel{"sidebar_move_down", "Move Down", sidebar},
    CommandLabel{"sidebar_separator", "Add Separator Above", sidebar},
    CommandLabel{"sidebar_remove_separator", "Remove Separator", sidebar},
    CommandLabel{"sidebar_hide", "Hide from Sidebar", sidebar},
    CommandLabel{"sidebar_options", "Sidebar", sidebar},
    CommandLabel{"remove_bookmark", "Remove from Sidebar", sidebar},
    CommandLabel{"rename_bookmark", "Rename Bookmark…", sidebar},
    CommandLabel{"choose_bookmark_folder", "Choose Folder…", sidebar},
    CommandLabel{"move_bookmark_up", "Move Bookmark Up", sidebar},
    CommandLabel{"move_bookmark_down", "Move Bookmark Down", sidebar},
};
constexpr std::array pane_commands{
    CommandLabel{"toggle_split", "Show Two Panes", entry | background},
    CommandLabel{"switch_pane", "Switch Pane", entry | background},
    CommandLabel{"swap_panes", "Swap Panes", entry | background},
    CommandLabel{"orient_panes", "Arrange Top / Bottom", entry | background},
    CommandLabel{"open_other_pane", "Open in Other Pane", entry | background | sidebar},
    CommandLabel{"same_other_pane", "Show Same Folder in Other Pane", entry | background},
    CommandLabel{"copy_other_pane", "Copy to Other Pane", entry},
    CommandLabel{"move_other_pane", "Move to Other Pane", entry},
};

MenuItem command_item(std::string_view id, std::string_view label) {
    MenuItem item;
    item.id = id;
    item.label =
        id == "open" ? std::string{i18n::trc("file-action", "Open")} : std::string{i18n::tr(label)};
    return item;
}

const std::optional<std::vector<core::MenuNode>>& layout(const core::MenuLayout& m,
                                                         std::size_t index) {
    switch (index) {
        case 1:
            return m.entry;
        case 2:
            return m.background;
        case 3:
            return m.trash;
        case 4:
            return m.sidebar;
        default:
            return m.items;
    }
}

std::string title(const core::MenuNode& node, const std::vector<MenuItem>& catalog) {
    if (node.remaining)
        return std::string{i18n::tr("Remaining commands")};
    if (!node.label.empty())
        return node.label;
    if (!node.id.empty()) {
        const auto command = std::ranges::find(catalog, node.id, &MenuItem::id);
        return command == catalog.end() ? std::string{i18n::tr("Unavailable command")}
                                        : command->label;
    }
    return std::string{i18n::tr("Separator")};
}

void prune(std::vector<core::MenuNode>& nodes) {
    for (auto& node : nodes)
        prune(node.items);
    std::erase_if(
        nodes, [](const auto& n) { return n.id.empty() && !n.label.empty() && n.items.empty(); });
}
}  // namespace

MenuEditor::MenuEditor(std::function<void()> redraw, Apply apply)
    : redraw_{std::move(redraw)}, apply_{std::move(apply)}, popup_{redraw_} {}

std::string MenuEditor::serialize(const std::vector<core::MenuNode>& nodes) {
    std::string out = "[";
    for (const auto& node : nodes) {
        if (out.size() > 1)
            out += ", ";
        out += "{";
        std::string fields;
        const auto field = [&](std::string value) {
            if (!fields.empty())
                fields += ", ";
            fields += value;
        };
        if (!node.id.empty())
            field("id = " + core::quote_config_string(node.id));
        if (!node.label.empty())
            field("label = " + core::quote_config_string(node.label));
        if (!node.items.empty())
            field("items = " + serialize(node.items));
        if (node.separator)
            field("separator = true");
        if (node.remaining)
            field("remaining = true");
        out += fields + "}";
    }
    return out + "]";
}

void MenuEditor::sync(const core::Config& config, bool editable) {
    const auto old =
        serialize(layout(config_.context_menu, context_).value_or(std::vector<core::MenuNode>{}));
    const bool old_present = layout(config_.context_menu, context_).has_value();
    const bool fallback_changed =
        serialize(config_.context_menu.items.value_or(std::vector<core::MenuNode>{})) !=
        serialize(config.context_menu.items.value_or(std::vector<core::MenuNode>{}));
    config_ = config;
    editable_ = editable;
    const auto& current = layout(config_.context_menu, context_);
    if (!initialized_ || old_present != current.has_value() ||
        old != serialize(current.value_or(std::vector<core::MenuNode>{})) ||
        (inherited_ && fallback_changed)) {
        initialized_ = true;
        inherited_ = !current;
        nodes_ = current.value_or(
            config_.context_menu.items.value_or(std::vector<core::MenuNode>{{.remaining = true}}));
        flatten();
        select(selected_);
    }
}

void MenuEditor::context(std::size_t index) {
    if (index >= keys.size() || !commit())
        return;
    initialized_ = true;
    context_ = index;
    const auto& current = layout(config_.context_menu, context_);
    inherited_ = !current;
    nodes_ = current.value_or(
        config_.context_menu.items.value_or(std::vector<core::MenuNode>{{.remaining = true}}));
    flatten();
    select(0);
    scroll_.jump_to(0);
    redraw_();
}

void MenuEditor::flatten() {
    rows_.clear();
    const auto walk = [&](const auto& self, const auto& nodes, Path path) -> void {
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            auto next = path;
            next.push_back(i);
            rows_.push_back({next, {}});
            self(self, nodes[i].items, next);
        }
    };
    walk(walk, nodes_, {});
    selected_ = rows_.empty() ? 0 : std::min(selected_, rows_.size() - 1);
}

std::vector<core::MenuNode>* MenuEditor::siblings(const Path& path) {
    auto* list = &nodes_;
    for (std::size_t i = 0; i + 1 < path.size(); ++i)
        list = &(*list)[path[i]].items;
    return list;
}

core::MenuNode* MenuEditor::selected() {
    if (selected_ >= rows_.size())
        return nullptr;
    const auto& path = rows_[selected_].path;
    return &(*siblings(path))[path.back()];
}

const core::MenuNode* MenuEditor::selected() const {
    if (selected_ >= rows_.size())
        return nullptr;
    const auto& path = rows_[selected_].path;
    const auto* nodes = &nodes_;
    for (std::size_t i = 0; i + 1 < path.size(); ++i)
        nodes = &(*nodes)[path[i]].items;
    return &(*nodes)[path.back()];
}

bool MenuEditor::renameable() const {
    const auto* node = selected();
    return node && !node->remaining && (!node->id.empty() || !node->items.empty());
}

void MenuEditor::select_node(const core::MenuNode& node) {
    const auto wanted = serialize({node});
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        const auto& path = rows_[i].path;
        if (serialize({(*siblings(path))[path.back()]}) == wanted) {
            select(i);
            return;
        }
    }
}

void MenuEditor::select(std::size_t row) {
    selected_ = rows_.empty() ? 0 : std::min(row, rows_.size() - 1);
    const auto* node = selected();
    original_label_ = node ? node->label : "";
    label_.set_text(original_label_);
    label_focus_ = focus_ == "label" && renameable();
    reveal_pending_ = true;
    redraw_();
}

bool MenuEditor::publish(std::vector<core::MenuNode> previous) {
    if (!editable_) {
        nodes_ = std::move(previous);
        return false;
    }
    error_ = apply_({{"context_menu", keys[context_], serialize(nodes_)}}, {});
    if (!error_.empty())
        nodes_ = std::move(previous);
    else {
        switch (context_) {
            case 1:
                config_.context_menu.entry = nodes_;
                break;
            case 2:
                config_.context_menu.background = nodes_;
                break;
            case 3:
                config_.context_menu.trash = nodes_;
                break;
            case 4:
                config_.context_menu.sidebar = nodes_;
                break;
            default:
                config_.context_menu.items = nodes_;
                break;
        }
        inherited_ = false;
    }
    flatten();
    select(selected_);
    redraw_();
    return error_.empty();
}

bool MenuEditor::add(core::MenuNode node) {
    auto before = nodes_;
    nodes_.push_back(std::move(node));
    return publish(std::move(before));
}

bool MenuEditor::remove() {
    if (!selected())
        return false;
    auto before = nodes_;
    const auto path = rows_[selected_].path;
    auto* list = siblings(path);
    list->erase(list->begin() + static_cast<std::ptrdiff_t>(path.back()));
    prune(nodes_);
    return publish(std::move(before));
}

bool MenuEditor::move(int direction) {
    if (!selected())
        return false;
    const auto path = rows_[selected_].path;
    auto* list = siblings(path);
    const auto at = path.back();
    if ((direction < 0 && at == 0) || (direction > 0 && at + 1 >= list->size()))
        return false;
    auto before = nodes_;
    const auto dest = direction < 0 ? at - 1 : at + 1;
    std::swap((*list)[at], (*list)[dest]);
    auto target = path;
    target.back() = dest;
    const bool ok = publish(std::move(before));
    if (ok)
        for (std::size_t i = 0; i < rows_.size(); ++i)
            if (rows_[i].path == target) {
                select(i);
                break;
            }
    return ok;
}

bool MenuEditor::group(std::string label) {
    auto* node = selected();
    if (!node || label.empty())
        return false;
    auto before = nodes_;
    auto child = std::move(*node);
    *node = {.label = std::move(label), .items = {std::move(child)}};
    return publish(std::move(before));
}

bool MenuEditor::nest() {
    if (!selected())
        return false;
    auto path = rows_[selected_].path;
    auto* list = siblings(path);
    const auto at = path.back();
    if (at == 0 || (*list)[at - 1].label.empty() || !(*list)[at - 1].id.empty() ||
        (*list)[at - 1].items.empty())
        return false;
    auto before = nodes_;
    const auto moving = (*list)[at];
    (*list)[at - 1].items.push_back(std::move((*list)[at]));
    list->erase(list->begin() + static_cast<std::ptrdiff_t>(at));
    const bool accepted = publish(std::move(before));
    if (accepted)
        select_node(moving);
    return accepted;
}

bool MenuEditor::unnest() {
    if (!selected() || rows_[selected_].path.size() < 2)
        return false;
    auto before = nodes_;
    auto path = rows_[selected_].path;
    auto* list = siblings(path);
    const auto moving = (*list)[path.back()];
    auto node = std::move((*list)[path.back()]);
    list->erase(list->begin() + static_cast<std::ptrdiff_t>(path.back()));
    path.pop_back();
    auto* parent = siblings(path);
    parent->insert(parent->begin() + static_cast<std::ptrdiff_t>(path.back() + 1), std::move(node));
    prune(nodes_);
    const bool accepted = publish(std::move(before));
    if (accepted)
        select_node(moving);
    return accepted;
}

bool MenuEditor::rename(std::string label) {
    if (!editable_ || !renameable())
        return false;
    auto* node = selected();
    if (node->id.empty() && label.empty()) {
        error_ = i18n::tr("A submenu needs a label.");
        label_.set_text(std::move(label));
        redraw_();
        return false;
    }
    auto before = nodes_;
    node->label = label;
    const bool accepted = publish(std::move(before));
    if (!accepted)
        label_.set_text(std::move(label));
    return accepted;
}

bool MenuEditor::reset() {
    if (!editable_)
        return false;
    error_ = apply_({{"context_menu", keys[context_], "", true}}, {});
    if (!error_.empty())
        return false;
    switch (context_) {
        case 1:
            config_.context_menu.entry.reset();
            break;
        case 2:
            config_.context_menu.background.reset();
            break;
        case 3:
            config_.context_menu.trash.reset();
            break;
        case 4:
            config_.context_menu.sidebar.reset();
            break;
        default:
            config_.context_menu.items.reset();
            break;
    }
    discard();
    context(context_);
    return true;
}

bool MenuEditor::has_draft() const { return label_.text() != original_label_; }

bool MenuEditor::commit() { return !has_draft() || rename(label_.text()); }

void MenuEditor::discard() {
    label_.set_text(original_label_);
    error_.clear();
}

void MenuEditor::dismiss() {
    popup_.close();
    label_focus_ = false;
}

void MenuEditor::enter_focus(bool reverse) {
    const auto order = focus_order();
    focus_ = reverse ? order.back() : order.front();
    label_focus_ = focus_ == "label";
    reveal_pending_ = true;
    redraw_();
}

void MenuEditor::blur() {
    label_focus_ = false;
    focus_.clear();
    popup_.close();
    redraw_();
}

std::vector<MenuItem> MenuEditor::available() const {
    std::vector<MenuItem> result;
    for (const auto& command : commands) {
        auto item = command_item(command.id, command.label);
        if (command.id == "sort") {
            item.submenu = true;
            for (const auto& column : kDetailsColumns)
                item.children.push_back(
                    command_item("sort:" + std::string{column.id}, column.title));
            item.children.push_back(command_item("sort:ascending", "Ascending"));
            item.children.back().separator = true;
            item.children.push_back(command_item("sort:descending", "Descending"));
        } else if (command.id == "panes") {
            item.submenu = true;
            for (const auto& pane : pane_commands)
                item.children.push_back(command_item(pane.id, pane.label));
        } else if (command.id == "open_with") {
            item.submenu = true;
            auto example = command_item("", "Applications for the selected file");
            example.enabled = false;
            item.children.push_back(std::move(example));
        } else if (command.id == "new_file") {
            item.submenu = true;
            item.children.push_back(command_item("empty_file", "Empty File"));
            auto example = command_item("", "Available templates");
            example.enabled = false;
            example.separator = true;
            item.children.push_back(std::move(example));
        } else if (command.id == "actions") {
            item.submenu = true;
            for (const auto& action : core::action_registry(config_.actions)) {
                MenuItem child;
                child.id = "action:" + action.id;
                child.label =
                    action.translate_label ? std::string{i18n::tr(action.label)} : action.label;
                child.enabled = action.enabled;
                item.children.push_back(std::move(child));
            }
        }
        result.push_back(std::move(item));
    }
    return result;
}

std::vector<MenuItem> MenuEditor::command_catalog() const {
    std::vector<MenuItem> result;
    const auto flatten = [&](const auto& self, const auto& items, std::string prefix) -> void {
        for (const auto& item : items) {
            if (item.id.empty())
                continue;  // Preview examples are not persistent command references.
            auto choice = item;
            choice.label = prefix + choice.label;
            choice.submenu = false;
            choice.children.clear();
            choice.enabled = true;
            choice.separator = false;
            result.push_back(std::move(choice));
            self(self, item.children, prefix + item.label + " › ");
        }
    };
    flatten(flatten, available(), {});
    return result;
}

std::vector<MenuItem> MenuEditor::preview_items() const {
    auto items = available();
    if (context_ != 0) {
        const unsigned mask = 1U << context_;
        std::erase_if(items, [&](const MenuItem& item) {
            const auto command = std::ranges::find(commands, item.id, &CommandLabel::id);
            return command == commands.end() || !(command->contexts & mask);
        });
    }
    return arrange_menu(items, nodes_);
}

void MenuEditor::picker(bool keyboard) {
    auto items = command_catalog();
    for (auto& item : items) {
        const auto id = item.id;
        item.action = [this, id] { add({.id = id}); };
    }
    const auto button = anchor("add");
    popup_.open(button.x, button.bottom(), std::move(items), keyboard);
}

void MenuEditor::activate(std::string_view id) {
    if (id != "label" && !commit())
        return;
    label_focus_ = id == "label" && renameable();
    if (id == "context") {
        std::vector<MenuItem> items;
        for (std::size_t i = 0; i < names.size(); ++i) {
            MenuItem item;
            item.label = i18n::tr(names[i]);
            item.action = [this, i] { context(i); };
            items.push_back(std::move(item));
        }
        const auto button = anchor("context");
        popup_.open(button.x, button.bottom(), std::move(items), true);
    } else if (id == "preview") {
        auto items = preview_items();
        const auto button = anchor("preview");
        popup_.open(button.x, button.bottom(), std::move(items), true);
    } else if (editable_) {
        if (id == "add")
            picker(true);
        else if (id == "separator")
            add({.separator = true});
        else if (id == "remaining")
            add({.remaining = true});
        else if (id == "up")
            move(-1);
        else if (id == "down")
            move(1);
        else if (id == "in")
            nest();
        else if (id == "out")
            unnest();
        else if (id == "group")
            group(std::string{i18n::tr("New group")});
        else if (id == "remove")
            remove();
        else if (id == "reset")
            reset();
    }
    redraw_();
}

std::vector<std::string> MenuEditor::focus_order() const {
    std::vector<std::string> order{"context", "preview"};
    if (editable_)
        order.insert(order.end(), {"add", "separator", "remaining"});
    order.push_back("list");
    if (editable_) {
        if (selected())
            order.insert(order.end(), {"up", "down", "in", "out", "group", "remove"});
        order.push_back("reset");
    }
    if (renameable())
        order.push_back("label");
    return order;
}

ui::Rect MenuEditor::anchor(std::string_view id) const {
    const auto button = std::ranges::find(buttons_, id, &Button::id);
    if (button == buttons_.end())
        return bounds_;
    return button->bounds.intersect(bounds_);
}

void MenuEditor::layout_controls(ui::Renderer& renderer) {
    buttons_.clear();
    const float width = std::max(1.0F, bounds_.width - 32), x = bounds_.x + 12;
    float y = bounds_.y + 4 - scroll_.offset();
    const auto add = [&](std::string id, std::string label, ui::Rect rect, bool enabled = true) {
        buttons_.push_back({std::move(id), std::move(label), rect, enabled});
    };
    if (width >= 280) {
        add("context", names[context_], {x, y, width - 100, 30});
        add("preview", "Preview", {x + width - 92, y, 92, 30});
        y += 38;
    } else {
        add("context", names[context_], {x, y, width, 30});
        add("preview", "Preview", {x, y + 36, width, 30});
        y += 74;
    }
    note_y_ = y;
    y += 60;
    const auto grid = [&](const auto& ids, const auto& labels, std::size_t columns, bool enabled) {
        // Translated labels can need more room than the English column breakpoints allow.
        float minimum = 0;
        for (const auto label : labels)
            minimum = std::max(
                minimum, renderer.fonts().layout(i18n::tr(label), 13, renderer.scale()).width /
                                 renderer.scale() +
                             20);
        while (columns > 1 &&
               (width - static_cast<float>(columns - 1) * 6) / static_cast<float>(columns) <
                   minimum)
            --columns;
        const float cell =
            (width - static_cast<float>(columns - 1) * 6) / static_cast<float>(columns);
        for (std::size_t i = 0; i < ids.size(); ++i)
            add(ids[i], std::string{labels[i]},
                {x + static_cast<float>(i % columns) * (cell + 6),
                 y + static_cast<float>(i / columns) * 36, cell, 30},
                enabled);
        y += static_cast<float>((ids.size() + columns - 1) / columns) * 36;
    };
    grid(std::array{"add", "separator", "remaining"},
         std::array{i18n::mark("Add command"), i18n::mark("Separator"),
                    i18n::mark("Remaining commands")},
         width >= 540   ? 3U
         : width >= 280 ? 2U
                        : 1U,
         editable_);
    y += 6;
    list_ = {x, y, width, std::max(44.0F, static_cast<float>(rows_.size()) * 34)};
    for (std::size_t i = 0; i < rows_.size(); ++i)
        rows_[i].bounds = {x, y + static_cast<float>(i) * 34, width, 32};
    y = list_.bottom() + 10;
    grid(std::array{"up", "down", "in", "out", "group"},
         std::array{i18n::mark("Move up"), i18n::mark("Move down"), i18n::mark("Nest"),
                    i18n::mark("Unnest"), i18n::mark("Group")},
         width >= 560   ? 5U
         : width >= 280 ? 2U
                        : 1U,
         editable_ && selected());
    grid(std::array{"remove", "reset"},
         std::array{i18n::mark("Remove"), i18n::mark("Reset layout")}, width >= 280 ? 2U : 1U,
         editable_);
    if (!selected())
        for (auto& button : buttons_)
            if (button.id == "remove")
                button.enabled = false;
    y += 6;
    label_bounds_ = {x, y + 20, width, 32};
    y += 60;
    error_y_ = y;
    if (!error_.empty())
        y += 28;
    scroll_.set_extent(y + scroll_.offset() - bounds_.y + 8, bounds_.height);
    scrollbar_.layout(bounds_);
}

void MenuEditor::reveal_focus() {
    ui::Rect target;
    if (focus_ == "label")
        target = label_bounds_;
    else if (focus_ == "list")
        target = selected_ < rows_.size() ? rows_[selected_].bounds : list_;
    else {
        const auto button = std::ranges::find(buttons_, focus_, &Button::id);
        if (button != buttons_.end())
            target = button->bounds;
    }
    if (target.empty())
        return;
    if (target.y < bounds_.y)
        scroll_.jump_to(scroll_.offset() + target.y - bounds_.y);
    else if (target.bottom() > bounds_.bottom())
        scroll_.jump_to(scroll_.offset() + target.bottom() - bounds_.bottom());
}

bool MenuEditor::key(const ui::KeyEvent& event) {
    if (popup_.key(event))
        return true;
    if (event.keysym == XKB_KEY_Escape) {
        if (has_draft()) {
            discard();
            redraw_();
            return true;
        }
        return false;
    }
    if (event.keysym == XKB_KEY_Tab || event.keysym == XKB_KEY_ISO_Left_Tab) {
        commit();  // Keep an invalid draft visible without trapping focus inside its field.
        const auto order = focus_order();
        const auto at = std::ranges::find(order, focus_);
        const auto n = at == order.end() ? 0 : static_cast<std::size_t>(at - order.begin());
        const bool reverse = event.modifiers.shift || event.keysym == XKB_KEY_ISO_Left_Tab;
        if ((reverse && n == 0) || (!reverse && n + 1 == order.size())) {
            blur();
            return false;
        }
        focus_ = order[reverse ? n - 1 : n + 1];
        label_focus_ = focus_ == "label";
        reveal_pending_ = true;
        redraw_();
        return true;
    }
    if (label_focus_ && renameable()) {
        const bool selecting =
            event.keysym == XKB_KEY_Left || event.keysym == XKB_KEY_Right ||
            event.keysym == XKB_KEY_Home || event.keysym == XKB_KEY_End ||
            (event.modifiers.ctrl && (event.keysym == XKB_KEY_a || event.keysym == XKB_KEY_A));
        if ((editable_ || selecting) && label_.key(event)) {
            redraw_();
            return true;
        }
    }
    if (event.keysym == XKB_KEY_Return || event.keysym == XKB_KEY_KP_Enter ||
        (event.keysym == XKB_KEY_space && !label_focus_)) {
        if (label_focus_)
            commit();
        else
            activate(focus_);
        return true;
    }
    if (focus_ == "list") {
        if (event.keysym == XKB_KEY_Up || event.keysym == XKB_KEY_Down) {
            if (event.modifiers.alt && editable_)
                move(event.keysym == XKB_KEY_Up ? -1 : 1);
            else if (commit())
                select(event.keysym == XKB_KEY_Up ? (selected_ > 0 ? selected_ - 1 : 0)
                                                  : selected_ + 1);
        } else if (editable_ && event.keysym == XKB_KEY_Delete)
            remove();
        else if (editable_ && event.modifiers.alt && event.keysym == XKB_KEY_Right)
            nest();
        else if (editable_ && event.modifiers.alt && event.keysym == XKB_KEY_Left)
            unnest();
    }
    return true;
}

void MenuEditor::pointer(const ui::PointerEvent& event) {
    if (popup_.pointer(event))
        return;
    if (label_focus_ && renameable() && event.kind != ui::PointerEvent::Kind::press &&
        label_.pointer(event)) {
        redraw_();
        return;
    }
    if (scrollbar_.pointer(event, scroll_)) {
        redraw_();
        return;
    }
    if (!bounds_.contains(event.x, event.y))
        return;
    if (event.kind == ui::PointerEvent::Kind::scroll) {
        scroll_.jump_to(scroll_.offset() +
                        (event.wheel_y != 0 ? event.wheel_y * 48 : event.scroll_y));
        redraw_();
        return;
    }
    if (event.kind != ui::PointerEvent::Kind::press || event.button != ui::PointerButton::left)
        return;
    for (const auto& button : buttons_)
        if (button.enabled && button.bounds.contains(event.x, event.y)) {
            const auto id = button.id;
            focus_ = id;
            activate(id);
            return;
        }
    if (renameable() && label_bounds_.contains(event.x, event.y)) {
        focus_ = "label";
        label_focus_ = true;
        label_.pointer(event);
        redraw_();
        return;
    }
    if (!commit())
        return;
    if (list_.contains(event.x, event.y))
        for (std::size_t i = 0; i < rows_.size(); ++i)
            if (rows_[i].bounds.contains(event.x, event.y)) {
                focus_ = "list";
                select(i);
                return;
            }
}

void MenuEditor::paste(std::string_view text) {
    if (editable_ && label_focus_ && renameable()) {
        label_.insert(text.substr(0, text.find_first_of("\r\n")));
        redraw_();
    }
}

std::string MenuEditor::copy_selection(bool cut) {
    if (!label_focus_ || !renameable())
        return {};
    const auto [a, b] = label_.selection();
    auto value = label_.text().substr(a, b - a);
    if (cut && editable_) {
        label_.insert("");
        redraw_();
    }
    return value;
}

void MenuEditor::draw(ui::Renderer& r, const ui::Rect& bounds, const theme::Palette& p) {
    if (bounds.width != bounds_.width || bounds.height != bounds_.height || bounds.x != bounds_.x ||
        bounds.y != bounds_.y) {
        popup_.close();
        reveal_pending_ = true;
    }
    bounds_ = bounds;
    layout_controls(r);
    if (reveal_pending_) {
        reveal_focus();
        layout_controls(r);
        reveal_pending_ = false;
    }
    r.push_clip(bounds);
    for (const auto& button : buttons_) {
        draw_dialog_button(r, button.bounds, p, i18n::tr(button.label), focus_ == button.id);
        if (!button.enabled)
            r.fill_rect(button.bounds, p.surface_container_high.with_alpha(0.55F));
    }
    r.draw_text(i18n::tr(inherited_ ? "Inherited layout; editing creates an override."
                                    : "Custom layout; reset restores inheritance."),
                bounds.x + 12, note_y_, 11, p.on_surface_variant, list_.width);
    r.draw_text(i18n::tr("Preview shows menu structure only."), bounds.x + 12, note_y_ + 16, 11,
                p.on_surface_variant, list_.width);
    r.draw_text(i18n::tr("Available commands depend on the selection."), bounds.x + 12,
                note_y_ + 32, 11, p.on_surface_variant, list_.width);
    const auto catalog = command_catalog();
    for (const auto& row : rows_) {
        if (row.bounds.intersect(bounds).empty())
            continue;
        if (&row == &rows_[selected_])
            r.draw_box(row.bounds, {.fill = p.surface_container_highest,
                                    .border_width = focus_ == "list" ? 1.0F : 0.0F,
                                    .border = p.on_surface});
        const auto& node = (*siblings(row.path))[row.path.back()];
        const float indent = static_cast<float>(row.path.size() - 1) * 16;
        r.draw_text(title(node, catalog), row.bounds.x + 8 + indent, row.bounds.y + 9, 13,
                    p.on_surface, std::max(1.0F, row.bounds.width - 16 - indent));
    }
    if (rows_.empty())
        r.draw_text(i18n::tr("This menu is hidden. Add a command to show it."), list_.x + 8,
                    list_.y + 8, 12, p.on_surface_variant, list_.width - 16);
    r.draw_text(i18n::tr("Label"), label_bounds_.x, label_bounds_.y - 18, 12, p.on_surface_variant,
                label_bounds_.width);
    if (renameable()) {
        label_.draw(r, label_bounds_, 13,
                    {p.surface_container_highest, label_focus_ ? p.primary : p.outline,
                     p.on_surface, theme::readable_foreground(p.secondary_container, p.on_surface),
                     label_focus_ && editable_ ? p.on_surface : p.on_surface.with_alpha(0)});
    } else {
        r.draw_text(i18n::tr("Select a command or submenu to edit its label."), label_bounds_.x,
                    label_bounds_.y + 8, 12, p.on_surface_variant, label_bounds_.width);
    }
    if (!error_.empty())
        r.draw_text(error_, list_.x, error_y_, 11, p.error, list_.width);
    r.fill_rect(scrollbar_.thumb(scroll_), p.outline);
    r.pop_clip();
    popup_.draw(r, bounds, p);
}
}  // namespace rummage::views
