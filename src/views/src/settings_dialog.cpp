#include "rummage/views/settings_dialog.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <unordered_set>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "rummage/core/config_edit.hpp"
#include "rummage/core/file.hpp"
#include "rummage/core/i18n.hpp"
#include "rummage/views/dialog_chrome.hpp"
#include "rummage/views/shortcuts.hpp"

namespace rummage::views {
namespace {
constexpr std::array kSections{"General", "Appearance", "Keyboard shortcuts", "Actions"};

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
    : pool_{pool}, redraw_{std::move(redraw)}, choices_{redraw_}, details_{redraw_, {}} {}

SettingsDialog::~SettingsDialog() { job_.cancel(); }

void SettingsDialog::open(std::filesystem::path path, std::function<void()> open_file,
                          std::function<void(const core::ConfigResult&)> applied) {
    if (busy_ || (open_ && (has_drafts() || staged_ != original_)))
        return;
    job_.cancel();
    ++generation_;
    path_ = std::move(path);
    open_file_ = std::move(open_file);
    applied_ = std::move(applied);
    open_ = busy_ = true;
    valid_ = saved_ = close_requested_ = discard_prompt_ = false;
    error_.clear();
    write_error_.clear();
    original_.clear();
    staged_.clear();
    fields_.clear();
    buttons_.clear();
    new_actions_ = new_bindings_ = 0;
    change_section(0);
    job_ = pool_.submit(
        core::Priority::visible,
        [path = path_](const core::CancelToken&) { return core::read_text_file(path); },
        [this](core::TextFile file) {
            busy_ = false;
            existed_ = file.exists;
            staged_ = original_ = std::move(file.text);
            write_error_ = std::move(file.error);
            if (write_error_.empty()) {
                const auto parsed = core::parse_config(original_, path_.string());
                if (!parsed.problems.empty())
                    write_error_ = parsed.problems.front();
                else {
                    valid_ = true;
                    populate(parsed);
                }
            }
            update_error();
            maybe_close();
            redraw_();
        });
    redraw_();
}

void SettingsDialog::finish_close() {
    choices_.close();
    open_ = false;
    close_requested_ = discard_prompt_ = false;
    ++generation_;
    redraw_();
}

bool SettingsDialog::has_drafts() const {
    return std::ranges::any_of(fields_, [](const Field& field) {
        return field.section >= 0 &&
               (field.table.starts_with("new:") || field.table.starts_with("binding:") ||
                field.input.text() != field.original);
    });
}

void SettingsDialog::maybe_close() {
    if (!close_requested_ || busy_)
        return;
    if (!valid_ || (!has_drafts() && staged_ == original_))
        finish_close();
    else {
        close_requested_ = false;
        discard_prompt_ = true;
    }
}

void SettingsDialog::close() {
    choices_.close();
    close_requested_ = true;
    if (valid_)
        save();
    maybe_close();
}

void SettingsDialog::populate(const core::ConfigResult& loaded) {
    const auto& config = loaded.config;
    const auto add = [&](int section, std::string group, std::string table, std::string key,
                         std::string label, std::string description, std::string value,
                         bool raw = false, std::vector<std::string> choices = {},
                         std::optional<Number> number = std::nullopt) {
        Field field;
        field.section = section;
        field.group = std::move(group);
        field.table = std::move(table);
        field.key = std::move(key);
        field.label = std::move(label);
        field.description = std::move(description);
        field.original = value;
        field.input.set_text(std::move(value));
        field.raw = raw;
        field.choices = std::move(choices);
        field.number = number;
        fields_.push_back(std::move(field));
    };
    const auto mode = config.startup.mode;
    add(0, "Startup", "startup", "mode", "When Rummage starts", "Applies to the next launch",
        mode == core::Config::Startup::Mode::home     ? "home"
        : mode == core::Config::Startup::Mode::folder ? "folder"
        : mode == core::Config::Startup::Mode::last   ? "last"
                                                      : "session",
        false, {"session", "home", "folder", "last"});
    add(0, "Startup", "startup", "folder", "Startup folder",
        "Used when starting in a specific folder", config.startup.folder);
    add(0, "Opening files", "open", "programs", "Open executable programs",
        "Allow opening applications from the browser", config.open.programs ? "true" : "false",
        true, {"true", "false"});
    add(0, "Opening files", "pointer", "double_click_ms", "Double-click interval",
        "Milliseconds · 100–2000", std::to_string(config.pointer.double_click_ms), true, {},
        Number{100, 2000, 50});
    add(0, "Opening files", "pointer", "double_click_distance", "Double-click distance",
        "Logical pixels · 0–50", std::to_string(config.pointer.double_click_distance), true, {},
        Number{0, 50, 1});
    add(0, "Terminal", "terminal", "program", "Terminal program",
        "Leave empty for automatic discovery", config.terminal.program);
    add(0, "Terminal", "terminal", "args", "Terminal arguments",
        "Literal argument array, e.g. [\"--title\", \"Files\"]", arguments(config.terminal.args),
        true);

    add(1, "Theme", "appearance", "palette_source", "Palette source",
        "Automatic, Desktop, Palette file or Built-in", config.appearance.palette_source, false,
        {"auto", "desktop", "file", "builtin"});
    add(1, "Theme", "appearance", "color_scheme", "Colour mode",
        "Follow the selected source or choose light or dark", config.appearance.color_scheme, false,
        {"system", "light", "dark"});
    add(1, "Theme", "icon", "theme", "Icon theme", "system, builtin, or an installed theme name",
        config.icon.theme);
    add(1, "Theme", "icon", "follow_accent", "Follow palette accent",
        "Use the palette accent for Rummage’s app icon",
        config.icon.follow_accent ? "true" : "false", true, {"true", "false"});
    add(1, "Theme", "appearance", "background_opacity", "Background opacity",
        "0 is transparent · 1 is opaque", std::format("{:g}", config.appearance.background_opacity),
        true, {}, Number{0, 1, 0.05F});
    add(1, "Layout", "appearance", "preview_position", "Preview placement",
        "Position of the docked preview", config.appearance.preview_position, false,
        {"right", "sidebar"});
    add(1, "Text", "font", "size", "Interface text size",
        "Logical pixels · 8–32 · applies immediately", std::format("{:g}", config.font.size), true,
        {}, Number{8, 32, 0.5F});
    add(1, "Text", "status_bar", "text_size", "Status text size", "Logical pixels · 8–32",
        std::format("{:g}", config.status_bar.text_size), true, {}, Number{8, 32, 0.5F});
    add(1, "Text", "font", "family", "Interface font", "Restart required", config.font.family);
    add(1, "Text", "font", "monospace", "Preview font", "Restart required", config.font.monospace);
    add(1, "Motion", "animations", "enabled", "Motion effects", "Animate changes in the browser",
        config.animations.enabled ? "true" : "false", true, {"true", "false"});
    add(1, "Motion", "animations", "duration_scale", "Motion duration",
        "Multiplier · 0–4 · 0 is immediate", std::format("{:g}", config.animations.duration_scale),
        true, {}, Number{0, 4, 0.1F});
    const auto reduce = config.animations.reduce_motion;
    add(1, "Motion", "animations", "reduce_motion", "Reduce motion",
        "Follow the desktop accessibility preference",
        reduce == core::MotionConfig::Reduce::always  ? "always"
        : reduce == core::MotionConfig::Reduce::never ? "never"
                                                      : "system",
        false, {"system", "always", "never"});
    const auto quality = config.animations.quality;
    add(1, "Motion", "animations", "quality", "Motion quality",
        "Choose the detail of animation effects",
        quality == core::MotionConfig::Quality::low    ? "low"
        : quality == core::MotionConfig::Quality::high ? "high"
                                                       : "auto",
        false, {"auto", "low", "high"});
    auto bindings = config.keybindings;
    for (const auto& [name, label] : binding_settings()) {
        const auto found = bindings.find(name);
        add(2, "Commands", "keybindings", name, label,
            "Chord, Default to reset, or empty to disable",
            found == bindings.end() ? "Default" : found->second);
        bindings.erase(name);
    }
    for (const auto& [name, chord] : bindings)
        add(2, "Custom shortcuts", "keybindings", name, name,
            "Chord, Default to reset, or empty to disable", chord);
    action_count_ = config.actions.size();
    for (std::size_t i = 0; i < config.actions.size(); ++i) {
        const auto& action = config.actions[i];
        const auto table = std::format("actions[{}]", i);
        add(3, action.id, table, "label", "Menu label",
            "Name shown in menus and the command palette", action.label);
        if (action.kind == core::Action::Kind::external)
            add(3, action.id, table, "argv", "Program and arguments",
                "Literal argument array; no shell expansion", arguments(action.argv), true);
        add(3, action.id, table, "enabled", "Enabled", "Make this action available when applicable",
            action.enabled ? "true" : "false", true, {"true", "false"});
    }
}

bool SettingsDialog::edit(std::string_view table, std::string_view key, std::string value) {
    if (!valid_)
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
    if (!valid_)
        return;
    commit_focus();
    const auto table = std::format("new:{}", new_actions_++);
    const auto add = [&](std::string key, std::string label, std::string description,
                         std::string value, bool raw = false) {
        Field field;
        field.section = 3;
        field.table = table;
        field.group = "New action";
        field.key = std::move(key);
        field.label = std::move(label);
        field.description = std::move(description);
        field.input.set_text(std::move(value));
        field.raw = raw;
        fields_.push_back(std::move(field));
    };
    add("id", "Unique ID", "Used by shortcuts and custom menu layouts",
        std::format("custom_{}", action_count_ + new_actions_));
    add("label", "Menu label", "Draft: saved when all required fields are valid",
        std::string{i18n::tr("New action")});
    add("argv", "Program and arguments", "Example: [\"xdg-open\", \"{file}\"]", "[]", true);
    change_section(3);
    focus_ = "f" + std::to_string(fields_.size() - 3);
    reveal_focus();
    redraw_();
}

void SettingsDialog::add_keybinding() {
    if (!valid_)
        return;
    commit_focus();
    const auto table = std::format("binding:{}", new_bindings_++);
    for (const auto& [key, label, description] :
         {std::array{"target", "Target", "action:ID or go:/path (also go:~/path)"},
          std::array{"chord", "Shortcut", "Draft: enter a chord, e.g. Ctrl+Alt+J"}}) {
        Field field;
        field.section = 2;
        field.table = table;
        field.group = "New shortcut";
        field.key = key;
        field.label = label;
        field.description = description;
        fields_.push_back(std::move(field));
    }
    change_section(2);
    focus_ = "f" + std::to_string(fields_.size() - 2);
    reveal_focus();
    redraw_();
}

void SettingsDialog::update_error() {
    error_ = write_error_;
    if (error_.empty())
        for (const auto& field : fields_)
            if (!field.error.empty()) {
                error_ = field.error;
                break;
            }
}

// Validate one logical entry against all previously accepted changes. Incomplete action and
// binding drafts never stop independent preferences from being saved.
void SettingsDialog::commit(std::size_t index) {
    if (!valid_ || index >= fields_.size() || fields_[index].section < 0)
        return;
    const auto table = fields_[index].table;
    const bool action = table.starts_with("actions[") || table.starts_with("new:");
    const bool new_action = table.starts_with("new:");
    const bool binding = table.starts_with("binding:");
    std::vector<std::size_t> group;
    for (std::size_t i = 0; i < fields_.size(); ++i)
        if (i == index || ((action || binding) && fields_[i].table == table))
            group.push_back(i);
    bool changed = new_action || binding;
    for (const auto i : group)
        changed = changed || fields_[i].input.text() != fields_[i].original;
    if (!changed) {
        for (const auto i : group)
            fields_[i].error.clear();
        update_error();
        return;
    }
    std::vector<core::ConfigEdit> edits;
    std::string appended, target, chord, problem;
    if (binding) {
        for (const auto i : group) {
            const auto& field = fields_[i];
            (field.key == "target" ? target : chord) = field.input.text();
        }
        if (target.empty() || chord.empty() || chord == "Default")
            problem = "Complete the target and shortcut to save this draft.";
        else if (std::ranges::any_of(fields_, [&](const Field& field) {
                     return field.table == "keybindings" && field.key == target;
                 }))
            problem = "This target already has a shortcut. Edit its existing entry.";
        else
            edits.push_back({"keybindings", target, core::quote_config_string(chord)});
    } else {
        if (new_action)
            appended = "\n[[actions]]\n";
        for (const auto i : group) {
            const auto& field = fields_[i];
            const auto value =
                field.raw ? field.input.text() : core::quote_config_string(field.input.text());
            if (new_action)
                appended += field.key + " = " + value + "\n";
            else if (field.input.text() != field.original)
                edits.push_back({field.table, field.key, value,
                                 field.table == "keybindings" && field.input.text() == "Default"});
        }
    }
    auto changed_text = core::edit_config_text(staged_, edits, appended);
    if (problem.empty())
        problem = changed_text.error;
    if (problem.empty()) {
        const auto parsed = core::parse_config(changed_text.text, path_.string());
        if (!parsed.problems.empty())
            problem = parsed.problems.front();
        else {
            const auto problems =
                configure_shortcuts(parsed.config.keybindings, parsed.config.actions, false);
            if (!problems.empty())
                problem = problems.front();
        }
    }
    for (const auto i : group)
        fields_[i].error = problem;
    if (problem.empty()) {
        staged_ = std::move(changed_text.text);
        saved_ = false;
        for (const auto i : group) {
            auto& field = fields_[i];
            field.original = field.input.text();
            if (new_action) {
                field.table = std::format("actions[{}]", action_count_);
                field.group = fields_[group.front()].input.text();
            } else if (binding) {
                if (field.key == "target") {
                    field.section =
                        -1;  // Keep stable focus/field IDs while the chord becomes a regular row.
                    field.table = "saved-draft";
                } else {
                    field.table = "keybindings";
                    field.key = target;
                    field.label = target;
                    field.group = "Custom shortcuts";
                    field.description = "Chord, Default to reset, or empty to disable";
                }
            }
        }
        if (new_action)
            ++action_count_;
    }
    update_error();
}

void SettingsDialog::flush() {
    if (busy_ || !valid_ || staged_ == original_ || !write_error_.empty())
        return;
    busy_ = true;
    const auto replacement = staged_;
    const auto parsed = core::parse_config(replacement, path_.string());
    job_ = pool_.submit(
        core::Priority::visible,
        [path = path_, expected = original_, existed = existed_,
         replacement](const core::CancelToken& token) -> std::string {
            const auto current = core::read_text_file(path);
            if (!current.error.empty())
                return current.error;
            if (current.exists != existed || current.text != expected)
                return "Config changed outside Settings. Keep a copy of your edits, then close and "
                       "reopen Settings.";
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
        [this, parsed, replacement](std::string error) {
            busy_ = false;
            write_error_ = std::move(error);
            if (write_error_.empty()) {
                original_ = replacement;
                existed_ = saved_ = true;
                if (applied_)
                    applied_(parsed);
                flush();  // The next snapshot includes edits made while this write was running.
            }
            update_error();
            maybe_close();
            redraw_();
        });
    redraw_();
}

void SettingsDialog::save() {
    if (!valid_)
        return;
    // Actions first: a new shortcut can refer to an action completed in this same commit.
    std::unordered_set<std::string> groups;
    for (int pass = 0; pass < 2; ++pass)
        for (std::size_t i = 0; i < fields_.size(); ++i) {
            const auto& field = fields_[i];
            if ((field.section == 3) != (pass == 0) || field.section < 0)
                continue;
            const bool grouped = field.table.starts_with("new:") ||
                                 field.table.starts_with("binding:") ||
                                 field.table.starts_with("actions[");
            if (grouped && !groups.insert(field.table).second)
                continue;
            commit(i);
        }
    write_error_.clear();
    update_error();
    flush();
    redraw_();
}

void SettingsDialog::commit_focus() {
    for (std::size_t i = 0; i < fields_.size(); ++i)
        if (focus_ == "f" + std::to_string(i)) {
            commit(i);
            flush();
            return;
        }
}

void SettingsDialog::set_focus(std::string focus) {
    if (focus_ != focus)
        commit_focus();
    focus_ = std::move(focus);
    reveal_focus();
    redraw_();
}

void SettingsDialog::change_section(int section) {
    commit_focus();
    choices_.close();
    section_ = section;
    scroller_.jump_to(0);
    scrollbar_ = ui::Scrollbar{};
    focus_ = "section" + std::to_string(section);
    for (auto& field : fields_)
        field.bounds = {};
    redraw_();
}

void SettingsDialog::layout_rows() {
    float top = 0;
    std::string group;
    for (auto& field : fields_) {
        if (field.section != section_)
            continue;
        if (group != field.group) {
            top += 36;
            group = field.group;
        }
        field.top = top;
        field.height =
            (list_.width - 44 >= 500 ? 64.0F : 94.0F) + (field.error.empty() ? 0.0F : 40.0F);
        top += field.height;
    }
    scroller_.set_extent(top + 12, list_.height);
}

void SettingsDialog::reveal_focus() {
    layout_rows();
    for (const auto& field : fields_) {
        const auto id = std::to_string(&field - fields_.data());
        if (field.section != section_ || (focus_ != "f" + id && focus_ != "minus" + id &&
                                          focus_ != "plus" + id && focus_ != "error" + id))
            continue;
        if (field.top < scroller_.offset())
            scroller_.jump_to(field.top);
        else if (field.top + field.height > scroller_.offset() + list_.height)
            scroller_.jump_to(field.top + std::min(field.height, list_.height) - list_.height);
        return;
    }
}

std::string SettingsDialog::choice_label(const Field& field, std::string_view value) {
    if (field.key == "preview_position")
        return std::string{i18n::tr(value == "right" ? "Right side" : "Below Places")};
    if (field.key == "palette_source") {
        if (value == "auto")
            return std::string{i18n::tr("Automatic")};
        if (value == "desktop")
            return std::string{i18n::tr("Desktop")};
        if (value == "file")
            return std::string{i18n::tr("Palette file")};
        return std::string{i18n::tr("Built-in")};
    }
    if (field.key == "color_scheme" && value == "system")
        return std::string{i18n::tr("Follow source")};
    if (field.key == "mode") {
        if (value == "session")
            return std::string{i18n::tr("Restore tabs and panes")};
        if (value == "folder")
            return std::string{i18n::tr("Specific folder")};
        if (value == "last")
            return std::string{i18n::tr("Last folder")};
        if (value == "home")
            return std::string{i18n::tr("Home")};
    }
    if (value == "system")
        return std::string{i18n::tr("Follow desktop")};
    if (value == "true" || value == "false")
        return std::string{i18n::tr(value == "true" ? "On" : "Off")};
    if (value == "light")
        return std::string{i18n::tr("Light")};
    if (value == "dark")
        return std::string{i18n::tr("Dark")};
    if (value == "always")
        return std::string{i18n::tr("Always")};
    if (value == "never")
        return std::string{i18n::tr("Never")};
    if (value == "auto")
        return std::string{i18n::tr("Automatic")};
    if (value == "low")
        return std::string{i18n::tr("Low")};
    if (value == "high")
        return std::string{i18n::tr("High")};
    return std::string{value};
}

void SettingsDialog::step_number(std::size_t index, int direction) {
    auto& field = fields_[index];
    if (!field.number)
        return;
    float value = 0;
    const auto& text = field.input.text();
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        !std::isfinite(value)) {
        commit(index);
        redraw_();
        return;
    }
    const auto number = *field.number;
    value = std::clamp(value + static_cast<float>(direction) * number.step, number.minimum,
                       number.maximum);
    field.input.set_text(std::format("{:.6g}", value));
    commit(index);
    flush();
    redraw_();
}

std::string SettingsDialog::palette_status() const {
    if (!theme_status_)
        return {};
    const auto status = theme_status_();
    using Source = theme::Theme::Source;
    std::string source;
    switch (status.active) {
        case Source::file:
            source = i18n::tr("Palette file");
            break;
        case Source::desktop:
            source = i18n::tr("Desktop");
            break;
        case Source::automatic:
        case Source::builtin:
            source =
                status.requested == Source::desktop ? i18n::tr("Built-in (desktop unavailable)")
                : status.requested == Source::file ? i18n::tr("Built-in (palette file unavailable)")
                                                   : i18n::tr("Built-in");
            break;
    }
    if (status.dark_override)
        return *status.dark_override ? i18n::trf("Active palette: {} · dark override", source)
                                     : i18n::trf("Active palette: {} · light override", source);
    return i18n::trf("Active palette: {}", source);
}

void SettingsDialog::activate(std::string_view id, bool keyboard) {
    if (id == "details" && !error_.empty()) {
        details_.push(error_);
        return;
    }
    if (id.starts_with("error")) {
        for (std::size_t i = 0; i < fields_.size(); ++i)
            if (id == "error" + std::to_string(i) && !fields_[i].error.empty())
                details_.push(fields_[i].error);
        return;
    }
    if (id == "close") {
        close();
        return;
    }
    if (id == "file" && open_file_) {
        commit_focus();
        open_file_();
        return;
    }
    if (id == "discard" && discard_prompt_ && !busy_) {
        finish_close();
        return;
    }
    if (id == "retry") {
        save();
        return;
    }
    if (!valid_)
        return;
    if (id == "add") {
        add_action();
        return;
    }
    if (id == "add_binding") {
        add_keybinding();
        return;
    }
    if (id == "category") {
        std::vector<MenuItem> items;
        for (std::size_t i = 0; i < kSections.size(); ++i) {
            MenuItem item;
            item.label = i18n::tr(kSections[i]);
            item.radio = true;
            item.checked = section_ == static_cast<int>(i);
            item.action = [this, i] { change_section(static_cast<int>(i)); };
            items.push_back(std::move(item));
        }
        const auto button = std::ranges::find_if(
            buttons_, [](const Button& each) { return each.id == "category"; });
        if (button != buttons_.end())
            choices_.open(button->bounds.x, button->bounds.bottom(), std::move(items), keyboard);
        return;
    }
    if (id.starts_with("section")) {
        change_section(id.back() - '0');
        return;
    }
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        auto& field = fields_[i];
        if (id == "minus" + std::to_string(i) || id == "plus" + std::to_string(i)) {
            step_number(i, id.starts_with("minus") ? -1 : 1);
            return;
        }
        if (id != "f" + std::to_string(i))
            continue;
        if (field.toggle()) {
            field.input.set_text(field.input.text() == "true" ? "false" : "true");
            commit(i);
            flush();
        } else if (!field.choices.empty()) {
            std::vector<MenuItem> items;
            for (const auto& value : field.choices) {
                MenuItem item;
                item.id = value;
                item.label = choice_label(field, value);
                item.radio = true;
                item.checked = value == field.input.text();
                item.action = [this, i, value] {
                    fields_[i].input.set_text(value);
                    commit(i);
                    flush();
                    redraw_();
                };
                items.push_back(std::move(item));
            }
            choices_.open(field.bounds.x, field.bounds.bottom(), std::move(items), keyboard);
            if (keyboard) {
                const auto current = std::ranges::find(field.choices, field.input.text());
                for (auto at = field.choices.begin(); at != current && at != field.choices.end();
                     ++at)
                    choices_.key({.keysym = XKB_KEY_Down, .text = {}});
            }
        } else {
            commit(i);
            flush();
        }
        redraw_();
        return;
    }
}

bool SettingsDialog::key(const ui::KeyEvent& event) {
    if (!open_)
        return false;
    if (details_.key(event))
        return true;
    if (choices_.is_open()) {
        if (event.keysym == XKB_KEY_Tab || event.keysym == XKB_KEY_ISO_Left_Tab)
            choices_.close();
        else {
            auto choice_event = event;
            if (choice_event.keysym == XKB_KEY_space)
                choice_event.keysym = XKB_KEY_Return;
            return choices_.key(choice_event);
        }
    }
    if (event.keysym == XKB_KEY_Escape) {
        close();
        return true;
    }
    if (event.modifiers.alt && event.keysym >= XKB_KEY_1 && event.keysym <= XKB_KEY_4) {
        change_section(static_cast<int>(event.keysym - XKB_KEY_1));
        return true;
    }
    if (event.modifiers.ctrl && (event.keysym == XKB_KEY_s || event.keysym == XKB_KEY_S)) {
        save();
        return true;
    }
    if (event.keysym == XKB_KEY_Tab || event.keysym == XKB_KEY_ISO_Left_Tab) {
        std::vector<std::string> order;
        if (last_card_.width > 0 && last_card_.width < 640)
            order.push_back("category");
        else
            for (std::size_t i = 0; i < kSections.size(); ++i)
                order.push_back("section" + std::to_string(i));
        for (std::size_t i = 0; i < fields_.size(); ++i) {
            const auto& field = fields_[i];
            if (field.section != section_)
                continue;
            order.push_back("f" + std::to_string(i));
            if (field.number) {
                order.push_back("minus" + std::to_string(i));
                order.push_back("plus" + std::to_string(i));
            }
            if (!field.error.empty())
                order.push_back("error" + std::to_string(i));
        }
        if (section_ == 3)
            order.push_back("add");
        if (section_ == 2)
            order.push_back("add_binding");
        if (!error_.empty())
            order.push_back("details");
        if (!write_error_.empty())
            order.push_back("retry");
        if (discard_prompt_)
            order.push_back("discard");
        order.insert(order.end(), {"file", "close"});
        const auto found = std::ranges::find(order, focus_);
        const auto at = found == order.end() ? 0 : static_cast<std::size_t>(found - order.begin());
        set_focus(order[(at + (event.modifiers.shift || event.keysym == XKB_KEY_ISO_Left_Tab
                                   ? order.size() - 1
                                   : 1)) %
                        order.size()]);
        return true;
    }
    if (focus_.starts_with("section") &&
        (event.keysym == XKB_KEY_Up || event.keysym == XKB_KEY_Down)) {
        change_section((section_ + (event.keysym == XKB_KEY_Down ? 1 : 3)) % 4);
        return true;
    }
    if (event.keysym == XKB_KEY_Return || event.keysym == XKB_KEY_KP_Enter ||
        (event.keysym == XKB_KEY_space && (!focus_.starts_with("f") || focus_ == "file"))) {
        const auto id = focus_;
        activate(id, true);
        return true;
    }
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        auto& field = fields_[i];
        if (focus_ != "f" + std::to_string(i))
            continue;
        if (field.number && (event.keysym == XKB_KEY_Up || event.keysym == XKB_KEY_Down))
            step_number(i, event.keysym == XKB_KEY_Up ? 1 : -1);
        else if (!field.choices.empty()) {
            if (event.keysym == XKB_KEY_space || event.keysym == XKB_KEY_Left ||
                event.keysym == XKB_KEY_Right)
                activate(focus_, true);
        } else if (field.input.key(event))
            redraw_();
        return true;
    }
    return true;
}

