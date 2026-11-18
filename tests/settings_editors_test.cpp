#include "rummage/views/settings_editors.hpp"

#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "rummage/core/config_edit.hpp"
#include "rummage/views/shortcuts.hpp"

using namespace rummage;

namespace {
struct EditorFixture {
    std::string text;
    int writes{0};
    views::SettingsEditors editor{
        [this](const std::vector<core::ConfigEdit>& edits, std::string_view append) {
            auto changed = core::edit_config_text(text, edits, append);
            if (!changed.error.empty())
                return changed.error;
            const auto result = core::parse_config(changed.text, "editor test");
            // Unknown keys deliberately survive the edit; the host chooses diagnostic policy.
            if (!result.actions_valid)
                return std::string{"invalid actions"};
            const auto problems =
                views::configure_shortcuts(result.config.keybindings, result.config.actions, false);
            if (!problems.empty())
                return problems.front();
            text = std::move(changed.text);
            ++writes;
            return std::string{};
        },
        [] {}};

    explicit EditorFixture(std::string initial = {}) : text{std::move(initial)} {
        editor.reset(core::parse_config(text, "editor test").config, true);
    }
};
}  // namespace

TEST_CASE("settings editor records physical chords and rejects modifier-only events") {
    CHECK_FALSE(views::SettingsEditors::chord_for({.keysym = XKB_KEY_Control_L, .text = {}}));
    CHECK_FALSE(
        views::SettingsEditors::chord_for({.keysym = XKB_KEY_a, .text = {}, .repeat = true}));
    CHECK(views::SettingsEditors::chord_for(
              {.keysym = XKB_KEY_A, .text = {}, .modifiers = {.ctrl = true, .shift = true}}) ==
          "Ctrl+Shift+a");
    CHECK(views::SettingsEditors::chord_for({.keysym = XKB_KEY_ISO_Left_Tab,
                                             .text = {},
                                             .modifiers = {.ctrl = true, .shift = true}}) ==
          "Ctrl+Shift+Tab");
    EditorFixture fixture;
    fixture.editor.record_shortcut("copy");
    CHECK(fixture.editor.recording());
    CHECK(fixture.editor.key({.keysym = XKB_KEY_Escape, .text = {}}));
    CHECK_FALSE(fixture.editor.recording());
    CHECK(fixture.writes == 0);
}

TEST_CASE("settings editor searches commands and validates shortcut conflicts immediately") {
    EditorFixture fixture;
    const auto results = fixture.editor.commands("COPY_PATH");
    REQUIRE(results.size() == 1);
    CHECK(results.front().first == "copy_path");
    fixture.editor.record_shortcut("copy");
    fixture.editor.key({.keysym = XKB_KEY_v, .text = {}, .modifiers = {.ctrl = true}});
    CHECK_FALSE(fixture.editor.error().empty());
    CHECK(fixture.editor.has_drafts());
    CHECK(fixture.writes == 0);
    CHECK(fixture.editor.assign_shortcut("copy", "Super+F12"));
    CHECK_FALSE(fixture.editor.has_drafts());
    CHECK(fixture.writes == 1);
    CHECK(fixture.editor.assign_shortcut("copy", ""));
    CHECK(core::parse_config(fixture.text, "test").config.keybindings.at("copy").empty());
    CHECK(fixture.editor.assign_shortcut("copy", std::nullopt));
    CHECK_FALSE(core::parse_config(fixture.text, "test").config.keybindings.contains("copy"));
}

TEST_CASE("settings editor preserves action IDs metadata and literal argument values") {
    EditorFixture fixture{R"(
# keep this comment
[future]
value = "unknown"
[[actions]]
id = "inspect"
label = "Inspect"
argv = ["tool", "{file}"]
selection = "one"
mime = ["text/*"]
placement = "menu"
notify_on_completion = true
)"};
    REQUIRE(fixture.editor.select_action("inspect"));
    auto action = core::parse_config(fixture.text, "test").config.actions.front();
    action.label = "Inspect safely";
    action.argv = {"program with spaces", "$(touch /tmp/never-run)", "", "--file", "{file}"};
    REQUIRE(fixture.editor.update_action(action));
    const auto parsed = core::parse_config(fixture.text, "test");
    REQUIRE(parsed.actions_valid);
    CHECK(parsed.config.actions.front().argv == action.argv);
    CHECK(parsed.config.actions.front().mime == std::vector<std::string>{"text/*"});
    CHECK(parsed.config.actions.front().notify_on_completion);
    CHECK(parsed.config.actions.front().placement == core::Action::Placement::menu);
    CHECK(fixture.text.find("# keep this comment") != std::string::npos);
    CHECK(fixture.text.find("value = \"unknown\"") != std::string::npos);
    action.id = "changed";
    CHECK_FALSE(fixture.editor.update_action(action));
    CHECK(fixture.writes == 1);
}

TEST_CASE("settings editor keeps incomplete and duplicate actions as drafts") {
    EditorFixture fixture;
    fixture.editor.new_action();
    CHECK(fixture.editor.has_drafts());
    CHECK_FALSE(fixture.editor.commit());
    CHECK(fixture.writes == 0);
    core::Action action{.id = "open_custom",
                        .label = "Custom",
                        .argv = {"tool", "{files}"},
                        .mime = {},
                        .selection = core::Action::Selection::many};
    REQUIRE(fixture.editor.update_action(action));
    CHECK_FALSE(fixture.editor.has_drafts());
    CHECK(fixture.writes == 1);
    fixture.editor.new_action();
    CHECK_FALSE(fixture.editor.update_action(action));
    CHECK(fixture.writes == 1);
    fixture.editor.discard();
    CHECK_FALSE(fixture.editor.has_drafts());
    REQUIRE(fixture.editor.select_action("open_custom"));
    action.selection = core::Action::Selection::background;
    CHECK_FALSE(fixture.editor.update_action(action));
    CHECK(fixture.writes == 1);
}

TEST_CASE("settings editor disables mutation when configuration becomes read only") {
    EditorFixture fixture;
    fixture.editor.sync({}, false);
    fixture.editor.record_shortcut("copy");
    CHECK_FALSE(fixture.editor.recording());
    CHECK_FALSE(fixture.editor.assign_shortcut("copy", "Super+F12"));
    fixture.editor.new_action();
    CHECK_FALSE(fixture.editor.has_drafts());
    CHECK_FALSE(fixture.editor.update_action(
        {.id = "safe", .label = "Safe", .argv = {"tool"}, .mime = {}}));
    CHECK(fixture.writes == 0);
    CHECK_FALSE(fixture.editor.commands("copy").empty());
}
