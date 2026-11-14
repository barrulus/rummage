#include "rummage/views/settings_dialog.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <sstream>
#include <toml++/toml.hpp>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "rummage/core/config_edit.hpp"
#include "rummage/core/file.hpp"
#include "rummage/views/shortcuts.hpp"

namespace rummage::views {
namespace {
std::string arguments(const std::vector<std::string>& argv) {
    std::string result = "[";
    for (const auto& arg : argv) {
        if (result.size() > 1)
            result += ", ";
        result += core::quote_config_string(arg);
    }
    return result + "]";
}
}  // namespace

SettingsDialog::SettingsDialog(core::WorkerPool& pool, std::function<void()> redraw)
    : pool_{pool}, redraw_{std::move(redraw)} {}

SettingsDialog::~SettingsDialog() { job_.cancel(); }

void SettingsDialog::open(std::filesystem::path path, std::function<void()> open_file,
                          std::function<void(const core::ConfigResult&)> applied) {
    if (busy_)
        return;
    job_.cancel();
    ++generation_;
    path_ = std::move(path);
    open_file_ = std::move(open_file);
    applied_ = std::move(applied);
    open_ = busy_ = true;
    valid_ = false;
    error_.clear();
    fields_.clear();
    buttons_.clear();
    new_actions_ = 0;
    change_section(0);
    job_ = pool_.submit(
        core::Priority::visible,
        [path = path_](const core::CancelToken&) { return core::read_text_file(path); },
        [this](core::TextFile file) {
            busy_ = false;
            existed_ = file.exists;
            original_ = std::move(file.text);
            error_ = std::move(file.error);
            if (error_.empty()) {
                const auto parsed = core::parse_config(original_, path_.string());
                if (!parsed.problems.empty())
                    error_ = parsed.problems.front();
                else {
                    valid_ = true;
                    populate(parsed);
                }
            }
            redraw_();
        });
    redraw_();
}

void SettingsDialog::close() {
    if (busy_)
        return;
    open_ = false;
    ++generation_;
    redraw_();
}

void SettingsDialog::populate(const core::ConfigResult& loaded) {
    const auto& config = loaded.config;
    const auto add = [&](int section, std::string table, std::string key, std::string label,
                         std::string value, bool raw = false,
                         std::vector<std::string> choices = {}) {
        fields_.push_back({section,
                           std::move(table),
                           std::move(key),
                           std::move(label),
                           value,
                           raw,
                           ui::TextField{value},
                           std::move(choices),
                           {}});
    };
    add(0, "appearance", "color_scheme", "Colour theme", config.appearance.color_scheme, false,
        {"system", "light", "dark"});
    add(0, "icon", "theme", "Icon theme (system, builtin, or an installed theme)",
        config.icon.theme);
    add(0, "appearance", "background_opacity", "Background opacity (0 to 1)",
        std::format("{:g}", config.appearance.background_opacity), true);
    add(0, "appearance", "preview_position", "Preview placement: right side / below Places",
        config.appearance.preview_position, false, {"right", "sidebar"});
    add(0, "icon", "follow_accent", "Follow desktop accent",
        config.icon.follow_accent ? "true" : "false", true, {"true", "false"});
    add(0, "font", "family", "Interface font (restart required)", config.font.family);
    add(0, "font", "monospace", "Preview font (restart required)", config.font.monospace);
    add(0, "status_bar", "text_size", "Status text size (logical pixels)",
        std::format("{:g}", config.status_bar.text_size), true);
    auto bindings = config.keybindings;
    for (const auto& [name, label] : binding_settings()) {
        const auto found = bindings.find(name);
        add(1, "keybindings", name, label, found == bindings.end() ? "Default" : found->second);
        bindings.erase(name);
    }
    for (const auto& [name, chord] : bindings)
        add(1, "keybindings", name, name, chord);
    action_count_ = config.actions.size();
    for (std::size_t i = 0; i < config.actions.size(); ++i) {
        const auto& action = config.actions[i];
        const auto table = std::format("actions[{}]", i);
        add(2, table, "label", action.id + " — label", action.label);
        if (action.kind == core::Action::Kind::external)
            add(2, table, "argv", action.id + " — program and arguments", arguments(action.argv),
                true);
        add(2, table, "enabled", action.id + " — enabled", action.enabled ? "true" : "false", true,
            {"true", "false"});
    }
}

bool SettingsDialog::edit(std::string_view table, std::string_view key, std::string value) {
    if (busy_ || !valid_)
        return false;
    for (auto& field : fields_) {
        if (field.table == table && field.key == key) {
            field.input.set_text(std::move(value));
            redraw_();
            return true;
        }
    }
    return false;
}

void SettingsDialog::add_action() {
    if (busy_ || !valid_)
        return;
    const auto table = std::format("new:{}", new_actions_++);
    const auto add = [&](std::string key, std::string label, std::string value, bool raw = false) {
        fields_.push_back({2,
                           table,
                           std::move(key),
                           std::move(label),
                           {},
                           raw,
                           ui::TextField{std::move(value)},
                           {},
                           {}});
    };
    add("id", "New action — unique ID", std::format("custom_{}", action_count_ + new_actions_));
    add("label", "New action — menu label", "New action");
    add("argv", "Program and arguments, e.g. [\"xdg-open\", \"{file}\"]", "[]", true);
    section_ = 2;
    focus_ = "f" + std::to_string(fields_.size() - 3);
    reveal_focus();
    redraw_();
}

void SettingsDialog::save() {
    if (busy_ || !valid_ || path_.empty())
        return;
    std::vector<core::ConfigEdit> edits;
    std::string appended;
    for (const auto& field : fields_) {
        if (field.table.starts_with("new:"))
            continue;
        if (field.input.text() != field.original)
            edits.push_back(
                {field.table, field.key,
                 field.raw ? field.input.text() : core::quote_config_string(field.input.text()),
                 field.table == "keybindings" && field.input.text() == "Default"});
    }
    for (std::size_t i = 0; i < new_actions_; ++i) {
        appended += "\n[[actions]]\n";
        for (const auto& field : fields_) {
            if (field.table == std::format("new:{}", i))
                appended += field.key + " = " +
                            (field.raw ? field.input.text()
                                       : core::quote_config_string(field.input.text())) +
                            "\n";
        }
    }
    const auto changed = core::edit_config_text(original_, edits, appended);
    error_ = changed.error;
    core::ConfigResult parsed;
    if (error_.empty()) {
        parsed = core::parse_config(changed.text, path_.string());
        if (!parsed.problems.empty())
            error_ = parsed.problems.front();
        else {
            const auto problems =
                configure_shortcuts(parsed.config.keybindings, parsed.config.actions, false);
            if (!problems.empty())
                error_ = problems.front();
        }
    }
    if (!error_.empty()) {
        redraw_();
        return;
    }
    busy_ = true;
    job_ = pool_.submit(
        core::Priority::visible,
        [path = path_, expected = original_, existed = existed_,
         replacement = changed.text](const core::CancelToken& token) -> std::string {
            const auto current = core::read_text_file(path);
            if (!current.error.empty())
                return current.error;
            if (current.exists != existed || current.text != expected)
                return "Config changed outside Settings. Close and reopen Settings before saving.";
            if (token.cancelled())
                return "Save cancelled";
            std::error_code path_error;
            const auto destination = std::filesystem::is_symlink(path, path_error)
                                         ? std::filesystem::canonical(path, path_error)
                                         : path;
            if (path_error && path_error != std::errc::no_such_file_or_directory)
                return path_error.message();
            if (const auto error = core::write_file_atomically(destination, replacement))
                return error.message();
            return {};
        },
        [this, parsed, replacement = changed.text](std::string error) {
            busy_ = false;
            error_ = std::move(error);
            if (error_.empty()) {
                original_ = replacement;
                existed_ = true;
                if (applied_)
                    applied_(parsed);
                close();
            }
            redraw_();
        });
    redraw_();
}

void SettingsDialog::change_section(int section) {
    section_ = section;
    scroll_ = 0;
    focus_ = "section" + std::to_string(section);
    redraw_();
}

void SettingsDialog::reveal_focus() {
    std::size_t row = 0;
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        if (fields_[i].section != section_)
            continue;
        if (focus_ == "f" + std::to_string(i)) {
            const float y = static_cast<float>(row) * 72;
            if (y < scroll_)
                scroll_ = y;
            if (y + 72 > scroll_ + list_.height)
                scroll_ = std::max(0.0F, y + 72 - list_.height);
            return;
        }
        ++row;
    }
}

