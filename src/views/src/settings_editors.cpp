#include "rummage/views/settings_editors.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "rummage/core/i18n.hpp"
#include "rummage/views/dialog_chrome.hpp"
#include "rummage/views/shortcuts.hpp"

namespace rummage::views {
namespace {
std::string lower(std::string_view value) {
    std::string result{value};
    for (char& c : result)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}

std::string array_text(const std::vector<std::string>& values) {
    std::string result{"["};
    for (const auto& value : values) {
        if (result.size() > 1)
            result += ", ";
        result += core::quote_config_string(value);
    }
    return result + "]";
}

std::string_view selection_name(core::Action::Selection value) {
    switch (value) {
        case core::Action::Selection::one:
            return "one";
        case core::Action::Selection::many:
            return "many";
        case core::Action::Selection::background:
            return "background";
        case core::Action::Selection::any:
            return "any";
    }
    return "one";
}

std::string_view destination_name(core::Action::Destination value) {
    switch (value) {
        case core::Action::Destination::none:
            return "none";
        case core::Action::Destination::file:
            return "file";
        case core::Action::Destination::directory:
            return "directory";
    }
    return "none";
}

bool same_form(const core::Action& a, const core::Action& b) {
    return a.id == b.id && a.label == b.label && a.argv == b.argv && a.enabled == b.enabled &&
           a.selection == b.selection && a.destination == b.destination;
}
}  // namespace

SettingsEditors::SettingsEditors(Commit commit, std::function<void()> redraw)
    : commit_{std::move(commit)}, redraw_{std::move(redraw)} {}

void SettingsEditors::reset(const core::Config& config, bool editable) {
    config_ = config;
    shortcut_problems_ = configure_shortcuts(config_.keybindings, config_.actions, false);
    editable_ = editable;
    action_.reset();
    expanded_groups_.clear();
    collapsed_search_groups_.clear();
    shown_query_.clear();
    fresh_ = false;
    target_.clear();
    recording_.clear();
    pending_chord_.clear();
    error_.clear();
    search_.set_text({});
    folder_.set_text({});
    scroll_.jump_to(0);
    clear_controls();
}

void SettingsEditors::sync(const core::Config& config, bool editable) {
    // sync also runs while drawing. Shortcut validation only depends on bindings
    // and the action IDs/labels; do not repeat it for every frame or appearance edit.
    const bool changed = config_.keybindings != config.keybindings ||
                         !std::ranges::equal(config_.actions, config.actions,
                                             [](const core::Action& a, const core::Action& b) {
                                                 return a.id == b.id && a.label == b.label;
                                             });
    config_ = config;
    if (changed)
        shortcut_problems_ = configure_shortcuts(config_.keybindings, config_.actions, false);
    editable_ = editable;
    if (!editable_)
        recording_.clear();
}

void SettingsEditors::clear_controls() {
    controls_.clear();
    focus_ = -1;
}

void SettingsEditors::set_page(Page page) {
    if (page == page_)
        return;
    commit();
    page_ = page;
    recording_.clear();
    search_.set_text({});
    scroll_.jump_to(0);
    clear_controls();
}

std::vector<std::pair<std::string, std::string>> SettingsEditors::commands(
    std::string_view query) const {
    auto result = binding_settings();
    for (const auto& action : core::action_registry(config_.actions)) {
        if (action.id != "copy_name")
            result.emplace_back("action:" + action.id, action.translate_label
                                                           ? std::string{i18n::tr(action.label)}
                                                           : action.label);
    }
    for (const auto& [name, chord] : config_.keybindings) {
        if (name.starts_with("go:"))
            result.emplace_back(name, i18n::trf("Go to {}", name.substr(3)));
    }
    const auto needle = lower(query);
    std::erase_if(result, [&](const auto& command) {
        return lower(command.first).find(needle) == std::string::npos &&
               lower(command.second).find(needle) == std::string::npos;
    });
    return result;
}

std::optional<std::string> SettingsEditors::chord_for(const ui::KeyEvent& event) {
    if (event.repeat || event.keysym == XKB_KEY_NoSymbol ||
        (event.keysym >= XKB_KEY_Shift_L && event.keysym <= XKB_KEY_Hyper_R) ||
        event.keysym == XKB_KEY_ISO_Level3_Shift || event.keysym == XKB_KEY_Mode_switch)
        return std::nullopt;
    auto symbol = event.keysym == XKB_KEY_ISO_Left_Tab ? XKB_KEY_Tab : event.keysym;
    symbol = xkb_keysym_to_lower(symbol);
    std::array<char, 128> name{};
    if (xkb_keysym_get_name(symbol, name.data(), name.size()) <= 0)
        return std::nullopt;
    std::string result;
    if (event.modifiers.ctrl)
        result += "Ctrl+";
    if (event.modifiers.alt)
        result += "Alt+";
    if (event.modifiers.shift)
        result += "Shift+";
    if (event.modifiers.super)
        result += "Super+";
    return result + name.data();
}

bool SettingsEditors::assign_shortcut(std::string_view target, std::optional<std::string> chord) {
    if (!editable_)
        return false;
    auto bindings = config_.keybindings;
    if (chord)
        bindings[std::string{target}] = *chord;
    else
        bindings.erase(std::string{target});
    const auto problems = configure_shortcuts(bindings, config_.actions, false);
    if (!problems.empty()) {
        error_ = problems.front();
        redraw_();
        return false;
    }
    error_ = commit_({{"keybindings", std::string{target},
                       core::quote_config_string(chord.value_or("")), !chord}},
                     {});
    if (error_.empty()) {
        config_.keybindings = std::move(bindings);
        shortcut_problems_.clear();
        pending_chord_.clear();
    }
    redraw_();
    return error_.empty();
}

void SettingsEditors::focus_shortcut(std::string_view target) {
    set_page(Page::shortcuts);
    target_ = target;
    scroll_.jump_to(0);
    clear_controls();
    redraw_();
}

void SettingsEditors::dismiss() {
    recording_.clear();
    redraw_();
}

void SettingsEditors::record_shortcut(std::string target) {
    if (!editable_)
        return;
    target_ = std::move(target);
    recording_ = target_;
    pending_chord_.clear();
    error_.clear();
    redraw_();
}

void SettingsEditors::load_action(core::Action action, bool fresh) {
    action_ = std::move(action);
    fresh_ = fresh;
    id_.set_text(action_->id);
    label_.set_text(action_->label);
    program_.set_text(action_->argv.empty() ? "" : action_->argv.front());
    arguments_.clear();
    for (std::size_t i = 1; i < action_->argv.size(); ++i)
        arguments_.emplace_back(action_->argv[i]);
    error_.clear();
    scroll_.jump_to(0);
    clear_controls();
}

bool SettingsEditors::select_action(std::string_view id) {
    if (has_drafts() && !commit())
        return false;
    const auto found = std::ranges::find(config_.actions, id, &core::Action::id);
    if (found == config_.actions.end())
        return false;
    load_action(*found, false);
    return true;
}

void SettingsEditors::new_action() {
    if (!editable_ || (has_drafts() && !commit()))
        return;
    load_action({}, true);
    redraw_();
}

core::Action SettingsEditors::form_action() const {
    auto result = *action_;
    result.id = fresh_ ? id_.text() : action_->id;
    result.label = label_.text();
    if (result.kind == core::Action::Kind::external) {
        result.argv = {program_.text()};
        for (const auto& argument : arguments_)
            result.argv.push_back(argument.text());
    }
    return result;
}

bool SettingsEditors::update_action(const core::Action& action) {
    if (!editable_)
        return false;
    if (action_ && !fresh_ && action.id != action_->id) {
        error_ = i18n::tr("Existing action IDs cannot be changed");
        return false;
    }
    if (const auto problem = core::action_problem(action)) {
        error_ = *problem;
        return false;
    }
    const auto found = std::ranges::find(config_.actions, action.id, &core::Action::id);
    if (fresh_ && found != config_.actions.end()) {
        error_ = i18n::tr("An action with this ID already exists");
        return false;
    }
    std::vector<core::ConfigEdit> edits;
    std::string append;
    if (found == config_.actions.end()) {
        append = "\n[[actions]]\nid = " + core::quote_config_string(action.id) +
                 "\nlabel = " + core::quote_config_string(action.label) +
                 "\nargv = " + array_text(action.argv) +
                 "\nselection = " + core::quote_config_string(selection_name(action.selection)) +
                 "\nenabled = " + (action.enabled ? "true" : "false") + "\n";
        if (action.destination != core::Action::Destination::none)
            append +=
                "destination = " + core::quote_config_string(destination_name(action.destination)) +
                "\n";
    } else {
        const auto table =
            std::format("actions[{}]", std::distance(config_.actions.begin(), found));
        if (action.label != found->label)
            edits.push_back({table, "label", core::quote_config_string(action.label)});
        if (action.argv != found->argv)
            edits.push_back({table, "argv", array_text(action.argv)});
        if (action.enabled != found->enabled)
            edits.push_back({table, "enabled", action.enabled ? "true" : "false"});
        if (action.selection != found->selection)
            edits.push_back(
                {table, "selection", core::quote_config_string(selection_name(action.selection))});
        if (action.destination != found->destination)
            edits.push_back({table, "destination",
                             core::quote_config_string(destination_name(action.destination))});
    }
    error_ = edits.empty() && append.empty() ? "" : commit_(edits, append);
    if (!error_.empty())
        return false;
    const auto accepted = std::ranges::find(config_.actions, action.id, &core::Action::id);
    if (accepted == config_.actions.end())
        config_.actions.push_back(action);
    else
        *accepted = action;
    shortcut_problems_ = configure_shortcuts(config_.keybindings, config_.actions, false);
    action_ = action;
    fresh_ = false;
    if (id_.text() != action.id)
        id_.set_text(action.id);
    if (label_.text() != action.label)
        label_.set_text(action.label);
    if (action.kind == core::Action::Kind::external) {
        if (program_.text() != action.argv.front())
            program_.set_text(action.argv.front());
        if (arguments_.size() + 1 != action.argv.size()) {
            arguments_.clear();
            for (std::size_t i = 1; i < action.argv.size(); ++i)
                arguments_.emplace_back(action.argv[i]);
            clear_controls();
        } else {
            for (std::size_t i = 0; i < arguments_.size(); ++i)
                if (arguments_[i].text() != action.argv[i + 1])
                    arguments_[i].set_text(action.argv[i + 1]);
        }
    }
    return true;
}

bool SettingsEditors::commit_form() {
    if (!action_ || (!fresh_ && !has_drafts()))
        return true;
    return update_action(form_action());
}

bool SettingsEditors::commit() {
    // Rejected recordings remain visible until explicitly discarded or replaced.
    if (!pending_chord_.empty())
        return false;
    return commit_form();
}

bool SettingsEditors::has_drafts() const {
    if (!pending_chord_.empty())
        return true;
    if (!action_)
        return false;
    const auto found = std::ranges::find(config_.actions, action_->id, &core::Action::id);
    return fresh_ || found == config_.actions.end() || !same_form(form_action(), *found);
}

void SettingsEditors::discard() {
    recording_.clear();
    pending_chord_.clear();
    error_.clear();
    if (action_) {
        if (fresh_)
            action_.reset();
        else {
            const auto found = std::ranges::find(config_.actions, action_->id, &core::Action::id);
            if (found != config_.actions.end())
                load_action(*found, false);
            else
                action_.reset();
        }
    }
    clear_controls();
    redraw_();
}

void SettingsEditors::changed() {
    if (action_ && page_ == Page::actions) {
        const auto problem = core::action_problem(form_action());
        error_ = problem.value_or("");
    }
    redraw_();
}

void SettingsEditors::activate(Control control) {
    if (control.editable && !editable_)
        return;
    switch (control.operation) {
        case Operation::field:
            break;
        case Operation::toggle_group:
            if (search_.text().empty()) {
                expanded_groups_[control.value] = !expanded_groups_[control.value];
            } else if (!collapsed_search_groups_.erase(control.value)) {
                collapsed_search_groups_.insert(control.value);
            }
            break;
        case Operation::select_command:
            target_ = control.value;
            recording_.clear();
            pending_chord_.clear();
            error_.clear();
            scroll_.jump_to(0);
            clear_controls();
            break;
        case Operation::record:
            record_shortcut(control.value);
            break;
        case Operation::reset:
            assign_shortcut(control.value, std::nullopt);
            break;
        case Operation::disable:
            assign_shortcut(control.value, "");
            break;
        case Operation::select_action:
            select_action(control.value);
            break;
        case Operation::add_action:
            new_action();
            break;
        case Operation::back:
            if (commit()) {
                recording_.clear();
                action_.reset();
                target_.clear();
                scroll_.jump_to(0);
                clear_controls();
            }
            break;
        case Operation::add_argument:
            arguments_.emplace_back();
            clear_controls();
            break;
        case Operation::remove_argument:
            if (control.index < arguments_.size()) {
                arguments_.erase(arguments_.begin() + static_cast<std::ptrdiff_t>(control.index));
                clear_controls();
                commit_form();
            }
            break;
        case Operation::selection:
            if (action_) {
                action_->selection = static_cast<core::Action::Selection>(
                    (static_cast<int>(action_->selection) + 1) % 4);
                commit_form();
            }
            break;
        case Operation::destination:
            if (action_) {
                action_->destination = static_cast<core::Action::Destination>(
                    (static_cast<int>(action_->destination) + 1) % 3);
                commit_form();
            }
            break;
        case Operation::enabled:
            if (action_) {
                action_->enabled = !action_->enabled;
                commit_form();
            }
            break;
        case Operation::add_folder:
            if (core::expand_folder_path(folder_.text()).empty())
                error_ = i18n::tr("Enter an absolute folder path or a path starting with ~/");
            else
                record_shortcut("go:" + folder_.text());
            break;
        case Operation::discard:
            discard();
            break;
    }
    redraw_();
}

void SettingsEditors::enter_focus(bool reverse) {
    focus_ = -1;
    for (std::size_t n = 0; n < controls_.size(); ++n) {
        const auto i = reverse ? controls_.size() - 1 - n : n;
        if (editable_ || !controls_[i].editable || controls_[i].field) {
            focus_ = static_cast<int>(i);
            break;
        }
    }
    reveal_focus();
    redraw_();
}

void SettingsEditors::blur() {
    focus_ = -1;
    redraw_();
}

void SettingsEditors::reveal_focus() {
    if (focus_ < 0 || static_cast<std::size_t>(focus_) >= controls_.size())
        return;
    const auto& rect = controls_[static_cast<std::size_t>(focus_)].bounds;
    if (rect.y < body_.y)
        scroll_.jump_to(scroll_.offset() + rect.y - body_.y);
    else if (rect.bottom() > body_.bottom())
        scroll_.jump_to(scroll_.offset() + rect.bottom() - body_.bottom());
}

bool SettingsEditors::key(const ui::KeyEvent& event) {
    if (!recording_.empty()) {
        if (event.keysym == XKB_KEY_Escape) {
            recording_.clear();
            redraw_();
        } else if (const auto chord = chord_for(event)) {
            const auto target = recording_;
            recording_.clear();
            pending_chord_ = *chord;
            assign_shortcut(target, *chord);
        }
        return true;
    }
    if (event.keysym == XKB_KEY_Tab || event.keysym == XKB_KEY_ISO_Left_Tab) {
        commit_form();
        if (controls_.empty())
            return false;
        const int count = static_cast<int>(controls_.size());
        const int step = event.modifiers.shift ? -1 : 1;
        do {
            focus_ += step;
            if (focus_ < 0 || focus_ >= count) {
                focus_ = -1;
                return false;
            }
        } while (!editable_ && controls_[static_cast<std::size_t>(focus_)].editable &&
                 !controls_[static_cast<std::size_t>(focus_)].field);
        reveal_focus();
        redraw_();
        return true;
    }
    if (event.keysym == XKB_KEY_Escape)
        return false;
    if (focus_ < 0 || static_cast<std::size_t>(focus_) >= controls_.size())
        return false;
    auto control = controls_[static_cast<std::size_t>(focus_)];
    if (control.editable && !editable_) {
        const bool selects =
            event.keysym == XKB_KEY_Left || event.keysym == XKB_KEY_Right ||
            event.keysym == XKB_KEY_Home || event.keysym == XKB_KEY_End ||
            (event.modifiers.ctrl && (event.keysym == XKB_KEY_a || event.keysym == XKB_KEY_A));
        if (control.field && selects) {
            control.field->key(event);
            redraw_();
            return true;
        }
        return false;
    }
    if (control.operation == Operation::toggle_group &&
        (event.keysym == XKB_KEY_Left || event.keysym == XKB_KEY_Right)) {
        const bool expanded = event.keysym == XKB_KEY_Right;
        if (search_.text().empty())
            expanded_groups_[control.value] = expanded;
        else if (expanded)
            collapsed_search_groups_.erase(control.value);
        else
            collapsed_search_groups_.insert(control.value);
        redraw_();
        return true;
    }
    if (event.keysym == XKB_KEY_Return || event.keysym == XKB_KEY_KP_Enter) {
        if (control.field)
            commit_form();
        else
            activate(std::move(control));
        redraw_();
        return true;
    }
    if (!control.field && event.keysym == XKB_KEY_space) {
        activate(std::move(control));
        return true;
    }
    if (control.field && control.field->key(event)) {
        if (control.field == &search_)
            scroll_.jump_to(0);
        changed();
        return true;
    }
    return false;
}

bool SettingsEditors::pointer(const ui::PointerEvent& event) {
    if (scrollbar_.pointer(event, scroll_)) {
        redraw_();
        return true;
    }
    if (!body_.contains(event.x, event.y))
        return false;
    if (event.kind == ui::PointerEvent::Kind::scroll) {
        scroll_.jump_to(scroll_.offset() +
                        (event.wheel_y != 0 ? event.wheel_y * 48 : event.scroll_y));
        redraw_();
        return true;
    }
    if (focus_ >= 0 && static_cast<std::size_t>(focus_) < controls_.size()) {
        auto& current = controls_[static_cast<std::size_t>(focus_)];
        if (current.field && current.field->pointer(event)) {
            redraw_();
            return true;
        }
    }
    if (event.kind != ui::PointerEvent::Kind::press || event.button != ui::PointerButton::left)
        return true;
    for (std::size_t i = 0; i < controls_.size(); ++i) {
        const auto control = controls_[i];
        if (!control.bounds.contains(event.x, event.y) ||
            (control.editable && !editable_ && !control.field))
            continue;
        commit_form();
        focus_ = static_cast<int>(i);
        if (control.field)
            control.field->pointer(event);
        else
            activate(control);
        redraw_();
        return true;
    }
    return true;
}

ui::CursorShape SettingsEditors::cursor_at(float x, float y) const {
    if (!body_.contains(x, y))
        return ui::CursorShape::standard;
    for (const auto& control : controls_) {
        if (control.bounds.contains(x, y) && (!control.editable || editable_ || control.field))
            return control.field ? ui::CursorShape::text : ui::CursorShape::pointer;
    }
    return ui::CursorShape::standard;
}

void SettingsEditors::paste(std::string_view text) {
    if (recording_.empty() && focus_ >= 0 && static_cast<std::size_t>(focus_) < controls_.size()) {
        auto& control = controls_[static_cast<std::size_t>(focus_)];
        if (control.field && (!control.editable || editable_)) {
            control.field->insert(text);
            changed();
        }
    }
}

std::string SettingsEditors::copy_selection(bool cut) {
    if (focus_ >= 0 && static_cast<std::size_t>(focus_) < controls_.size()) {
        const auto& control = controls_[static_cast<std::size_t>(focus_)];
        if (auto* field = control.field) {
            const auto [first, last] = field->selection();
            auto result = field->text().substr(first, last - first);
            if (cut && (!control.editable || editable_)) {
                field->insert("");
                changed();
            }
            return result;
        }
    }
    return {};
}

void SettingsEditors::draw(ui::Renderer& renderer, const ui::Rect& body,
                           const theme::Palette& palette) {
    const auto previous_body = body_;
    body_ = body;
    if (shown_query_ != search_.text()) {
        shown_query_ = search_.text();
        collapsed_search_groups_.clear();
        scroll_.jump_to(0);
    }
    std::optional<Control> focused_control;
    if (focus_ >= 0 && static_cast<std::size_t>(focus_) < controls_.size())
        focused_control = controls_[static_cast<std::size_t>(focus_)];
    controls_.clear();
    scrollbar_.layout(body);
    renderer.push_clip(body);
    const float x = body.x + 12;
    const float width = std::max(1.0F, body.width - 36);
    const float drawn_offset = scroll_.offset();
    float y = body.y + 8 - drawn_offset;
    const auto accent = theme::readable_foreground(palette.primary, palette.surface_container_high);
    const auto matches_focus = [&](const Control& control) {
        return focused_control && control.operation == focused_control->operation &&
               control.field == focused_control->field && control.value == focused_control->value &&
               control.index == focused_control->index;
    };
    const auto text = [&](std::string_view value, bool dim = false) {
        while (!value.empty()) {
            std::size_t length = value.size();
            while (renderer.fonts().layout(value.substr(0, length), 12, renderer.scale()).width /
                       renderer.scale() >
                   width) {
                const auto space = value.rfind(' ', length - 1);
                if (space == std::string_view::npos || space == 0)
                    break;
                length = space;
            }
            renderer.draw_text(value.substr(0, length), x, y, 12,
                               dim ? palette.on_surface_variant : palette.on_surface, width);
            y += 20;
            value.remove_prefix(length);
            while (value.starts_with(' '))
                value.remove_prefix(1);
        }
    };
    const auto button_at = [&](std::string_view label, ui::Rect rect, Operation operation,
                               std::string value = "", bool mutates = true, std::size_t index = 0) {
        Control control{rect, operation, nullptr, std::move(value), index, mutates};
        draw_dialog_button(renderer, rect, palette, label, matches_focus(control));
        if (mutates && !editable_)
            renderer.fill_rect(rect, palette.surface.with_alpha(0.45F));
        controls_.push_back(std::move(control));
    };
    const auto button = [&](std::string_view label, Operation operation, std::string value = "",
                            bool mutates = true, std::size_t index = 0) {
        const float measured =
            renderer.fonts().layout(label, 13, renderer.scale()).width / renderer.scale() + 28;
        button_at(label, {x, y, std::min(width, std::max(100.0F, measured)), 32}, operation,
                  std::move(value), mutates, index);
        y += 40;
    };
    const auto group = [&](std::string id, std::string_view label, bool initially_open) {
        auto [position, inserted] = expanded_groups_.try_emplace(id, initially_open);
        static_cast<void>(inserted);
        const bool expanded =
            search_.text().empty() ? position->second : !collapsed_search_groups_.contains(id);
        y += 6;
        std::vector<std::string_view> lines;
        auto remaining = label;
        while (!remaining.empty()) {
            auto length = remaining.size();
            while (
                renderer.fonts().layout(remaining.substr(0, length), 13, renderer.scale()).width /
                    renderer.scale() >
                std::max(1.0F, width - 32)) {
                const auto space = remaining.rfind(' ', length - 1);
                if (space == std::string_view::npos || space == 0)
                    break;
                length = space;
            }
            lines.push_back(remaining.substr(0, length));
            remaining.remove_prefix(length);
            while (remaining.starts_with(' '))
                remaining.remove_prefix(1);
        }
        const float height = std::max(36.0F, 18.0F + 18.0F * static_cast<float>(lines.size()));
        const ui::Rect rect{x, y, width, height};
        Control control{rect, Operation::toggle_group, nullptr, id, 0, false};
        renderer.draw_box(rect, {.fill = palette.surface_container_high,
                                 .radius = {},
                                 .border_width = matches_focus(control) ? 1.0F : 0.0F,
                                 .border = palette.primary});
        renderer.fill_rect({x, rect.bottom() - 1, width, 1}, palette.outline_variant);
        renderer.draw_text(expanded ? "▾" : "▸", x + 4, y + 9, 13, accent);
        float line_y = y + 9;
        for (const auto line : lines) {
            renderer.draw_text(line, x + 24, line_y, 13, palette.on_surface,
                               std::max(1.0F, width - 32));
            line_y += 18;
        }
        core::AccessibleNode node;
        node.id = "settings-group:" + id;
        node.name = label;
        node.role = core::AccessibleRole::button;
        node.focused = matches_focus(control);
        node.checkable = true;
        node.checked = expanded;
        renderer.accessible(std::move(node), rect);
        controls_.push_back(std::move(control));
        y += height + 6;
        return expanded;
    };
    const auto field = [&](std::string_view label, ui::TextField& input, bool mutates = true,
                           std::optional<std::size_t> argument = {}) {
        input.set_accessible_name(std::string{label});
        const bool aligned = width >= 540 && &input != &search_;
        const float label_width = aligned ? std::min(180.0F, width * 0.28F) : 0;
        if (aligned)
            renderer.draw_text(label, x, y + 8, 13, palette.on_surface, label_width - 12);
        else
            text(label);
        const float remove_width = argument ? 32.0F : 0;
        const ui::Rect rect{x + label_width, y,
                            std::max(1.0F, width - label_width - remove_width - (argument ? 8 : 0)),
                            32};
        Control control{rect, Operation::field, &input, {}, 0, mutates};
        const bool focused = matches_focus(control);
        input.draw(renderer, rect, 13,
                   {palette.surface_container_low, focused ? palette.on_surface : palette.outline,
                    palette.on_surface,
                    theme::readable_foreground(palette.secondary_container, palette.on_surface),
                    focused ? palette.on_surface : palette.on_surface.with_alpha(0)});
        if (mutates && !editable_)
            renderer.fill_rect(rect, palette.surface.with_alpha(0.45F));
        controls_.push_back(std::move(control));
        if (argument) {
            const ui::Rect remove{x + width - remove_width, y, remove_width, 32};
            // The compact glyph has the full action name in the semantic tree.
            Control remove_control{remove, Operation::remove_argument, nullptr, {}, *argument,
                                   true};
            core::AccessibleNode node;
            node.id = "remove-argument:" + std::to_string(*argument);
            node.name = i18n::tr("Remove argument");
            node.role = core::AccessibleRole::button;
            node.focused = matches_focus(remove_control);
            node.enabled = editable_;
            renderer.accessible(std::move(node), remove);
            renderer.draw_box(remove, {.fill = palette.surface_container_highest,
                                       .radius = ui::Corners::all(4),
                                       .border_width = matches_focus(remove_control) ? 1.0F : 0.0F,
                                       .border = palette.primary});
            renderer.draw_text("−", remove.x + 10, remove.y + 5, 16, palette.on_surface);
            if (!editable_)
                renderer.fill_rect(remove, palette.surface.with_alpha(0.45F));
            controls_.push_back(std::move(remove_control));
        }
        y += 42;
    };
    const auto chord = [&](const std::string& target) {
        if (const auto found = config_.keybindings.find(target); found != config_.keybindings.end())
            return found->second.empty() ? std::string{i18n::tr("Disabled")}
                                         : i18n::keys(found->second);
        if (const auto entry = default_shortcut(target); entry && !entry->keys.empty())
            return i18n::keys(entry->keys);
        return std::string{"—"};
    };
    const auto row = [&](std::string_view label, std::string_view value, std::string_view detail,
                         Operation operation, const std::string& target) {
        const bool narrow = width < 420;
        const float row_height = narrow || !detail.empty() ? 50.0F : 32.0F;
        const ui::Rect rect{x, y, width, row_height};
        Control control{rect, operation, nullptr, target, 0, false};
        if (matches_focus(control))
            renderer.draw_box(rect, {.fill = palette.surface_container_highest,
                                     .radius = {},
                                     .border_width = 1,
                                     .border = palette.primary});
        const float key_width = std::min(210.0F, width * 0.33F);
        renderer.draw_text(value, x + 24, y + 7, 13, accent,
                           std::max(1.0F, narrow ? width - 36 : key_width - 36));
        renderer.draw_text(label, narrow ? x + 24 : x + key_width, y + (narrow ? 28 : 7), 13,
                           palette.on_surface,
                           std::max(1.0F, narrow ? width - 36 : width - key_width - 12));
        if (!narrow && !detail.empty())
            renderer.draw_text(detail, x + key_width, y + 28, 11, palette.on_surface_variant,
                               std::max(1.0F, width - key_width - 12));
        core::AccessibleNode node;
        node.id = "settings-row:" + target;
        node.name = label;
        node.description = std::string{value} + (detail.empty() ? "" : " · " + std::string{detail});
        node.role = core::AccessibleRole::list_item;
        node.focused = matches_focus(control);
        renderer.accessible(std::move(node), rect);
        controls_.push_back(std::move(control));
        y += row_height;
    };
    const auto buttons = [&](const std::vector<std::pair<std::string_view, Operation>>& items,
                             const std::string& target = "") {
        float left = x;
        for (const auto& [label, operation] : items) {
            const float measured =
                renderer.fonts().layout(label, 13, renderer.scale()).width / renderer.scale() + 28;
            const float size = std::min(width, std::max(100.0F, measured));
            if (left != x && left + size > x + width) {
                left = x;
                y += 40;
            }
            button_at(label, {left, y, size, 32}, operation, target);
            left += size + 8;
        }
        y += 40;
    };
    const auto binding = [&](const std::string& target) {
        text(i18n::trf("Shortcut: {}", chord(target)));
        // Action forms can scroll far beyond their title. Keep recording feedback beside
        // the focused controls so entering capture mode never looks like a silent no-op.
        if (recording_ == target)
            text(i18n::tr("Press a shortcut now. Escape cancels."));
        else if (target_ == target && !pending_chord_.empty()) {
            text(i18n::trf("Not saved: {}", i18n::keys(pending_chord_)));
            if (!error_.empty())
                text(error_);
        }
        buttons({{i18n::tr("Record shortcut"), Operation::record},
                 {i18n::tr("Reset shortcut to default"), Operation::reset},
                 {i18n::tr("Disable shortcut"), Operation::disable}},
                target);
        y += 8;
    };
    if (!error_.empty() && pending_chord_.empty())
        text(error_);
    if (has_drafts())
        button(i18n::tr("Discard unfinished edit"), Operation::discard, {}, false);
    if (page_ == Page::shortcuts) {
        for (const auto& problem : shortcut_problems_)
            if (problem != error_)
                text(problem);
        if (!target_.empty()) {
            auto label = target_;
            for (const auto& [name, candidate] : commands())
                if (name == target_)
                    label = candidate;
            if (const auto entry = default_shortcut(target_);
                entry && !target_.starts_with("action:"))
                label = i18n::tr(entry->action);
            text(label);
            y += 8;
            binding(target_);
            button(i18n::tr("Back to commands"), Operation::back, {}, false);
        } else {
            field(i18n::tr("Search commands"), search_, false);

            struct CommandRow {
                std::string target, label, group;
            };

            std::vector<CommandRow> rows;
            std::vector<std::string> groups;
            for (const auto& [name, label] : commands(search_.text())) {
                const auto entry = default_shortcut(name);
                std::string category = entry                     ? std::string{entry->group}
                                       : name.starts_with("go:") ? "Folders"
                                                                 : "Actions";
                if (std::ranges::find(groups, category) == groups.end())
                    groups.push_back(category);
                rows.push_back({name,
                                entry && !name.starts_with("action:")
                                    ? std::string{i18n::tr(entry->action)}
                                    : label,
                                std::move(category)});
            }
            for (const auto& category : groups)
                if (group("shortcuts:" + category, i18n::tr(category), false))
                    for (const auto& item : rows)
                        if (item.group == category)
                            row(item.label, chord(item.target), {}, Operation::select_command,
                                item.target);
            if (group("folder-shortcuts", i18n::tr("Folder shortcut path"), false)) {
                field(i18n::tr("Folder shortcut path"), folder_);
                text(i18n::tr("Use an absolute path or ~/folder"), true);
                button(i18n::tr("Record folder shortcut"), Operation::add_folder);
            }
        }
    } else if (!action_) {
        field(i18n::tr("Search actions"), search_, false);
        button(i18n::tr("Add action"), Operation::add_action);
        const auto needle = lower(search_.text());
        for (const bool external : {true, false}) {
            std::vector<const core::Action*> rows;
            for (const auto& action : config_.actions)
                if ((action.kind == core::Action::Kind::external) == external &&
                    (lower(action.label).find(needle) != std::string::npos ||
                     lower(action.id).find(needle) != std::string::npos))
                    rows.push_back(&action);
            if (rows.empty() || !group(external ? "program-actions" : "builtin-actions",
                                       external ? i18n::tr("Program") : i18n::tr("Built-in"), true))
                continue;
            for (const auto* action : rows)
                row(action->label, action->enabled ? i18n::tr("Enabled") : i18n::tr("Disabled"),
                    action->id, Operation::select_action, action->id);
        }
        if (config_.actions.empty())
            text(i18n::tr("No custom actions yet"), true);
    } else {
        button(i18n::tr("Back to actions"), Operation::back, {}, false);
        if (fresh_)
            field(i18n::tr("Action ID"), id_);
        else
            text(i18n::trf("Action ID: {}", action_->id), true);
        field(i18n::tr("Menu label"), label_);
        if (action_->kind == core::Action::Kind::external &&
            group("action-program", i18n::tr("Program and arguments"), true)) {
            field(i18n::tr("Program"), program_);
            text(i18n::tr("Arguments are literal values, without shell expansion."), true);
            for (std::size_t i = 0; i < arguments_.size(); ++i)
                field(i18n::trf("Argument {}", i + 1), arguments_[i], true, i);
            button(i18n::tr("Add argument"), Operation::add_argument);
            if (group("argument-help", i18n::tr("Use each placeholder as a whole argument:"),
                      false)) {
                text(i18n::tr("{file}: one selected file"), true);
                text(i18n::tr("{files}: one or more selected files"), true);
                text(i18n::tr("{directory}: the current folder"), true);
                text(i18n::tr("{destination}: the chosen destination"), true);
            }
        }
        if (group("action-availability", i18n::tr("Availability"), true)) {
            std::string_view selection;
            switch (action_->selection) {
                case core::Action::Selection::one:
                    selection = i18n::tr("Selection: one item");
                    break;
                case core::Action::Selection::many:
                    selection = i18n::tr("Selection: one or more items");
                    break;
                case core::Action::Selection::background:
                    selection = i18n::tr("Selection: folder background");
                    break;
                case core::Action::Selection::any:
                    selection = i18n::tr("Selection: any");
                    break;
            }
            std::vector<std::pair<std::string_view, Operation>> availability{
                {selection, Operation::selection}};
            if (action_->kind == core::Action::Kind::external) {
                const auto destination = action_->destination;
                availability.emplace_back(destination == core::Action::Destination::none
                                              ? i18n::tr("Destination picker: none")
                                          : destination == core::Action::Destination::file
                                              ? i18n::tr("Destination picker: file")
                                              : i18n::tr("Destination picker: folder"),
                                          Operation::destination);
            }
            availability.emplace_back(action_->enabled ? i18n::tr("Enabled") : i18n::tr("Disabled"),
                                      Operation::enabled);
            buttons(availability);
        }
        if (!fresh_) {
            if (group("action-shortcut", i18n::tr("Keyboard shortcuts"), true))
                binding("action:" + action_->id);
        } else {
            text(i18n::tr("Complete the ID, label and program to save this action."), true);
        }
    }
    focus_ = -1;
    if (focused_control) {
        const auto found = std::ranges::find_if(controls_, matches_focus);
        if (found != controls_.end())
            focus_ = static_cast<int>(std::distance(controls_.begin(), found));
    }
    const float content = y + drawn_offset - body.y;
    scroll_.set_extent(content, body.height);
    if (focus_ >= 0 && focused_control) {
        const auto& rect = controls_[static_cast<std::size_t>(focus_)].bounds;
        const float top = rect.y - body.y + drawn_offset;
        const float previous_top = focused_control->bounds.y - previous_body.y + layout_offset_;
        const bool moved = top != previous_top || rect.height != focused_control->bounds.height ||
                           previous_body.width != body.width || previous_body.height != body.height;
        if (moved && top < scroll_.offset())
            scroll_.jump_to(top);
        else if (moved && top + rect.height > scroll_.offset() + body.height)
            scroll_.jump_to(top + rect.height - body.height);
    }
    layout_offset_ = drawn_offset;
    if (scroll_.offset() != drawn_offset)
        redraw_();
    renderer.pop_clip();
    renderer.fill_rect(scrollbar_.thumb(scroll_), palette.outline);
}

}  // namespace rummage::views