std::string SettingsDialog::copy_selection(bool cut) {
    if (details_.is_open())
        return details_.detail();
    if (!valid_ || !open_ || choices_.is_open())
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
    if (!valid_ || !open_ || choices_.is_open() || details_.is_open())
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
    if (details_.is_open())
        return details_.clickable_at(x, y) ? ui::CursorShape::pointer : ui::CursorShape::standard;
    if (choices_.is_open())
        return ui::CursorShape::pointer;
    if (scrollbar_.dragging() || scrollbar_.hit(scroller_, x, y))
        return ui::CursorShape::pointer;
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
    if (!open_)
        return;
    if (details_.is_open()) {
        details_.pointer(event);
        return;
    }
    if (choices_.pointer(event))
        return;
    if (event.kind != ui::PointerEvent::Kind::press)
        for (auto& field : fields_)
            if (field.section == section_ && field.choices.empty() && field.input.pointer(event)) {
                redraw_();
                return;
            }
    if (scrollbar_.pointer(event, scroller_)) {
        redraw_();
        return;
    }
    if (event.kind == ui::PointerEvent::Kind::scroll && list_.contains(event.x, event.y)) {
        scroller_.jump_to(scroller_.offset() +
                          (event.wheel_y != 0 ? event.wheel_y * 48.0F : event.scroll_y));
        redraw_();
        return;
    }
    if (event.kind != ui::PointerEvent::Kind::press || event.button != ui::PointerButton::left)
        return;
    for (const auto& button : buttons_)
        if (button.bounds.contains(event.x, event.y)) {
            const auto id = button.id;
            set_focus(id);
            activate(id);
            return;
        }
    if (!valid_ || !list_.contains(event.x, event.y))
        return;
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        auto& field = fields_[i];
        if (field.section == section_ && field.bounds.contains(event.x, event.y)) {
            set_focus("f" + std::to_string(i));
            if (field.choices.empty())
                field.input.pointer(event);
            else
                activate(focus_);
            return;
        }
    }
    commit_focus();
}