void SettingsDialog::activate(std::string_view id) {
    if (busy_)
        return;
    if (id == "save")
        save();
    else if (id == "close")
        close();
    else if (id == "file" && open_file_)
        open_file_();
    else if (id == "add")
        add_action();
    else if (id.starts_with("section"))
        change_section(id.back() - '0');
    else {
        for (std::size_t i = 0; i < fields_.size(); ++i) {
            auto& field = fields_[i];
            if (id != "f" + std::to_string(i) || field.choices.empty())
                continue;
            const auto found = std::ranges::find(field.choices, field.input.text());
            const auto index = found == field.choices.end()
                                   ? 0
                                   : (static_cast<std::size_t>(found - field.choices.begin()) + 1) %
                                         field.choices.size();
            field.input.set_text(field.choices[index]);
            redraw_();
        }
    }
}

bool SettingsDialog::key(const ui::KeyEvent& event) {
    if (!open_)
        return false;
    if (busy_)
        return true;
    if (event.keysym == XKB_KEY_Escape) {
        close();
        return true;
    }
    if (event.modifiers.alt && event.keysym >= XKB_KEY_1 && event.keysym <= XKB_KEY_3) {
        change_section(static_cast<int>(event.keysym - XKB_KEY_1));
        return true;
    }
    if (event.modifiers.ctrl && (event.keysym == XKB_KEY_s || event.keysym == XKB_KEY_S)) {
        save();
        return true;
    }
    if (event.keysym == XKB_KEY_Tab || event.keysym == XKB_KEY_ISO_Left_Tab) {
        std::vector<std::string> order{"section0", "section1", "section2"};
        for (std::size_t i = 0; i < fields_.size(); ++i)
            if (fields_[i].section == section_)
                order.push_back("f" + std::to_string(i));
        if (section_ == 2)
            order.push_back("add");
        order.insert(order.end(), {"file", "close", "save"});
        auto found = std::ranges::find(order, focus_);
        const auto at = found == order.end() ? 0 : static_cast<std::size_t>(found - order.begin());
        focus_ = order[(at + (event.modifiers.shift || event.keysym == XKB_KEY_ISO_Left_Tab
                                  ? order.size() - 1
                                  : 1)) %
                       order.size()];
        reveal_focus();
        redraw_();
        return true;
    }
    if (event.keysym == XKB_KEY_Return || event.keysym == XKB_KEY_KP_Enter ||
        (event.keysym == XKB_KEY_space && (!focus_.starts_with("f") || focus_ == "file"))) {
        activate(focus_);
        return true;
    }
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        auto& field = fields_[i];
        if (focus_ != "f" + std::to_string(i))
            continue;
        if (!field.choices.empty()) {
            if (event.keysym == XKB_KEY_space || event.keysym == XKB_KEY_Left ||
                event.keysym == XKB_KEY_Right)
                activate(focus_);
        } else if (field.input.key(event))
            redraw_();
        return true;
    }
    return true;
}

