#include "rummage/views/settings_dialog.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>
#include <unordered_set>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "draw_helpers.hpp"
#include "rummage/core/actions.hpp"
#include "rummage/core/config_edit.hpp"
#include "rummage/core/file.hpp"
#include "rummage/core/i18n.hpp"
#include "rummage/views/dialog_chrome.hpp"
#include "rummage/views/selectable_text.hpp"
#include "rummage/views/shortcuts.hpp"

namespace rummage::views {
namespace {
constexpr std::array kSections{
    i18n::mark("General"), i18n::mark("Appearance"), i18n::mark("Keyboard shortcuts"),
    i18n::mark("Actions"), i18n::mark("Menus"),      i18n::mark("Effects")};
constexpr std::array kMenuContexts{"Common layout", "Files and folders", "Folder background",
                                   "Trash", "Sidebar"};
constexpr int kEffects = 5, kShortcuts = 2, kActions = 3;
constexpr std::size_t kBackgroundSlot = core::kMotionEvents.size();
constexpr std::size_t kSelectionSlot = kBackgroundSlot + 1;
constexpr std::array kEffectSlots{
    i18n::mark("Open directory"),   i18n::mark("Go back"),         i18n::mark("Change view"),
    i18n::mark("Copy files"),       i18n::mark("Move files"),      i18n::mark("Trash files"),
    i18n::mark("Change selection"), i18n::mark("Change focus"),    i18n::mark("Invalid operation"),
    i18n::mark("Quick Look"),       i18n::mark("Command palette"), i18n::mark("Search results"),
    i18n::mark("Background"),       i18n::mark("Selection glow")};
static_assert(kEffectSlots.size() == core::kMotionEvents.size() + 2);
constexpr std::array kEffectLabels{
    i18n::mark("None"),      i18n::mark("Fade"),         i18n::mark("Slide"),
    i18n::mark("Scale"),     i18n::mark("Iris"),         i18n::mark("Ripple"),
    i18n::mark("Ink bleed"), i18n::mark("Disintegrate"), i18n::mark("Hologram"),
    i18n::mark("Portal"),    i18n::mark("Energy trace"), i18n::mark("Focus glow"),
    i18n::mark("Glitch"),    i18n::mark("Frost"),        i18n::mark("Particles"),
    i18n::mark("Scan")};
static_assert(kEffectLabels.size() == core::kMotionEffects.size());

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
    : pool_{pool},
      redraw_{std::move(redraw)},
      choices_{redraw_},
      details_{redraw_, {}, "Settings details"},
      editors_{
          [this](const auto& edits, std::string_view append) { return apply_edits(edits, append); },
          redraw_},
      menus_{redraw_, [this](const auto& edits, std::string_view append) {
                 return apply_edits(edits, append);
             }} {}

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
    search_.set_text("");
    read_only_reason_.clear();
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
                    refresh_editability();
                    editors_.reset(parsed.config, editable());
                    menus_.discard();
                    menus_.sync(parsed.config, editable());
                    menus_.context(0);
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
    editors_.discard();
    menus_.discard();
    menus_.dismiss();
    open_ = false;
    preview_drawn_ = false;
    preview_motion_.cancel_all();
    close_requested_ = discard_prompt_ = false;
    ++generation_;
    redraw_();
}

bool SettingsDialog::has_drafts() const {
    return editors_.has_drafts() || menus_.has_draft() ||
           std::ranges::any_of(fields_, [](const Field& field) {
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
    accepted_config_ = loaded;
    const auto& config = loaded.config;
    preview_config_ = config.animations;
    preview_motion_.configure(preview_config_);
    const auto add = [&](int section, std::string_view group, std::string table, std::string key,
                         std::string_view label, std::string_view description, std::string value,
                         bool raw = false, std::vector<std::string> choices = {},
                         std::optional<Number> number = std::nullopt) {
        Field field;
        field.section = section;
        field.group = group;
        field.table = std::move(table);
        field.key = std::move(key);
        field.label = label;
        field.description = description;
        field.original = value;
        field.input.set_text(std::move(value));
        field.raw = raw;
        field.resettable = section < 2 || section == kEffects || field.table == "keybindings";
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
        "Automatic, Desktop, Palette file, Built-in or KDE", config.appearance.palette_source,
        false, {"auto", "desktop", "file", "builtin", "kde"});
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
    for (std::size_t index = 0; index < core::kMotionEvents.size(); ++index) {
        const auto name = std::string{core::kMotionEvents[index]};
        add(kEffects, i18n::mark("Selected slot"), "animations.events", name, i18n::mark("Effect"),
            i18n::mark("Choose the animation for this slot"),
            std::string{
                core::kMotionEffects[static_cast<std::size_t>(config.animations.events[index])]},
            false,
            std::vector<std::string>{core::kMotionEffects.begin(), core::kMotionEffects.end()});
        add(kEffects, i18n::mark("Selected slot"), "animations.durations", name,
            i18n::mark("Duration"), i18n::mark("Milliseconds before duration scaling · 0–2000"),
            std::to_string(config.animations.durations_ms[index]), true, {}, Number{0, 2000, 20});
    }
    add(kEffects, i18n::mark("Selected slot"), "animations", "hologram_spread",
        i18n::mark("Hologram spread"),
        i18n::mark("Colour separation in pixels · shared by hologram slots"),
        std::format("{:g}", config.animations.hologram_spread), true, {}, Number{0, 12, 0.5F});
    add(kEffects, i18n::mark("Selected slot"), "animations", "portal_rotation",
        i18n::mark("Portal rotation"), i18n::mark("Rotation in radians · shared by portal slots"),
        std::format("{:g}", config.animations.portal_rotation), true, {}, Number{0, 12, 0.25F});
    const auto& ambient = config.animations.ambient;
    add(kEffects, i18n::mark("Selected slot"), "animations.ambient", "background",
        i18n::mark("Enabled"), i18n::mark("Flowing accent colour behind file content"),
        ambient.background ? "true" : "false", true, {"true", "false"});
    add(kEffects, i18n::mark("Selected slot"), "animations.ambient", "shader",
        i18n::mark("Background shader"),
        i18n::mark("Absolute .frag path · empty uses the built-in effect"), ambient.shader);
    add(kEffects, i18n::mark("Selected slot"), "animations.ambient", "selection_glow",
        i18n::mark("Enabled"), i18n::mark("Breathing outline around the visible selection"),
        ambient.selection_glow ? "true" : "false", true, {"true", "false"});
    add(kEffects, i18n::mark("Selected slot"), "animations.ambient", "speed", i18n::mark("Speed"),
        i18n::mark("Shared by persistent slots · 0 freezes the effect · 0–4"),
        std::format("{:g}", ambient.speed), true, {}, Number{0, 4, 0.1F});
    add(kEffects, i18n::mark("Selected slot"), "animations.ambient", "intensity",
        i18n::mark("Intensity"),
        i18n::mark("Shared by persistent slots · 0 disables drawing · 0–1"),
        std::format("{:g}", ambient.intensity), true, {}, Number{0, 1, 0.05F});
    add(kEffects, i18n::mark("Selected slot"), "animations.ambient", "fps",
        i18n::mark("Frame rate"),
        i18n::mark("Shared by persistent slots · maximum idle rate · 1–60"),
        std::to_string(ambient.fps), true, {}, Number{1, 60, 1});
    add(kEffects, i18n::mark("All effects"), "animations", "enabled", i18n::mark("Motion effects"),
        i18n::mark("Animate changes in the browser"), config.animations.enabled ? "true" : "false",
        true, {"true", "false"});
    add(kEffects, i18n::mark("All effects"), "animations", "duration_scale",
        i18n::mark("Motion duration"), i18n::mark("Multiplier · 0–4 · 0 is immediate"),
        std::format("{:g}", config.animations.duration_scale), true, {}, Number{0, 4, 0.1F});
    const auto reduce = config.animations.reduce_motion;
    add(kEffects, i18n::mark("All effects"), "animations", "reduce_motion",
        i18n::mark("Reduce motion"), i18n::mark("Follow the desktop accessibility preference"),
        reduce == core::MotionConfig::Reduce::always  ? "always"
        : reduce == core::MotionConfig::Reduce::never ? "never"
                                                      : "system",
        false, {"system", "always", "never"});
    const auto quality = config.animations.quality;
    add(kEffects, i18n::mark("All effects"), "animations", "quality", i18n::mark("Motion quality"),
        i18n::mark("Choose the detail of animation effects"),
        quality == core::MotionConfig::Quality::low    ? "low"
        : quality == core::MotionConfig::Quality::high ? "high"
                                                       : "auto",
        false, {"auto", "low", "high"});
    auto bindings = config.keybindings;
    for (const auto& [name, label] : binding_settings()) {
        const auto found = bindings.find(name);
        add(kShortcuts, "Commands", "keybindings", name, label,
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
        add(kActions, action.id, table, "label", "Menu label",
            "Name shown in menus and the command palette", action.label);
        if (action.kind == core::Action::Kind::external)
            add(kActions, action.id, table, "argv", "Program and arguments",
                "Literal argument array; no shell expansion", arguments(action.argv), true);
        add(kActions, action.id, table, "enabled", "Enabled",
            "Make this action available when applicable", action.enabled ? "true" : "false", true,
            {"true", "false"});
    }
}

bool SettingsDialog::refresh_editability() {
    if (!valid_)
        return false;
    const auto target = core::atomic_write_target(path_);
    read_only_reason_ =
        target.error.empty()
            ? std::string{}
            : target.destination.string() + "\n" + target.error + "\n" +
                  std::string{i18n::tr("Change the declared configuration, then reopen Settings.")};
    if (!read_only_reason_.empty())
        choices_.close();
    return editable();
}

std::string SettingsDialog::apply_edits(const std::vector<core::ConfigEdit>& edits,
                                        std::string_view append) {
    if (!refresh_editability())
        return read_only_reason_.empty() ? "Settings are not loaded." : read_only_reason_;
    auto changed = core::edit_config_text(staged_, edits, append);
    if (!changed.error.empty())
        return changed.error;
    auto parsed = core::parse_config(changed.text, path_.string());
    if (!parsed.problems.empty())
        return parsed.problems.front();
    const auto problems =
        configure_shortcuts(parsed.config.keybindings, parsed.config.actions, false);
    if (!problems.empty())
        return problems.front();
    staged_ = std::move(changed.text);
    accepted_config_ = std::move(parsed);
    refresh_values(edits);
    saved_ = false;
    flush();
    redraw_();
    return {};
}

void SettingsDialog::refresh_values(const std::vector<core::ConfigEdit>& reset) {
    auto previous = std::move(fields_);
    fields_.clear();
    const auto accepted = accepted_config_;
    populate(accepted);
    for (auto& field : fields_) {
        const bool resetting = std::ranges::any_of(reset, [&](const core::ConfigEdit& edit) {
            return edit.table == field.table && edit.key == field.key;
        });
        const auto old = std::ranges::find_if(previous, [&](const Field& before) {
            return before.table == field.table && before.key == field.key;
        });
        if (!resetting && old != previous.end() && old->input.text() != old->original) {
            field.input.set_text(old->input.text());
            field.error = old->error;
        }
    }
    // Preserve incomplete action and shortcut drafts, which are not represented in Config.
    for (auto& field : previous)
        if (field.table.starts_with("new:") || field.table.starts_with("binding:"))
            fields_.push_back(std::move(field));
    update_error();
    refresh_preview();
    if (section_ == kEffects)
        restart_preview();
    layout_rows();
}

bool SettingsDialog::reset_setting(std::string_view table, std::string_view key) {
    const auto field = std::ranges::find_if(fields_, [&](const Field& item) {
        return item.table == table && item.key == key && item.resettable;
    });
    if (field == fields_.end())
        return false;
    const std::vector<core::ConfigEdit> edits{{std::string{table}, std::string{key}, "", true}};
    const auto error = apply_edits(edits);
    if (!error.empty()) {
        field->error = error;
        update_error();
        return false;
    }
    return true;
}

bool SettingsDialog::reset_category(int section) {
    if (section < 0 || (section > 2 && section != kEffects))
        return false;
    std::vector<core::ConfigEdit> edits;
    for (const auto& field : fields_)
        if (field.section == section && field.resettable)
            edits.push_back({field.table, field.key, "", true});
    const auto error = apply_edits(edits);
    if (!error.empty()) {
        write_error_ = error;
        update_error();
        return false;
    }
    return true;
}

void SettingsDialog::search(std::string query) {
    commit_focus();
    search_.set_text(std::move(query));
    scroller_.jump_to(0);
    redraw_();
}

std::vector<SettingsDialog::SearchResult> SettingsDialog::search_results() const {
    const auto folded = [](std::string text) {
        std::ranges::transform(text, text.begin(), [](unsigned char c) { return std::tolower(c); });
        return text;
    };
    const auto query = folded(search_.text());
    std::vector<SearchResult> results;
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        const auto& field = fields_[i];
        if (field.section < 0)
            continue;
        if ((field.key == "hologram_spread" &&
             std::ranges::find(preview_config_.events, core::MotionEffect::hologram) ==
                 preview_config_.events.end()) ||
            (field.key == "portal_rotation" &&
             std::ranges::find(preview_config_.events, core::MotionEffect::portal) ==
                 preview_config_.events.end()))
            continue;
        std::string label = field.label;
        std::string slot;
        if (field.table == "animations.events" || field.table == "animations.durations") {
            const auto event = core::motion_name<core::MotionEvent>(field.key, core::kMotionEvents);
            if (event) {
                slot = kEffectSlots[static_cast<std::size_t>(*event)];
                label = std::string{i18n::tr(slot)} + " · " + std::string{i18n::tr(field.label)};
            }
        }
        const auto searchable =
            std::string{i18n::tr(field.label)} + " " + std::string{i18n::tr(field.description)} +
            " " + field.label + " " + field.description + " " + field.group + " " + field.table +
            "." + field.key + " " + label + " " + slot +
            (field.section == 3 && field.key == "label" ? " " + field.original : "");
        if (folded(searchable).find(query) != std::string::npos)
            results.push_back({i, field.section, label, field.description});
    }
    for (std::size_t i = 0; i < kMenuContexts.size(); ++i) {
        const auto label = std::string{i18n::tr(kMenuContexts[i])};
        const std::string description = "Arrange menu items, separators and submenus";
        if (folded(label + " " + std::string{i18n::tr(description)} + " " + description)
                .find(query) != std::string::npos)
            results.push_back({fields_.size() + i, 4, kMenuContexts[i], description});
    }
    const auto& actions = accepted_config_.config.actions;
    for (std::size_t i = 0; i < actions.size(); ++i) {
        const auto& action = actions[i];
        const std::string description = "Edit action availability, selection and arguments";
        if (folded(action.label + " " + action.id + " " + std::string{i18n::tr(description)} + " " +
                   description)
                .find(query) != std::string::npos)
            results.push_back(
                {fields_.size() + kMenuContexts.size() + i, 3, action.label, description});
    }
    return results;
}

void SettingsDialog::jump_to_result(std::size_t field) {
    if (field >= fields_.size()) {
        if (field - fields_.size() < kMenuContexts.size()) {
            const auto context = field - fields_.size();
            search_.set_text("");
            change_section(4);
            menus_.context(context);
            set_focus("editor");
        } else {
            const auto index = field - fields_.size() - kMenuContexts.size();
            const auto& actions = accepted_config_.config.actions;
            if (index < actions.size()) {
                const auto id = actions[index].id;
                search_.set_text("");
                change_section(3);
                editors_.select_action(id);
                set_focus("editor");
            }
        }
        return;
    }
    if (fields_[field].section < 0)
        return;
    const auto section = fields_[field].section;
    const auto table = fields_[field].table;
    const auto key = fields_[field].key;
    const auto group = fields_[field].group;
    search_.set_text("");
    if (section == kEffects) {
        if (table == "animations.events" || table == "animations.durations") {
            const auto event = core::motion_name<core::MotionEvent>(key, core::kMotionEvents);
            if (event)
                effect_slot_ = static_cast<std::size_t>(*event);
        } else if (table == "animations.ambient") {
            effect_slot_ = key == "selection_glow" ? kSelectionSlot : kBackgroundSlot;
        } else if (key == "hologram_spread" || key == "portal_rotation") {
            const auto effect = key == "hologram_spread" ? core::MotionEffect::hologram
                                                         : core::MotionEffect::portal;
            if (effect_slot_ >= core::kMotionEvents.size() ||
                preview_config_.events[effect_slot_] != effect) {
                const auto found = std::ranges::find(preview_config_.events, effect);
                if (found != preview_config_.events.end())
                    effect_slot_ = static_cast<std::size_t>(found - preview_config_.events.begin());
            }
        }
    }
    change_section(section);
    if (section == 2) {
        editors_.focus_shortcut(key);
        set_focus("editor");
    } else if (section == 3) {
        editors_.select_action(group);
        set_focus("editor");
    } else {
        set_focus("f" + std::to_string(field));
    }
}

bool SettingsDialog::edit(std::string_view table, std::string_view key, std::string value) {
    if (!refresh_editability())
        return false;
    for (auto& field : fields_) {
        if (field.table == table && field.key == key) {
            field.input.set_text(std::move(value));
            refresh_preview();
            redraw_();
            return true;
        }
    }
    return false;
}

void SettingsDialog::add_action() {
    if (!refresh_editability())
        return;
    commit_focus();
    const auto table = std::format("new:{}", new_actions_++);
    const auto add = [&](std::string key, std::string label, std::string description,
                         std::string value, bool raw = false) {
        Field field;
        field.section = kActions;
        field.table = table;
        field.group = "New action";
        field.key = std::move(key);
        field.label = label;
        field.description = description;
        field.input.set_text(std::move(value));
        field.raw = raw;
        fields_.push_back(std::move(field));
    };
    add("id", "Unique ID", "Used by shortcuts and custom menu layouts",
        std::format("custom_{}", action_count_ + new_actions_));
    add("label", "Menu label", "Draft: saved when all required fields are valid",
        std::string{i18n::tr("New action")});
    add("argv", "Program and arguments", "Example: [\"xdg-open\", \"{file}\"]", "[]", true);
    change_section(kActions);
    focus_ = "f" + std::to_string(fields_.size() - 3);
    reveal_focus();
    redraw_();
}

void SettingsDialog::add_keybinding() {
    if (!refresh_editability())
        return;
    commit_focus();
    const auto table = std::format("binding:{}", new_bindings_++);
    for (const auto& [key, label, description] :
         {std::array{"target", "Target", "action:ID or go:/path (also go:~/path)"},
          std::array{"chord", "Shortcut", "Draft: enter a chord, e.g. Ctrl+Alt+J"}}) {
        Field field;
        field.section = kShortcuts;
        field.table = table;
        field.group = "New shortcut";
        field.key = key;
        field.label = label;
        field.description = description;
        fields_.push_back(std::move(field));
    }
    change_section(kShortcuts);
    focus_ = "f" + std::to_string(fields_.size() - 2);
    reveal_focus();
    redraw_();
}

void SettingsDialog::update_error() {
    error_ = write_error_;
    if (error_.empty())
        error_ = editors_.error();
    if (error_.empty())
        error_ = menus_.error();
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
    if (!refresh_editability() || index >= fields_.size() || fields_[index].section < 0)
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
        accepted_config_ = core::parse_config(staged_, path_.string());
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
                    field.resettable = true;
                    field.key = target;
                    field.label = target;
                    field.group = "Custom shortcuts";
                    field.description = "Chord, Default to reset, or empty to disable";
                }
            }
        }
        if (new_action)
            ++action_count_;
        refresh_preview();
    }
    update_error();
}