void SettingsDialog::draw(ui::Renderer& renderer, const ui::Rect& bounds,
                          const theme::Palette& palette, ui::IconSet& icons) {
    if (!open_)
        return;
    auto frame = DialogFrame::fit(bounds, 880, 740);
    const bool narrow = frame.card.width < 640;
    const bool compact_retry = narrow && valid_ && !write_error_.empty();
    if (discard_prompt_ || compact_retry)
        frame = DialogFrame::fit(bounds, 880, 740, 104);
    if (!last_card_.empty() &&
        (frame.card.width != last_card_.width || frame.card.height != last_card_.height ||
         frame.card.x != last_card_.x || frame.card.y != last_card_.y))
        choices_.close();
    const bool resized =
        frame.card.width != last_card_.width || frame.card.height != last_card_.height;
    last_card_ = frame.card;
    frame.draw(renderer, bounds, palette, i18n::tr("Settings"));
    renderer.push_clip(frame.card);
    buttons_.clear();
    const auto button = [&](std::string id, std::string label, ui::Rect rect,
                            DialogButton style = DialogButton::normal, bool in_list = false) {
        const auto hit = in_list ? rect.intersect(list_) : rect.intersect(frame.card);
        buttons_.push_back({id, label, hit});
        draw_dialog_button(renderer, rect, palette, i18n::tr(label), focus_ == id, style);
    };
    const std::string status = !error_.empty() ? "Changes need attention"
                               : busy_         ? (valid_ ? "Saving…" : "Loading…")
                               : has_drafts()  ? "Editing…"
                               : saved_        ? "Saved"
                                               : "Changes save automatically";
    const float status_width = narrow ? std::max(1.0F, frame.header.width - 40) : 230.0F;
    const float status_x = narrow ? frame.header.x + 20 : frame.header.right() - status_width - 20;
    if (!error_.empty())
        button("details", "Changes need attention",
               {status_x, frame.header.y + (narrow ? 40.0F : 14.0F), status_width,
                narrow ? 18.0F : 32.0F});
    else
        renderer.draw_text(i18n::tr(status), status_x, frame.header.y + (narrow ? 43.0F : 22.0F),
                           narrow ? 10.0F : 12.0F, palette.on_surface_variant, status_width);
    auto content = frame.body;
    if (narrow) {
        button("category",
               std::string{i18n::tr(kSections[static_cast<std::size_t>(section_)])} + "  ▾",
               {content.x + 16, content.y + 8, std::max(1.0F, content.width - 32), 32});
        if (focus_.starts_with("section"))
            focus_ = "category";
        content.y += 48;
        content.height = std::max(0.0F, content.height - 48);
    } else {
        if (focus_ == "category")
            focus_ = "section" + std::to_string(section_);
        const float nav_width = frame.card.width < 800 ? 172.0F : 188.0F;
        for (std::size_t i = 0; i < kSections.size(); ++i)
            button(
                "section" + std::to_string(i), kSections[i],
                {content.x + 12, content.y + 16 + static_cast<float>(i) * 44, nav_width - 24, 36},
                section_ == static_cast<int>(i) ? DialogButton::primary : DialogButton::normal);
        renderer.fill_rect({content.x + nav_width, content.y, 1, content.height},
                           palette.outline_variant);
        content.x += nav_width;
        content.width -= nav_width;
    }
    if (section_ == 2 || section_ == 3) {
        button(section_ == 2 ? "add_binding" : "add", section_ == 2 ? "New shortcut" : "Add action",
               {content.x + 16, content.bottom() - 42, std::min(160.0F, content.width - 32), 32});
        content.height = std::max(0.0F, content.height - 48);
    }
    if (section_ == 1 && theme_status_) {
        renderer.draw_text(palette_status(), content.x + 16, content.y + 8, 12,
                           palette.on_surface_variant, std::max(1.0F, content.width - 32));
        content.y += 28;
        content.height = std::max(0.0F, content.height - 28);
    }
    list_ = content;
    layout_rows();
    if (resized)
        reveal_focus();
    scrollbar_.layout(list_);
    renderer.push_clip(list_);
    std::string group;
    bool any = false;
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        auto& field = fields_[i];
        field.bounds = {};
        if (field.section != section_)
            continue;
        any = true;
        const float y = list_.y + field.top - scroller_.offset();
        const ui::Rect row{list_.x + 16, y, std::max(1.0F, list_.width - 44), field.height};
        if (group != field.group) {
            renderer.draw_text(i18n::tr(field.group), row.x, y - 24, 14, palette.primary,
                               row.width);
            group = field.group;
        }
        if (y + field.height <= list_.y || y >= list_.bottom())
            continue;
        const auto control = draw_setting_row(renderer, row, palette, i18n::tr(field.label),
                                              i18n::tr(field.description));
        field.bounds = control;
        const bool focused = focus_ == "f" + std::to_string(i);
        if (field.toggle()) {
            const bool on = field.input.text() == "true";
            const ui::Rect toggle{control.x, control.y + 4, 44, 24};
            renderer.draw_box(toggle,
                              {.fill = on ? palette.primary : palette.surface_container_highest,
                               .radius = ui::Corners::all(12),
                               .border_width = focused ? 2.0F : 1.0F,
                               .border = focused ? palette.on_surface : palette.outline});
            renderer.draw_box({toggle.x + (on ? 24.0F : 4.0F), toggle.y + 4, 16, 16},
                              {.fill = on ? palette.on_primary : palette.on_surface_variant,
                               .radius = ui::Corners::all(8)});
            renderer.draw_text(choice_label(field, field.input.text()), control.x + 54,
                               control.y + 8, 13, palette.on_surface);
        } else if (!field.choices.empty()) {
            draw_dialog_button(renderer, control, palette, choice_label(field, field.input.text()),
                               focused);
            icons.draw(renderer, "chevron-down", control.right() - 24, control.y + 8, 16,
                       palette.on_surface_variant);
        } else {
            if (field.number) {
                field.bounds.width = std::max(1.0F, control.width - 76);
                button("minus" + std::to_string(i), "−", {control.right() - 70, control.y, 32, 32},
                       DialogButton::normal, true);
                button("plus" + std::to_string(i), "+", {control.right() - 32, control.y, 32, 32},
                       DialogButton::normal, true);
            }
            field.input.draw(
                renderer, field.bounds, 13,
                {.background = palette.surface_container_highest,
                 .border = !field.error.empty() ? palette.error
                           : focused            ? palette.primary
                                                : palette.outline_variant,
                 .text = palette.on_surface,
                 .selection = palette.secondary_container,
                 .caret = focused ? palette.on_surface : palette.on_surface.with_alpha(0)});
        }
        if (!field.error.empty()) {
            const float error_y = y + field.height - 38;
            renderer.draw_text(i18n::tr(field.error), row.x, error_y + 8, 11, palette.error,
                               std::max(1.0F, row.width - 82));
            button("error" + std::to_string(i), "Details", {row.right() - 76, error_y, 76, 30},
                   DialogButton::normal, true);
        }
        renderer.fill_rect({row.x, y + field.height - 8, row.width, 1},
                           palette.outline_variant.with_alpha(0.5F));
    }
    if (!any && valid_)
        renderer.draw_text(i18n::tr("No custom actions yet. Choose Add action to create one."),
                           list_.x + 16, list_.y + 20, 13, palette.on_surface_variant,
                           std::max(1.0F, list_.width - 32));
    renderer.fill_rect(scrollbar_.thumb(scroller_), palette.outline);
    renderer.pop_clip();
    const auto button_width = [&](std::string_view label, float minimum) {
        return std::max(minimum,
                        renderer.fonts().layout(i18n::tr(label), 13, renderer.scale()).width /
                                renderer.scale() +
                            24);
    };
    const float bottom = frame.footer.bottom() - 44;
    const float close_width = button_width("Close", 80);
    const float file_width = std::min(button_width("Open config", 112),
                                      std::max(1.0F, frame.footer.width - close_width - 48));
    const float retry_width = button_width("Retry", 68);
    button("file", "Open config", {frame.footer.x + 16, bottom, file_width, 32});
    button("close", "Close", {frame.footer.right() - close_width - 16, bottom, close_width, 32},
           DialogButton::primary);
    if (!write_error_.empty() && valid_) {
        const ui::Rect retry =
            compact_retry ? ui::Rect{frame.footer.x + 16, frame.footer.y + 8, retry_width, 32}
                          : ui::Rect{frame.footer.right() - close_width - retry_width - 24, bottom,
                                     retry_width, 32};
        button("retry", "Retry", retry);
    }
    if (discard_prompt_) {
        const float discard_width = button_width("Discard unsaved", 144);
        if (!compact_retry)
            renderer.draw_text(i18n::tr("Unsaved edits remain."), frame.footer.x + 16,
                               frame.footer.y + 15, 12, palette.error,
                               std::max(1.0F, frame.footer.width - discard_width - 48));
        button("discard", "Discard unsaved",
               {frame.footer.right() - discard_width - 16, frame.footer.y + 8, discard_width, 32});
    }
    renderer.pop_clip();
    choices_.draw(renderer, bounds, palette);
    details_.draw(renderer, bounds, palette);
}
}  // namespace rummage::views