std::string SettingsDialog::copy_selection(bool cut) {
    if (busy_ || !open_)
        return {};
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        auto& field = fields_[i];
        if (focus_ != "f" + std::to_string(i) || !field.choices.empty())
            continue;
        const auto [first, last] = field.input.selection();
        const auto text = field.input.text().substr(first, last - first);
        if (cut && !text.empty()) {
            field.input.insert("");
            redraw_();
        }
        return text;
    }
    return {};
}

void SettingsDialog::paste(std::string_view text) {
    if (busy_ || !open_)
        return;
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        auto& field = fields_[i];
        if (focus_ == "f" + std::to_string(i) && field.choices.empty()) {
            field.input.insert(text.substr(0, text.find_first_of("\r\n")));
            redraw_();
        }
    }
}

ui::CursorShape SettingsDialog::cursor_at(float x, float y) const {
    if (busy_)
        return ui::CursorShape::standard;
    for (const auto& button : buttons_)
        if (button.bounds.contains(x, y))
            return ui::CursorShape::pointer;
    if (list_.contains(x, y))
        for (const auto& field : fields_)
            if (field.section == section_ && field.bounds.contains(x, y))
                return field.choices.empty() ? ui::CursorShape::text : ui::CursorShape::pointer;
    return ui::CursorShape::standard;
}