void SettingsDialog::flush() {
    if (busy_ || !valid_ || staged_ == original_ || !write_error_.empty() || !refresh_editability())
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
            const auto target = core::atomic_write_target(path);
            if (!target.error.empty())
                return target.error;
            if (const auto error = core::write_file_atomically(target.destination, replacement))
                return error.message();
            return {};
        },
        [this, parsed, replacement](std::string error) {
            busy_ = false;
            write_error_ = std::move(error);
            refresh_editability();
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
    if (!refresh_editability())
        return;
    editors_.sync(accepted_config_.config, editable());
    menus_.sync(accepted_config_.config, editable());
    editors_.commit();
    menus_.commit();
    // Actions first: a new shortcut can refer to an action completed in this same commit.
    std::unordered_set<std::string> groups;
    for (int pass = 0; pass < 2; ++pass)
        for (std::size_t i = 0; i < fields_.size(); ++i) {
            const auto& field = fields_[i];
            if ((field.section == kActions) != (pass == 0) || field.section < 0)
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
    if (focus_ == "editor") {
        if (section_ == 4)
            menus_.commit();
        else
            editors_.commit();
        update_error();
    }
    for (std::size_t i = 0; i < fields_.size(); ++i)
        if (focus_ == "f" + std::to_string(i)) {
            commit(i);
            flush();
            return;
        }
}

void SettingsDialog::set_focus(std::string focus) {
    if (focus_ != focus) {
        commit_focus();
        if (focus_ == "editor") {
            editors_.blur();
            menus_.blur();
        }
        editor_focus_pending_ = focus == "editor";
        editor_reverse_focus_ = false;
    }
    focus_ = std::move(focus);
    reveal_focus();
    redraw_();
}

void SettingsDialog::change_section(int section) {
    commit_focus();
    choices_.close();
    editors_.dismiss();
    editors_.blur();
    menus_.dismiss();
    menus_.blur();
    editor_focus_pending_ = false;
    section_ = section;
    if (section == 2 || section == 3)
        editors_.set_page(section == 2 ? SettingsEditors::Page::shortcuts
                                       : SettingsEditors::Page::actions);
    if (section_ == kEffects)
        restart_preview();
    else
        preview_motion_.cancel_all();
    scroller_.jump_to(0);
    scrollbar_ = ui::Scrollbar{};
    focus_ = "section" + std::to_string(section);
    for (auto& field : fields_)
        field.bounds = {};
    redraw_();
}

void SettingsDialog::layout_rows() {
    if (!search_.text().empty()) {
        scroller_.set_extent(static_cast<float>(search_results().size()) * 76 + 12, list_.height);
        return;
    }
    float top = 0;
    std::string group;
    for (auto& field : fields_) {
        if (!field_visible(field))
            continue;
        if (group != field.group) {
            top += 36;
            group = field.group;
        }
        field.top = top;
        field.height =
            field.layout.height +
            ((field.section < 2 || field.section == kEffects) && editable() ? 28.0F : 0.0F) +
            (field.error.empty() ? 0.0F : 40.0F);
        top += field.height;
    }
    scroller_.set_extent(top + 12, list_.height);
}

void SettingsDialog::reveal_focus() {
    layout_rows();
    for (const auto& field : fields_) {
        const auto id = std::to_string(&field - fields_.data());
        if (!field_visible(field) ||
            (focus_ != "f" + id && focus_ != "minus" + id && focus_ != "plus" + id &&
             focus_ != "error" + id && focus_ != "reset" + id))
            continue;
        const float top = field.top + (focus_ == "reset" + id
                                           ? field.height - (field.error.empty() ? 34.0F : 74.0F)
                                       : focus_ == "error" + id  ? field.height - 38
                                       : list_.width - 44 >= 500 ? 5.0F
                                                                 : 44.0F);
        if (top < scroller_.offset())
            scroller_.jump_to(top);
        else if (top + 32 > scroller_.offset() + list_.height)
            scroller_.jump_to(top + 32 - list_.height);
        return;
    }
}

std::string SettingsDialog::choice_label(const Field& field, std::string_view value) {
    if (field.table == "animations.events") {
        const auto effect = core::motion_name<core::MotionEffect>(value, core::kMotionEffects);
        return effect ? std::string{i18n::tr(kEffectLabels[static_cast<std::size_t>(*effect)])}
                      : std::string{value};
    }
    if (field.key == "preview_position")
        return std::string{i18n::tr(value == "right" ? "Right side" : "Below Places")};
    if (field.key == "palette_source") {
        if (value == "auto")
            return std::string{i18n::tr("Automatic")};
        if (value == "desktop")
            return std::string{i18n::tr("Desktop")};
        if (value == "kde")
            return std::string{i18n::tr("KDE")};
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
    if (!refresh_editability() || !field.number)
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
        case Source::kde:
            source = i18n::tr("KDE");
            break;
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
                : status.requested == Source::kde   ? i18n::tr("Built-in (KDE palette unavailable)")
                : status.requested == Source::file ? i18n::tr("Built-in (palette file unavailable)")
                                                   : i18n::tr("Built-in");
            break;
    }
    if (status.requested == Source::kde && status.kde_state == theme::Theme::FileState::invalid) {
        source += status.retaining_last_valid_kde
                      ? std::string{i18n::tr(" (reload failed; last valid palette)")}
                      : std::string{i18n::tr(" (palette file rejected)")};
    } else if (status.requested != Source::kde &&
               status.file_state == theme::Theme::FileState::invalid) {
        source += status.retaining_last_valid_file && status.active == Source::file
                      ? std::string{i18n::tr(" (reload failed; last valid palette)")}
                      : std::string{i18n::tr(" (palette file rejected)")};
    }
    if (status.dark_override)
        return *status.dark_override ? i18n::trf("Active palette: {} · dark override", source)
                                     : i18n::trf("Active palette: {} · light override", source);
    return i18n::trf("Active palette: {}", source);
}

void SettingsDialog::activate(std::string_view id, bool keyboard) {
    if (id == "palette_details" && theme_status_) {
        auto text = palette_status();
        const auto status = theme_status_();
        for (const auto& problem : status.problems())
            text += "\n" + problem;
        details_.push(std::move(text));
        return;
    }
    if (id == "readonly") {
        details_.push(read_only_reason_);
        return;
    }
    if (id.starts_with("result")) {
        jump_to_result(static_cast<std::size_t>(std::stoul(std::string{id.substr(6)})));
        return;
    }
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
    if (id == "replay") {
        refresh_preview();
        restart_preview();
        return;
    }
    if (id == "effect_slot") {
        std::vector<MenuItem> items;
        for (std::size_t i = 0; i < kEffectSlots.size(); ++i) {
            MenuItem item;
            item.label = i18n::tr(kEffectSlots[i]);
            item.radio = true;
            item.checked = effect_slot_ == i;
            item.separator = i == kBackgroundSlot;
            item.action = [this, i] { select_effect_slot(i); };
            items.push_back(std::move(item));
        }
        const auto button = std::ranges::find_if(
            buttons_, [](const Button& each) { return each.id == "effect_slot"; });
        if (button != buttons_.end()) {
            choices_.open(button->bounds.x, button->bounds.bottom(), std::move(items), keyboard);
            if (keyboard)
                for (std::size_t i = 0; i < effect_slot_; ++i)
                    choices_.key({.keysym = XKB_KEY_Down, .text = {}});
        }
        return;
    }
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
        if ((section_ <= 2 || section_ == kEffects) && editable()) {
            MenuItem reset;
            reset.label = i18n::tr("Reset category to defaults");
            reset.separator = true;
            reset.action = [this] { reset_category(section_); };
            items.push_back(std::move(reset));
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
    if (!refresh_editability())
        return;
    if (id == "reset_category") {
        reset_category(section_);
        return;
    }
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        auto& field = fields_[i];
        if (id == "reset" + std::to_string(i)) {
            reset_setting(field.table, field.key);
            return;
        }
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
                    if (!refresh_editability())
                        return;
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
    refresh_editability();
    editors_.sync(accepted_config_.config, editable());
    menus_.sync(accepted_config_.config, editable());
    if (section_ == 4 && menus_.popup_open()) {
        menus_.key(event);
        update_error();
        return true;
    }
    if ((section_ == 2 || section_ == 3) && editors_.recording()) {
        editors_.key(event);
        update_error();
        return true;
    }
    if (event.modifiers.ctrl && (event.keysym == XKB_KEY_f || event.keysym == XKB_KEY_F)) {
        set_focus("search");
        search_.select_all();
        return true;
    }
    if (event.keysym == XKB_KEY_Escape && !search_.text().empty()) {
        search("");
        return true;
    }
    if (focus_ == "search" && event.keysym != XKB_KEY_Tab && event.keysym != XKB_KEY_ISO_Left_Tab &&
        event.keysym != XKB_KEY_Escape &&
        !(event.modifiers.alt && event.keysym >= XKB_KEY_1 && event.keysym <= XKB_KEY_6) &&
        !(event.modifiers.ctrl && (event.keysym == XKB_KEY_s || event.keysym == XKB_KEY_S))) {
        if (event.keysym == XKB_KEY_Return) {
            const auto results = search_results();
            if (!results.empty())
                jump_to_result(results.front().field);
        } else if (search_.key(event)) {
            scroller_.jump_to(0);
            redraw_();
        }
        return true;
    }
    if (focus_ == "editor" && search_.text().empty() &&
        !(event.modifiers.alt && event.keysym >= XKB_KEY_1 && event.keysym <= XKB_KEY_6) &&
        !(event.modifiers.ctrl && (event.keysym == XKB_KEY_s || event.keysym == XKB_KEY_S))) {
        if (section_ == 4 ? menus_.key(event) : editors_.key(event)) {
            update_error();
            return true;
        }
    }
    if (event.keysym == XKB_KEY_Escape) {
        close();
        return true;
    }
    if (event.modifiers.alt && event.keysym >= XKB_KEY_1 && event.keysym <= XKB_KEY_6) {
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
        order.push_back("search");
        if (!search_.text().empty())
            for (const auto& result : search_results())
                order.push_back("result" + std::to_string(result.field));
        if ((section_ >= 2 && section_ < kEffects) && search_.text().empty())
            order.push_back("editor");
        if (section_ == kEffects && search_.text().empty())
            order.insert(order.end(), {"effect_slot", "replay"});
        for (std::size_t i = 0;
             (section_ < 2 || section_ == kEffects) && i < fields_.size() && search_.text().empty();
             ++i) {
            const auto& field = fields_[i];
            if (!field_visible(field))
                continue;
            order.push_back("f" + std::to_string(i));
            if (editable() && (field.section < 2 || field.section == kEffects))
                order.push_back("reset" + std::to_string(i));
            if (editable() && field.number) {
                order.push_back("minus" + std::to_string(i));
                order.push_back("plus" + std::to_string(i));
            }
            if (!field.error.empty())
                order.push_back("error" + std::to_string(i));
        }
        if (editable() && (section_ <= 2 || section_ == kEffects) && search_.text().empty() &&
            last_card_.width >= 640)
            order.push_back("reset_category");

        if (section_ == 1 && theme_status_ && !theme_status_().problems().empty())
            order.push_back("palette_details");
        if (!error_.empty())
            order.push_back("details");
        if (!write_error_.empty() && editable())
            order.push_back("retry");
        if (!read_only_reason_.empty())
            order.push_back("readonly");
        if (discard_prompt_)
            order.push_back("discard");
        order.insert(order.end(), {"file", "close"});
        const auto found = std::ranges::find(order, focus_);
        const auto at = found == order.end() ? 0 : static_cast<std::size_t>(found - order.begin());
        set_focus(order[(at + (event.modifiers.shift || event.keysym == XKB_KEY_ISO_Left_Tab
                                   ? order.size() - 1
                                   : 1)) %
                        order.size()]);
        if (focus_ == "editor")
            editor_reverse_focus_ = event.modifiers.shift || event.keysym == XKB_KEY_ISO_Left_Tab;
        return true;
    }
    if (focus_.starts_with("section") &&
        (event.keysym == XKB_KEY_Up || event.keysym == XKB_KEY_Down)) {
        change_section((section_ + (event.keysym == XKB_KEY_Down ? 1 : 5)) % 6);
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
        if (!editable()) {
            if (event.keysym == XKB_KEY_Left || event.keysym == XKB_KEY_Right ||
                event.keysym == XKB_KEY_Home || event.keysym == XKB_KEY_End ||
                (event.modifiers.ctrl && (event.keysym == XKB_KEY_a || event.keysym == XKB_KEY_A)))
                field.input.key(event);
            redraw_();
            return true;
        }
        if (field.number && (event.keysym == XKB_KEY_Up || event.keysym == XKB_KEY_Down))
            step_number(i, event.keysym == XKB_KEY_Up ? 1 : -1);
        else if (!field.choices.empty()) {
            if (event.keysym == XKB_KEY_space || event.keysym == XKB_KEY_Left ||
                event.keysym == XKB_KEY_Right)
                activate(focus_, true);
        } else if (field.input.key(event)) {
            refresh_preview();
            redraw_();
        }
        return true;
    }
    return true;
}

std::string SettingsDialog::copy_selection(bool cut) {
    if (details_.is_open())
        return details_.detail();
    if (!valid_ || !open_ || choices_.is_open())
        return {};
    if (focus_ == "editor")
        return section_ == 4 ? menus_.copy_selection(cut && editable())
                             : editors_.copy_selection(cut && editable());
    if (focus_ == "search") {
        const auto [first, last] = search_.selection();
        const auto text = search_.text().substr(first, last - first);
        if (cut) {
            search_.insert("");
            redraw_();
        }
        return text;
    }
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        auto& field = fields_[i];
        if (focus_ != "f" + std::to_string(i) || !field.choices.empty())
            continue;
        const auto [first, last] = field.input.selection();
        const auto text = field.input.text().substr(first, last - first);
        if (cut && refresh_editability() && !text.empty()) {
            field.input.insert("");
            refresh_preview();
            redraw_();
        }
        return text;
    }
    return {};
}

void SettingsDialog::paste(std::string_view text) {
    if (!valid_ || !open_ || choices_.is_open() || details_.is_open())
        return;
    if (focus_ == "search") {
        search_.insert(text.substr(0, text.find_first_of("\r\n")));
        scroller_.jump_to(0);
        redraw_();
        return;
    }
    if (!refresh_editability())
        return;
    if (focus_ == "editor") {
        if (section_ == 4)
            menus_.paste(text);
        else
            editors_.paste(text);
        return;
    }
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        auto& field = fields_[i];
        if (focus_ == "f" + std::to_string(i) && field.choices.empty()) {
            field.input.insert(text.substr(0, text.find_first_of("\r\n")));
            refresh_preview();
            redraw_();
        }
    }
}

