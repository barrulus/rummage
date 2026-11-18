#include "rummage/views/settings_editors.hpp"

#include <array>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "rummage/core/config_edit.hpp"
#include "rummage/theme/desktop.hpp"
#include "rummage/ui/offscreen.hpp"
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

TEST_CASE(
    "settings editor retains drafts across redraw and makes narrow forms keyboard accessible") {
    ui::Offscreen surface{320, 260};
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    EditorFixture fixture{R"([[actions]]
id = "inspect"
label = "Inspect"
argv = ["tool", "{file}"]
)"};
    fixture.editor.set_page(views::SettingsEditors::Page::actions);
    REQUIRE(fixture.editor.select_action("inspect"));
    const auto draw = [&] {
        renderer.begin_frame(320, 260, 1, {});
        fixture.editor.draw(renderer, {0, 0, 320, 260}, {});
        renderer.end_frame();
    };
    const auto tab = [&](bool reverse = false) {
        const bool handled = fixture.editor.key(
            {.keysym = XKB_KEY_Tab, .text = {}, .modifiers = {.shift = reverse}});
        draw();
        return handled;
    };
    const auto all = [&] {
        fixture.editor.key({.keysym = XKB_KEY_a, .text = {}, .modifiers = {.ctrl = true}});
    };
    draw();
    fixture.editor.enter_focus();
    REQUIRE(tab());  // label
    all();
    CHECK(fixture.editor.copy_selection(true) == "Inspect");
    draw();
    CHECK(fixture.editor.has_drafts());
    REQUIRE(tab());  // program, even though a new diagnostic/discard row appeared
    all();
    CHECK(fixture.editor.copy_selection() == "tool");
    CHECK(fixture.writes == 0);
    REQUIRE(tab(true));  // label
    all();
    fixture.editor.paste("New label");
    draw();
    REQUIRE(tab());  // commit complete edit; retain focus when draft row disappears
    all();
    CHECK(fixture.editor.copy_selection() == "tool");
    CHECK(fixture.writes == 1);
    CHECK_FALSE(fixture.editor.has_drafts());
    CHECK(core::parse_config(fixture.text, "test").config.actions.front().label == "New label");
    bool left = false;
    for (int i = 0; i < 30 && !left; ++i)
        left = !tab();
    CHECK(left);
    fixture.editor.sync(core::parse_config(fixture.text, "test").config, false);
    draw();
    fixture.editor.enter_focus();
    REQUIRE(tab());  // readonly label still selectable
    all();
    CHECK(fixture.editor.copy_selection(true) == "New label");
    fixture.editor.paste("No change");
    CHECK(fixture.editor.copy_selection() == "New label");
    CHECK_FALSE(fixture.editor.has_drafts());
    CHECK(fixture.writes == 1);
}

TEST_CASE("settings editor saves a new action only after its required fields are complete") {
    ui::Offscreen surface{320, 260};
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    EditorFixture fixture;
    fixture.editor.set_page(views::SettingsEditors::Page::actions);
    fixture.editor.new_action();
    const auto draw = [&] {
        renderer.begin_frame(320, 260, 1, {});
        fixture.editor.draw(renderer, {0, 0, 320, 260}, {});
        renderer.end_frame();
    };
    const auto tab = [&] {
        REQUIRE(fixture.editor.key({.keysym = XKB_KEY_Tab, .text = {}}));
        draw();
    };
    draw();
    fixture.editor.enter_focus();  // discard
    tab();                         // back
    tab();                         // ID
    fixture.editor.paste("new_action");
    draw();
    tab();  // label
    CHECK(fixture.writes == 0);
    fixture.editor.paste("New action");
    draw();
    tab();  // program
    CHECK(fixture.writes == 0);
    fixture.editor.paste("literal-program");
    draw();
    tab();  // the completed action is now accepted by the host
    REQUIRE(fixture.writes == 1);
    const auto parsed = core::parse_config(fixture.text, "test");
    REQUIRE(parsed.config.actions.size() == 1);
    CHECK(parsed.config.actions.front().id == "new_action");
    CHECK(parsed.config.actions.front().label == "New action");
    CHECK(parsed.config.actions.front().argv == std::vector<std::string>{"literal-program"});
    CHECK_FALSE(fixture.editor.has_drafts());
}

TEST_CASE("settings editor back navigation cancels recording before command search") {
    ui::Offscreen surface{400, 500};
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    EditorFixture fixture;
    fixture.editor.focus_shortcut("copy");
    fixture.editor.record_shortcut("copy");
    renderer.begin_frame(400, 500, 1, {});
    fixture.editor.draw(renderer, {0, 0, 400, 500}, {});
    renderer.end_frame();
    // Prompt + target + current shortcut, then Record / Reset / Disable / Back.
    fixture.editor.pointer({.kind = ui::PointerEvent::Kind::press,
                            .x = 30,
                            .y = 205,
                            .button = ui::PointerButton::left});
    CHECK_FALSE(fixture.editor.recording());
    CHECK(fixture.writes == 0);
    renderer.begin_frame(400, 500, 1, {});
    fixture.editor.draw(renderer, {0, 0, 400, 500}, {});
    renderer.end_frame();
    fixture.editor.enter_focus();
    fixture.editor.key({.keysym = XKB_KEY_a, .text = "a"});
    CHECK(fixture.writes == 0);
}

TEST_CASE("settings editor selected text retains contrast with explicit light accent roles") {
    ui::Offscreen surface{400, 500};
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    EditorFixture fixture;
    theme::Palette palette;
    palette.on_surface = palette.primary = palette.secondary_container = ui::Color::rgb(0xffffff);
    const auto draw = [&] {
        renderer.begin_frame(400, 500, 1, {});
        fixture.editor.draw(renderer, {0, 0, 400, 500}, palette);
        renderer.end_frame();
    };
    draw();
    fixture.editor.enter_focus();
    fixture.editor.paste("       ");  // No glyphs cover the selected background sample.
    fixture.editor.key({.keysym = XKB_KEY_a, .text = {}, .modifiers = {.ctrl = true}});
    draw();
    std::array<unsigned char, 4> pixel{};
    glReadPixels(14, 459, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
    const ui::Color selected{static_cast<float>(pixel[0]) / 255, static_cast<float>(pixel[1]) / 255,
                             static_cast<float>(pixel[2]) / 255, 1};
    CHECK(theme::contrast(selected, palette.on_surface) >= 4.45F);  // 8-bit rounding tolerance.
}