void SettingsDialog::pointer(const ui::PointerEvent& event) {
    if (!open_ || busy_)
        return;
    if (event.kind == ui::PointerEvent::Kind::scroll && list_.contains(event.x, event.y)) {
        const float distance = event.wheel_y != 0 ? event.wheel_y * 48.0F : event.scroll_y;
        scroll_ = std::clamp(scroll_ + distance, 0.0F, max_scroll_);
        redraw_();
        return;
    }
    if (event.kind != ui::PointerEvent::Kind::press || event.button != ui::PointerButton::left)
        return;
    for (const auto& button : buttons_) {
        if (button.bounds.contains(event.x, event.y)) {
            const auto id = button.id;
            focus_ = id;
            activate(id);
            return;
        }
    }
    if (!list_.contains(event.x, event.y))
        return;
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        auto& field = fields_[i];
        if (field.section == section_ && field.bounds.contains(event.x, event.y)) {
            focus_ = "f" + std::to_string(i);
            if (field.choices.empty())
                field.input.click(event.x, event.modifiers.shift);
            else
                activate(focus_);
            redraw_();
            return;
        }
    }
}

void SettingsDialog::draw(ui::Renderer& renderer, const ui::Rect& bounds,
                          const theme::Palette& palette) {
    if (!open_)
        return;
    renderer.fill_rect(bounds, palette.scrim.with_alpha(0.55F));
    const float width = std::min(660.0F, std::max(0.0F, bounds.width - 16));
    const float height = std::min(650.0F, std::max(0.0F, bounds.height - 16));
    const ui::Rect card{bounds.x + (bounds.width - width) / 2,
                        bounds.y + (bounds.height - height) / 2, width, height};
    renderer.draw_box(card, {.fill = palette.surface_container_high, .radius = {}});
    renderer.push_clip(card);
    buttons_.clear();
    for (auto& field : fields_)
        field.bounds = {};
    const float left = card.x + 16, room = std::max(1.0F, width - 32);
    renderer.draw_text(busy_ ? "Settings · Please wait…" : "Settings", left, card.y + 14, 18,
                       palette.on_surface, room);
    const auto button = [&](std::string id, std::string label, ui::Rect rect) {
        buttons_.push_back({id, label, rect});
        const bool selected = focus_ == id || id == "section" + std::to_string(section_);
        renderer.draw_box(rect,
                          {.fill = selected ? palette.primary : palette.surface_container_highest,
                           .radius = {},
                           .border_width = focus_ == id ? 1.0F : 0.0F,
                           .border = palette.primary});
        renderer.draw_text(label, rect.x + 8, rect.y + 9, 13,
                           selected ? palette.on_primary : palette.on_surface,
                           std::max(1.0F, rect.width - 16));
    };
    const float tab_width = (room - 8) / 3;
    for (int i = 0; i < 3; ++i)
        button("section" + std::to_string(i),
               std::array{"Appearance", "Keybindings", "Actions"}[static_cast<std::size_t>(i)],
               {left + static_cast<float>(i) * (tab_width + 4), card.y + 44, tab_width, 34});
    const char* hint =
        section_ == 0   ? "Choose a theme and preview layout. Save applies live settings."
        : section_ == 1 ? "Enter a chord (Ctrl+J), Default to reset, or empty to disable."
                        : "Edit custom actions here. Open config for filters and advanced options.";
    renderer.draw_text(hint, left, card.y + 88, 12, palette.on_surface_variant, room);
    list_ = {left, card.y + 112, room, std::max(0.0F, height - (section_ == 2 ? 236 : 188))};
    const auto count = std::ranges::count_if(
        fields_, [this](const Field& field) { return field.section == section_; });
    max_scroll_ = std::max(0.0F, static_cast<float>(count) * 72 - list_.height);
    scroll_ = std::clamp(scroll_, 0.0F, max_scroll_);
    renderer.push_clip(list_);
    if (section_ == 2 && count == 0 && !busy_ && valid_)
        renderer.draw_text("No custom actions yet. Choose Add action to create one.", left,
                           list_.y + 8, 13, palette.on_surface_variant, room);
    float y = list_.y - scroll_;
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        auto& field = fields_[i];
        if (field.section != section_)
            continue;
        if (y + 72 > list_.y && y < list_.bottom()) {
            renderer.draw_text(field.label, left, y, 13, palette.on_surface_variant, room - 10);
            field.bounds = {left, y + 22, room - 10, 32};
            const bool focused = focus_ == "f" + std::to_string(i);
            if (field.choices.empty())
                field.input.draw(
                    renderer, field.bounds, 13,
                    {.background = palette.surface_container_highest,
                     .border = focused ? palette.primary : palette.outline_variant,
                     .text = palette.on_surface,
                     .selection = palette.secondary_container,
                     .caret = focused ? palette.on_surface : palette.on_surface.with_alpha(0)});
            else {
                renderer.draw_box(field.bounds,
                                  {.fill = palette.surface_container_highest,
                                   .radius = {},
                                   .border_width = 1,
                                   .border = focused ? palette.primary : palette.outline_variant});
                std::string label = field.input.text();
                if (field.key == "preview_position")
                    label = label == "right" ? "Right side" : "Below Places";
                else if (field.key == "color_scheme" && label == "system")
                    label = "Desktop / Noctalia";
                else if (label == "true")
                    label = "On";
                else if (label == "false")
                    label = "Off";
                renderer.draw_text(label + "  ↔", left + 6, y + 30, 13, palette.on_surface,
                                   room - 28);
            }
        }
        y += 72;
    }
    if (max_scroll_ > 0 && list_.height > 0) {
        const float thumb =
            std::max(18.0F, list_.height * list_.height / (max_scroll_ + list_.height));
        renderer.fill_rect(
            {list_.right() - 4, list_.y + (list_.height - thumb) * scroll_ / max_scroll_, 3, thumb},
            palette.outline);
    }
    renderer.pop_clip();
    if (section_ == 2)
        button("add", "Add action", {left, card.bottom() - 114, std::min(116.0F, room), 32});
    if (!error_.empty())
        renderer.draw_text(error_, left, card.bottom() - 72, 12, palette.error, room);
    const float footer = (room - 12) / 3;
    button("file", "Open config", {left, card.bottom() - 48, footer, 32});
    button("close", "Cancel", {left + footer + 6, card.bottom() - 48, footer, 32});
    button("save", "Save", {left + (footer + 6) * 2, card.bottom() - 48, footer, 32});
    renderer.pop_clip();
}
}  // namespace rummage::views