ui::CursorShape SettingsDialog::cursor_at(float x, float y) const {
    if (details_.is_open())
        return details_.clickable_at(x, y) ? ui::CursorShape::pointer : ui::CursorShape::standard;
    if (choices_.is_open())
        return ui::CursorShape::pointer;
    if (section_ >= 2 && section_ < kEffects && editor_bounds_.contains(x, y) &&
        search_.text().empty())
        return section_ == 4 ? ui::CursorShape::standard : editors_.cursor_at(x, y);
    if (search_bounds_.contains(x, y))
        return ui::CursorShape::text;
    if (scrollbar_.dragging() || scrollbar_.hit(scroller_, x, y))
        return ui::CursorShape::pointer;
    for (const auto& button : buttons_)
        if (button.bounds.contains(x, y))
            return ui::CursorShape::pointer;
    if (list_.contains(x, y))
        for (const auto& field : fields_)
            if (field_visible(field) && field.bounds.contains(x, y))
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
    refresh_editability();
    editors_.sync(accepted_config_.config, editable());
    menus_.sync(accepted_config_.config, editable());
    if (section_ == 4 && menus_.popup_open()) {
        menus_.pointer(event);
        update_error();
        return;
    }
    if (choices_.pointer(event))
        return;
    if ((event.kind == ui::PointerEvent::Kind::press &&
         search_bounds_.contains(event.x, event.y)) ||
        (event.kind != ui::PointerEvent::Kind::press && focus_ == "search")) {
        if (event.kind == ui::PointerEvent::Kind::press)
            set_focus("search");
        if (search_.pointer(event)) {
            redraw_();
            return;
        }
    }
    if (section_ >= 2 && section_ < kEffects && search_.text().empty() &&
        editor_bounds_.contains(event.x, event.y)) {
        if (event.kind == ui::PointerEvent::Kind::press) {
            set_focus("editor");
            editor_focus_pending_ = false;
        }
        if (section_ == 4)
            menus_.pointer(event);
        else
            editors_.pointer(event);
        update_error();
        return;
    }
    if (event.kind != ui::PointerEvent::Kind::press)
        for (auto& field : fields_)
            if (field_visible(field) && field.choices.empty() && field.input.pointer(event)) {
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
        if (field_visible(field) && field.bounds.contains(event.x, event.y)) {
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
    refresh_editability();
    editors_.sync(accepted_config_.config, editable());
    menus_.sync(accepted_config_.config, editable());
    preview_drawn_ = false;
    auto frame = DialogFrame::fit(bounds, 880, 740);
    const bool narrow = frame.card.width < 640;
    const bool compact_retry = narrow && valid_ && !write_error_.empty();
    if (discard_prompt_ || compact_retry)
        frame = DialogFrame::fit(bounds, 880, 740, 104);
    const std::string status = !read_only_reason_.empty() ? "Read-only configuration"
                               : !error_.empty()          ? "Changes need attention"
                               : busy_                    ? (valid_ ? "Saving…" : "Loading…")
                               : has_drafts()             ? "Editing…"
                               : saved_                   ? "Saved"
                                                          : "Changes save automatically";
    const float title_width = text_width(renderer, i18n::tr("Settings"), 18);
    const float desired_status_width =
        text_width(renderer, i18n::tr(status),
                   error_.empty() && read_only_reason_.empty() ? 12 : 13) +
        20;
    const bool stacked_status =
        narrow || title_width + desired_status_width + 64 > frame.header.width;
    const float status_width =
        stacked_status ? std::max(1.0F, frame.header.width - 40) : desired_status_width;
    const float status_height =
        SelectableText::height(renderer, i18n::tr(status), status_width, 12, 16);
    if (stacked_status) {
        const float extra =
            std::min(frame.body.height, std::max(0.0F, 44 + status_height - frame.header.height));
        frame.header.height += extra;
        frame.body.y += extra;
        frame.body.height -= extra;
    }
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
    const auto button = [&](std::string id, std::string_view label, ui::Rect rect,
                            DialogButton style = DialogButton::normal, bool in_list = false) {
        const auto hit = in_list ? rect.intersect(list_) : rect.intersect(frame.card);
        buttons_.push_back({id, std::string{label}, hit});
        draw_dialog_button(renderer, rect, palette, i18n::tr(label), focus_ == id, style);
    };
    const float status_x =
        stacked_status ? frame.header.x + 20 : frame.header.right() - status_width - 20;
    if (!read_only_reason_.empty())
        button("readonly", "Read-only configuration",
               {status_x, frame.header.y + (stacked_status ? 40.0F : 14.0F), status_width,
                stacked_status ? 18.0F : 32.0F});
    else if (!error_.empty())
        button("details", "Changes need attention",
               {status_x, frame.header.y + (stacked_status ? 40.0F : 14.0F), status_width,
                stacked_status ? 18.0F : 32.0F});
    else {
        SelectableText text;
        text.begin_frame(frame.header);
        text.draw(renderer, "status", i18n::tr(status),
                  {status_x, frame.header.y + (stacked_status ? 40.0F : 22.0F), status_width,
                   status_height},
                  12, palette.on_surface_variant, {}, 16);
    }
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
    search_bounds_ = {content.x + 16, content.y + 8, std::max(1.0F, content.width - 32), 32};
    search_.draw(
        renderer, search_bounds_, 13,
        {.background = palette.surface_container_highest,
         .border = focus_ == "search" ? palette.primary : palette.outline_variant,
         .text = palette.on_surface,
         .selection = theme::readable_foreground(palette.secondary_container, palette.on_surface),
         .caret = focus_ == "search" ? palette.on_surface : palette.on_surface.with_alpha(0)});
    if (search_.text().empty() && focus_ != "search")
        renderer.draw_text(i18n::tr("Search settings… (Ctrl+F)"), search_bounds_.x + 6,
                           search_bounds_.y + 8, 13, palette.on_surface_variant,
                           search_bounds_.width - 12);
    content.y += 48;
    content.height = std::max(0.0F, content.height - 48);
    if (!read_only_reason_.empty()) {
        renderer.draw_text(i18n::tr("Edit the declared configuration to change settings."),
                           content.x + 16, content.y + 4, 11, palette.on_surface_variant,
                           std::max(1.0F, content.width - 32));
        content.y += 28;
        content.height = std::max(0.0F, content.height - 28);
    }
    if ((section_ <= 2 || section_ == kEffects) && editable() && search_.text().empty() &&
        !narrow) {
        button("reset_category", "Reset category to defaults",
               {content.x + 16, content.bottom() - 42, std::max(1.0F, content.width - 32), 32});
        content.height = std::max(0.0F, content.height - 48);
    }
    if (section_ == 1 && theme_status_ && search_.text().empty()) {
        const auto palette_state = theme_status_();
        const bool diagnostics = !palette_state.problems().empty();
        renderer.draw_text(palette_status(), content.x + 16, content.y + 8, 12,
                           palette.on_surface_variant,
                           std::max(1.0F, content.width - (diagnostics ? 116 : 32)));
        if (diagnostics)
            button("palette_details", "Details", {content.right() - 92, content.y, 76, 28});
        content.y += 28;
        content.height = std::max(0.0F, content.height - 28);
    }
    if (section_ == kEffects && valid_ && search_.text().empty()) {
        const float replay_width =
            std::clamp(renderer.fonts().layout(i18n::tr("Replay"), 13, renderer.scale()).width /
                               renderer.scale() +
                           24,
                       88.0F, std::max(88.0F, content.width * 0.4F));
        button(
            "effect_slot",
            std::string{i18n::tr("Slot")} + ": " +
                std::string{i18n::tr(kEffectSlots[effect_slot_])} + "  ▾",
            {content.x + 16, content.y + 4, std::max(1.0F, content.width - replay_width - 40), 32});
        button("replay", i18n::mark("Replay"),
               {content.right() - replay_width - 16, content.y + 4, replay_width, 32});
        content.y += 44;
        content.height = std::max(0.0F, content.height - 44);
        const float preview_height = std::clamp(content.height * 0.3F, 64.0F, 136.0F);
        if (content.height >= 100) {
            draw_preview(
                renderer,
                {content.x + 16, content.y, std::max(1.0F, content.width - 32), preview_height},
                palette, icons);
            content.y += preview_height + 8;
            content.height = std::max(0.0F, content.height - preview_height - 8);
        } else {
            preview_motion_.cancel_all();
        }
    }
    list_ = content;
    editor_bounds_ = {};
    const bool embedded = section_ >= 2 && section_ < kEffects && search_.text().empty();
    if (embedded) {
        editor_bounds_ = content;
        if (section_ == 4)
            menus_.draw(renderer, content, palette);
        else
            editors_.draw(renderer, content, palette);
        if (focus_ == "editor" && editor_focus_pending_) {
            if (section_ == 4)
                menus_.enter_focus(editor_reverse_focus_);
            else
                editors_.enter_focus(editor_reverse_focus_);
            editor_focus_pending_ = false;
        }
    }
    // Keep one aligned control column, sized for the longest translated option.
    float control_width = 200;
    for (const auto& field : fields_) {
        if (!field_visible(field))
            continue;
        for (const auto& choice : field.choices)
            control_width =
                std::max(control_width, text_width(renderer, choice_label(field, choice), 13) + 44);
    }
    bool rows_changed = false;
    for (auto& field : fields_) {
        if (!field_visible(field))
            continue;
        const auto control_label = !field.choices.empty() && !field.toggle()
                                       ? choice_label(field, field.input.text())
                                       : std::string{};
        const auto layout =
            measure_setting_row(renderer, list_.width - 44, i18n::tr(field.label),
                                i18n::tr(field.description), control_width, control_label);
        rows_changed |= layout.height != field.layout.height;
        field.layout = layout;
    }
    layout_rows();
    if (resized || rows_changed)
        reveal_focus();
    scrollbar_.layout(list_);
    renderer.push_clip(list_);
    std::string group;
    bool any = false;
    if (!search_.text().empty()) {
        const auto results = search_results();
        scroller_.set_extent(static_cast<float>(results.size()) * 76 + 12, list_.height);
        for (std::size_t n = 0; n < results.size(); ++n) {
            const auto& result = results[n];
            const float y = list_.y + static_cast<float>(n) * 76 + 8 - scroller_.offset();
            button("result" + std::to_string(result.field),
                   std::string{i18n::tr(kSections[static_cast<std::size_t>(result.section)])} +
                       " · " + std::string{i18n::tr(result.label)},
                   {list_.x + 16, y, std::max(1.0F, list_.width - 44), 32}, DialogButton::normal,
                   true);
            renderer.draw_text(i18n::tr(result.description), list_.x + 20, y + 40, 11,
                               palette.on_surface_variant, std::max(1.0F, list_.width - 48));
        }
        if (results.empty())
            renderer.draw_text(i18n::tr("No settings match your search."), list_.x + 16,
                               list_.y + 20, 13, palette.on_surface_variant, list_.width - 32);
    }
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        auto& field = fields_[i];
        field.bounds = {};
        if (embedded || !field_visible(field) || !search_.text().empty())
            continue;
        any = true;
        const float y = list_.y + field.top - scroller_.offset();
        const ui::Rect row{list_.x + 16, y, std::max(1.0F, list_.width - 44), field.height};
        if (group != field.group) {
            renderer.draw_text(
                i18n::tr(field.group), row.x, y - 24, 14,
                theme::readable_foreground(palette.primary, palette.surface_container_high),
                row.width);
            group = field.group;
        }
        if (y + field.height <= list_.y || y >= list_.bottom())
            continue;
        const auto control = draw_setting_row(renderer, row, palette, i18n::tr(field.label),
                                              i18n::tr(field.description), field.layout);
        field.bounds = control;
        const bool focused = focus_ == "f" + std::to_string(i);
        if (field.toggle()) {
            const bool on = field.input.text() == "true";
            const ui::Rect toggle{control.x, control.y + 4, 44, 24};
            renderer.draw_box(toggle, {.fill = on && editable() ? palette.primary
                                                                : palette.surface_container_highest,
                                       .radius = ui::Corners::all(12),
                                       .border_width = focused ? 2.0F : 1.0F,
                                       .border = focused ? palette.on_surface : palette.outline});
            renderer.draw_box(
                {toggle.x + (on ? 24.0F : 4.0F), toggle.y + 4, 16, 16},
                {.fill = on && editable() ? palette.on_primary : palette.on_surface_variant,
                 .radius = ui::Corners::all(8)});
            renderer.draw_text(choice_label(field, field.input.text()), control.x + 54,
                               control.y + 8, 13, palette.on_surface);
        } else if (!field.choices.empty()) {
            draw_dialog_button(renderer, control, palette, "", focused);
            const auto label = choice_label(field, field.input.text());
            const float width = std::max(1.0F, control.width - 44);
            const float height = SelectableText::height(renderer, label, width, 13, 18);
            SelectableText text;
            text.begin_frame(control);
            text.draw(renderer, "choice", label,
                      {control.x + 10, control.y + (control.height - height) / 2, width, height},
                      13, palette.on_surface, {}, 18);
            icons.draw(renderer, "chevron-down", control.right() - 24,
                       control.y + (control.height - 16) / 2, 16, palette.on_surface_variant);
        } else {
            if (field.number && editable()) {
                field.bounds.width = std::max(1.0F, control.width - 76);
                button("minus" + std::to_string(i), "−", {control.right() - 70, control.y, 32, 32},
                       DialogButton::normal, true);
                button("plus" + std::to_string(i), "+", {control.right() - 32, control.y, 32, 32},
                       DialogButton::normal, true);
            }
            field.input.draw(renderer, field.bounds, 13,
                             {.background = palette.surface_container_highest,
                              .border = !field.error.empty() ? palette.error
                                        : focused            ? palette.primary
                                                             : palette.outline_variant,
                              .text = palette.on_surface,
                              .selection = theme::readable_foreground(palette.secondary_container,
                                                                      palette.on_surface),
                              .caret = focused && editable() ? palette.on_surface
                                                             : palette.on_surface.with_alpha(0)});
        }
        if ((field.section < 2 || field.section == kEffects) && editable())
            button("reset" + std::to_string(i), "Reset to default",
                   {row.x, y + field.height - (field.error.empty() ? 34.0F : 74.0F),
                    std::min(std::max(150.0F,
                                      text_width(renderer, i18n::tr("Reset to default"), 13) + 24),
                             row.width),
                    24},
                   DialogButton::normal, true);
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
    if (!embedded && !any && valid_ && search_.text().empty())
        renderer.draw_text(i18n::tr("No custom actions yet. Choose Add action to create one."),
                           list_.x + 16, list_.y + 20, 13, palette.on_surface_variant,
                           std::max(1.0F, list_.width - 32));
    if (!embedded)
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
    if (!write_error_.empty() && editable()) {
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

void SettingsDialog::preview_effect(core::MotionEvent event) {
    select_effect_slot(static_cast<std::size_t>(event));
}

void SettingsDialog::select_effect_slot(std::size_t slot) {
    if (!open_ || !valid_ || slot >= kEffectSlots.size())
        return;
    commit_focus();
    search_.set_text("");
    effect_slot_ = slot;
    if (section_ != kEffects)
        change_section(kEffects);
    scroller_.jump_to(0);
    scrollbar_ = ui::Scrollbar{};
    for (auto& field : fields_)
        field.bounds = {};
    focus_ = "effect_slot";
    refresh_preview();
    restart_preview();
}

bool SettingsDialog::field_visible(const Field& field) const {
    if (field.section != section_)
        return false;
    if (section_ != kEffects)
        return true;
    const bool event = effect_slot_ < core::kMotionEvents.size();
    if (field.table == "animations.events" || field.table == "animations.durations")
        return event && field.key == core::kMotionEvents[effect_slot_];
    if (field.table == "animations.ambient") {
        if (field.key == "background" || field.key == "shader")
            return effect_slot_ == kBackgroundSlot;
        if (field.key == "selection_glow")
            return effect_slot_ == kSelectionSlot;
        return !event;
    }
    if (field.key == "hologram_spread")
        return event && preview_config_.events[effect_slot_] == core::MotionEffect::hologram;
    if (field.key == "portal_rotation")
        return event && preview_config_.events[effect_slot_] == core::MotionEffect::portal;
    return true;
}

void SettingsDialog::refresh_preview() {
    if (!valid_)
        return;
    std::vector<core::ConfigEdit> edits;
    for (const auto& field : fields_)
        if (field.section == kEffects && field.input.text() != field.original)
            edits.push_back(
                {field.table, field.key,
                 field.raw ? field.input.text() : core::quote_config_string(field.input.text())});
    const auto changed = core::edit_config_text(staged_, edits);
    preview_invalid_ = !changed.error.empty();
    if (preview_invalid_)
        return;
    const auto parsed = core::parse_config(changed.text, path_.string());
    preview_invalid_ = !parsed.problems.empty();
    if (preview_invalid_)
        return;
    if (preview_config_ != parsed.config.animations) {
        preview_config_ = parsed.config.animations;
        preview_motion_.configure(preview_config_);
        if (section_ == kEffects)
            restart_preview();
    }
}

void SettingsDialog::restart_preview() {
    if (!open_ || !valid_)
        return;
    preview_drawn_ = true;
    preview_started_ = ui::Motion::Clock::now();
    preview_motion_.configure(preview_config_);
    preview_motion_.cancel_all();
    if (effect_slot_ < core::kMotionEvents.size())
        preview_motion_.start(ui::MotionChannel::operation,
                              static_cast<core::MotionEvent>(effect_slot_), preview_started_);
    redraw_();
}

std::optional<std::chrono::milliseconds> SettingsDialog::ambient_interval() const {
    const auto& ambient = preview_config_.ambient;
    const bool selected_enabled = effect_slot_ == kBackgroundSlot
                                      ? ambient.background
                                      : effect_slot_ == kSelectionSlot && ambient.selection_glow;
    if (!previewing() || !selected_enabled || !preview_motion_.ambient_enabled() ||
        ambient.speed <= 0)
        return std::nullopt;
    return std::chrono::milliseconds{(1000 + ambient.fps - 1) / ambient.fps};
}

void SettingsDialog::draw_preview(ui::Renderer& renderer, const ui::Rect& bounds,
                                  const theme::Palette& palette, ui::IconSet& icons) {
    preview_drawn_ = true;
    const auto now = ui::Motion::Clock::now();
    preview_motion_.advance(now);
    renderer.push_clip(bounds);
    renderer.fill_rect(bounds, palette.surface);
    const ui::Rect scene{bounds.x + 8, bounds.y + 8, bounds.width - 16, bounds.height - 36};
    const ui::Rect row{scene.x + 4, scene.y + 4, scene.width - 8, std::min(28.0F, scene.height)};
    const float seconds = std::chrono::duration<float>(now - preview_started_).count() *
                          preview_config_.ambient.speed;
    if (effect_slot_ == kBackgroundSlot && preview_motion_.ambient_enabled() &&
        preview_config_.ambient.background)
        renderer.draw_ambient(scene, preview_config_.ambient, seconds, palette.primary, false,
                              true);
    auto frame = preview_frame();
    const bool operation = effect_slot_ >= static_cast<std::size_t>(core::MotionEvent::file_copy) &&
                           effect_slot_ <= static_cast<std::size_t>(core::MotionEvent::file_trash);
    const bool overlay =
        effect_slot_ == static_cast<std::size_t>(core::MotionEvent::quick_look) ||
        effect_slot_ == static_cast<std::size_t>(core::MotionEvent::palette_open) ||
        effect_slot_ == static_cast<std::size_t>(core::MotionEvent::search_results);
    const bool native =
        frame && core::native_motion_feedback(frame->effect) && !operation && !overlay;
    const ui::Rect panel{scene.x + scene.width * 0.12F, scene.y + 2, scene.width * 0.76F,
                         std::max(1.0F, scene.height - 4)};
    const auto draw_rows = [&](const ui::Rect& area) {
        for (int i = 0; i < 3; ++i) {
            const float y = area.y + 4 + static_cast<float>(i) * 24;
            if (y + 18 > area.bottom())
                break;
            icons.draw(renderer, i == 0 ? "folder" : "file", area.x + 8, y, 18,
                       i == 0 ? palette.primary : palette.on_surface_variant);
            renderer.fill_rect(
                {area.x + 36, y + 5,
                 std::max(1.0F, area.width * (0.55F - static_cast<float>(i) * 0.1F)), 5},
                palette.on_surface_variant.with_alpha(0.35F));
        }
    };
    if (overlay)
        draw_rows(scene);
    const auto target = overlay ? panel : scene;
    const bool captured =
        frame && !operation && !native && renderer.begin_motion(target, preview_config_.quality);
    if (operation) {
        icons.draw(renderer, "folder", scene.right() - 36, scene.y + 8, 28,
                   palette.on_surface_variant);
        ui::Rect icon{scene.x + 8, scene.y + 8, 28, 28};
        if (frame) {
            frame->from = icon;
            frame->to = {scene.right() - 36, scene.y + 8, 28, 28};
            icon = frame->geometry();
        }
        const bool sprite = frame && renderer.begin_motion(icon.inset(-6), preview_config_.quality);
        icons.draw(renderer, "file", icon.x, icon.y, icon.width, palette.primary);
        if (sprite)
            renderer.end_motion(*frame, icon.x + 14, icon.y + 14, palette.primary);
    } else {
        if (overlay)
            renderer.draw_box(
                panel, {.fill = palette.surface_container_highest, .radius = ui::Corners::all(6)});
        draw_rows(overlay ? panel.inset(8) : scene);
    }
    if (captured)
        renderer.end_motion(*frame, row.x + 16, row.y + 14, palette.primary);
    if (native) {
        const float alpha = (1.0F - frame->progress) * 0.45F;
        if (frame->effect == core::MotionEffect::focus_glow)
            renderer.draw_shadow(row, ui::Corners::all(8), 8, palette.primary.with_alpha(alpha));
        else {
            renderer.draw_box(row, {.fill = palette.primary.with_alpha(0),
                                    .radius = ui::Corners::all(5),
                                    .border_width = 2,
                                    .border = palette.primary.with_alpha(alpha)});
            const float distance = 2 * (row.width + row.height) * frame->progress;
            ui::Rect tip{row.x, row.y, 4, 4};
            if (distance < row.width)
                tip.x += distance;
            else if (distance < row.width + row.height) {
                tip.x = row.right() - 4;
                tip.y += distance - row.width;
            } else if (distance < 2 * row.width + row.height) {
                tip.x = row.right() - (distance - row.width - row.height);
                tip.y = row.bottom() - 4;
            } else
                tip.y = row.bottom() - (distance - 2 * row.width - row.height);
            renderer.fill_rect(tip, palette.primary.with_alpha(1 - frame->progress));
        }
    }
    if (effect_slot_ == kSelectionSlot && preview_motion_.ambient_enabled() &&
        preview_config_.ambient.selection_glow)
        renderer.draw_ambient(row, preview_config_.ambient, seconds, palette.primary, true, true);
    if (!preview_motion_.active())
        renderer.release_motion_textures();
    const auto error =
        effect_slot_ == kBackgroundSlot ? renderer.ambient_error(true) : std::string_view{};
    const bool slot_disabled =
        effect_slot_ < core::kMotionEvents.size()
            ? preview_config_.events[effect_slot_] == core::MotionEffect::none ||
                  preview_config_.durations_ms[effect_slot_] == 0
        : effect_slot_ == kBackgroundSlot ? !preview_config_.ambient.background
                                          : !preview_config_.ambient.selection_glow;
    const auto caption =
        preview_invalid_ ? i18n::tr("Changes need attention")
        : !error.empty() ? i18n::tr("Shader error — using the last working effect; see log")
        : preview_motion_.reduced() ? i18n::tr("Reduced motion is active")
        : !preview_config_.enabled || preview_config_.duration_scale == 0 || slot_disabled
            ? i18n::tr("Motion effects are disabled")
            : i18n::tr("Sample only — no files are changed");
    renderer.draw_text(
        caption, bounds.x + 8, bounds.bottom() - 24, 11,
        error.empty() && !preview_invalid_ ? palette.on_surface_variant : palette.error,
        bounds.width - 16);
    renderer.pop_clip();
}

}  // namespace rummage::views
