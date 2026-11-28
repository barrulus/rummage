#include "rummage/views/settings_editors.hpp"

#include <algorithm>
#include <array>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "rummage/core/config_edit.hpp"
#include "rummage/core/i18n.hpp"
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

TEST_CASE("settings editor reports inherited shortcut problems without making a draft") {
    EditorFixture fixture{"[keybindings]\nnew_folder = 'Ctrl+n'\n"};
    REQUIRE(fixture.editor.shortcut_problems().size() == 1);
    CHECK(fixture.editor.shortcut_problems().front().find("New Window") != std::string::npos);
    CHECK(fixture.editor.error().empty());
    CHECK_FALSE(fixture.editor.has_drafts());
    CHECK(fixture.editor.commit());
    CHECK(fixture.writes == 0);
    auto accepted = core::parse_config(fixture.text, "test").config;
    accepted.appearance.background_opacity = 0.4F;
    fixture.editor.sync(accepted, true);
    CHECK(fixture.editor.shortcut_problems().size() == 1);
    CHECK(fixture.editor.error().empty());
    REQUIRE(fixture.editor.assign_shortcut("new_folder", "Ctrl+Alt+n"));
    CHECK(fixture.editor.shortcut_problems().empty());
    CHECK(fixture.editor.error().empty());
    CHECK_FALSE(fixture.editor.has_drafts());
    fixture.editor.sync(accepted, true);
    CHECK(fixture.editor.shortcut_problems().size() == 1);

    EditorFixture missing_action{"[keybindings]\n'action:hello' = 'Ctrl+Alt+h'\n"};
    REQUIRE(missing_action.editor.shortcut_problems().size() == 1);
    missing_action.editor.new_action();
    REQUIRE(missing_action.editor.update_action(
        {.id = "hello", .label = "Hello", .argv = {"echo", "hello"}, .mime = {}}));
    CHECK(missing_action.editor.shortcut_problems().empty());
}

TEST_CASE("settings editor translates built-in action shortcuts and searches Japanese labels") {
    struct RestoreLanguage {
        std::string previous{i18n::language()};

        ~RestoreLanguage() { i18n::set_language(previous); }
    } restore;

    i18n::set_language("ja");
    EditorFixture fixture;
    for (const auto& [id, label] : std::array{std::pair{"action:copy_location", "場所をコピー"},
                                              std::pair{"action:extract_here", "ここに展開"},
                                              std::pair{"action:extract_to", "展開先を選択…"}}) {
        const auto by_id = fixture.editor.commands(id);
        REQUIRE(by_id.size() == 1);
        CHECK(by_id.front().second == label);
        const auto by_label = fixture.editor.commands(label);
        REQUIRE(by_label.size() == 1);
        CHECK(by_label.front().first == id);
    }

    // A user's label stays literal even when it matches a bundled English message.
    EditorFixture custom{
        "[[actions]]\nid='extract_here'\nlabel='Extract Here'\nargv=['tool']\n"
        "[[actions]]\nid='extract_to'\nenabled=false\n"
        "[[actions]]\nid='custom'\nlabel='Copy Location'\nargv=['tool']\n"};
    const auto replaced = custom.editor.commands("action:extract_here");
    REQUIRE(replaced.size() == 1);
    CHECK(replaced.front().second == "Extract Here");
    const auto added = custom.editor.commands("action:custom");
    REQUIRE(added.size() == 1);
    CHECK(added.front().second == "Copy Location");
    const auto inherited = custom.editor.commands("action:extract_to");
    REQUIRE(inherited.size() == 1);
    CHECK(inherited.front().second == "展開先を選択…");

    for (const auto locale : {"de", "es", "fr", "ja", "pt_BR", "ru", "zh_CN"}) {
        CAPTURE(locale);
        i18n::set_language(locale);
        CHECK(i18n::tr("Remove") != "Remove");
        for (const auto& [id, english] :
             std::array{std::pair{"action:copy_location", "Copy Location"},
                        std::pair{"action:extract_here", "Extract Here"},
                        std::pair{"action:extract_to", "Extract To…"}}) {
            const auto commands = fixture.editor.commands(id);
            REQUIRE(commands.size() == 1);
            CHECK(commands.front().second != english);
            CHECK(commands.front().second == i18n::tr(english));
        }
    }
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
    REQUIRE(tab());  // program group, even though a new diagnostic/discard row appeared
    REQUIRE(tab());  // program
    all();
    CHECK(fixture.editor.copy_selection() == "tool");
    CHECK(fixture.writes == 0);
    REQUIRE(tab(true));  // program group
    REQUIRE(tab(true));  // label
    all();
    fixture.editor.paste("New label");
    draw();
    REQUIRE(tab());  // commit complete edit; retain focus when draft row disappears
    REQUIRE(tab());  // program
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
    tab();  // program group
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
    ui::AccessibleFrame frame;
    renderer.begin_frame(400, 500, 1, {});
    fixture.editor.draw(renderer, {0, 0, 400, 500}, {});
    renderer.end_frame();
    const auto back =
        std::ranges::find(frame.nodes, "Back to commands", &core::AccessibleNode::name);
    REQUIRE(back != frame.nodes.end());
    fixture.editor.pointer({.kind = ui::PointerEvent::Kind::press,
                            .x = static_cast<float>(back->bounds.x + back->bounds.width / 2),
                            .y = static_cast<float>(back->bounds.y + back->bounds.height / 2),
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
    REQUIRE(fixture.editor.input_field() != nullptr);
    const auto bounds = fixture.editor.input_field()->bounds();
    glReadPixels(static_cast<int>(bounds.x + 7), 500 - static_cast<int>(bounds.y + 16), 1, 1,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
    const ui::Color selected{static_cast<float>(pixel[0]) / 255, static_cast<float>(pixel[1]) / 255,
                             static_cast<float>(pixel[2]) / 255, 1};
    CHECK(theme::contrast(selected, palette.on_surface) >= 4.45F);  // 8-bit rounding tolerance.
}

TEST_CASE("settings editor groups shortcuts without changing active bindings") {
    ui::Offscreen surface{900, 750};
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    REQUIRE(views::configure_shortcuts({}).empty());
    const auto original = views::keys_for(views::Command::copy);
    EditorFixture fixture{"[keybindings]\ncopy = \"Super+F12\"\n"};
    std::vector<core::AccessibleNode> nodes;
    const auto draw = [&] {
        ui::AccessibleFrame frame;
        renderer.begin_frame(900, 750, 1, {});
        fixture.editor.draw(renderer, {0, 0, 900, 750}, {});
        renderer.end_frame();
        nodes = frame.nodes;
    };
    const auto find = [&](std::string_view id) {
        return std::ranges::find(nodes, id, &core::AccessibleNode::id);
    };
    draw();
    CHECK(find("settings-row:copy") == nodes.end());
    auto files = find("settings-group:shortcuts:Files");
    REQUIRE(files != nodes.end());
    CHECK_FALSE(files->checked);
    fixture.editor.pointer({.kind = ui::PointerEvent::Kind::press,
                            .x = static_cast<float>(files->bounds.x + 40),
                            .y = static_cast<float>(files->bounds.y + 15),
                            .button = ui::PointerButton::left});
    draw();
    auto copy = find("settings-row:copy");
    REQUIRE(copy != nodes.end());
    CHECK(copy->description == "Super+F12");
    CHECK(views::keys_for(views::Command::copy) == original);
    CHECK(fixture.writes == 0);
    REQUIRE(fixture.editor.key({.keysym = XKB_KEY_Left, .text = {}}));
    draw();
    CHECK(find("settings-row:copy") == nodes.end());

    // Search temporarily opens matching groups, without replacing the saved expansion state.
    fixture.editor.enter_focus();
    fixture.editor.paste("copy");
    draw();
    REQUIRE(find("settings-row:copy") != nodes.end());
    fixture.editor.key({.keysym = XKB_KEY_a, .text = {}, .modifiers = {.ctrl = true}});
    CHECK(fixture.editor.copy_selection(true) == "copy");
    draw();
    CHECK(find("settings-row:copy") == nodes.end());
    CHECK(fixture.writes == 0);

    const auto general = find("settings-group:shortcuts:General");
    REQUIRE(general != nodes.end());
    fixture.editor.pointer({.kind = ui::PointerEvent::Kind::press,
                            .x = static_cast<float>(general->bounds.x + 40),
                            .y = static_cast<float>(general->bounds.y + 15),
                            .button = ui::PointerButton::left});
    draw();
    fixture.editor.pointer(
        {.kind = ui::PointerEvent::Kind::scroll, .x = 300, .y = 400, .wheel_y = 20});
    draw();
    CHECK(find("settings-group:shortcuts:General") == nodes.end());
    // Wheel scrolling must not jump back to the still-focused group header.
    draw();
    CHECK(find("settings-group:shortcuts:General") == nodes.end());
}

TEST_CASE("settings editor aligns action arguments and removes only the chosen argument") {
    ui::Offscreen surface{900, 750};
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    EditorFixture fixture{R"([[actions]]
id = "inspect"
label = "Inspect"
argv = ["tool", "{file}", "literal argument"]
)"};
    fixture.editor.set_page(views::SettingsEditors::Page::actions);
    std::vector<core::AccessibleNode> nodes;
    const auto draw = [&] {
        ui::AccessibleFrame frame;
        renderer.begin_frame(900, 750, 1, {});
        fixture.editor.draw(renderer, {0, 0, 900, 750}, {});
        renderer.end_frame();
        nodes = frame.nodes;
    };
    const auto find = [&](std::string_view id) {
        return std::ranges::find(nodes, id, &core::AccessibleNode::id);
    };
    draw();
    REQUIRE(find("settings-row:inspect") != nodes.end());
    CHECK(find("settings-row:inspect")->description == "Enabled · inspect");
    REQUIRE(fixture.editor.select_action("inspect"));
    draw();
    const auto argument = std::ranges::find_if(nodes, [](const auto& node) {
        return node.name == "Argument 1" && node.role == core::AccessibleRole::entry;
    });
    REQUIRE(argument != nodes.end());
    const auto remove = find("remove-argument:0");
    REQUIRE(remove != nodes.end());
    CHECK(argument->bounds.y == remove->bounds.y);
    CHECK(argument->bounds.x + argument->bounds.width < remove->bounds.x);
    CHECK(remove->bounds.x + remove->bounds.width <= 900);
    fixture.editor.pointer({.kind = ui::PointerEvent::Kind::press,
                            .x = static_cast<float>(remove->bounds.x + 15),
                            .y = static_cast<float>(remove->bounds.y + 15),
                            .button = ui::PointerButton::left});
    const auto saved = core::parse_config(fixture.text, "test");
    REQUIRE(saved.config.actions.size() == 1);
    CHECK(saved.config.actions.front().argv ==
          std::vector<std::string>{"tool", "literal argument"});
    CHECK(fixture.writes == 1);
}

TEST_CASE("settings editor narrow action form reveals complete shortcut buttons by keyboard") {
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
    std::vector<core::AccessibleNode> nodes;
    const auto draw = [&] {
        ui::AccessibleFrame frame;
        renderer.begin_frame(320, 260, 1, {});
        fixture.editor.draw(renderer, {0, 0, 320, 260}, {});
        renderer.end_frame();
        nodes = frame.nodes;
    };
    draw();
    fixture.editor.enter_focus();
    bool reached = false;
    for (int step = 0; step < 30; ++step) {
        draw();
        draw();  // layout changes can request a second frame after adjusting the scroll extent
        const auto disable = std::ranges::find_if(nodes, [](const auto& node) {
            return node.name == "Disable shortcut" && node.focused;
        });
        if (disable != nodes.end()) {
            CHECK(disable->bounds.height == 32);
            CHECK(disable->bounds.y >= 0);
            CHECK(disable->bounds.y + disable->bounds.height <= 260);
            REQUIRE(fixture.editor.key({.keysym = XKB_KEY_Return, .text = {}}));
            reached = true;
            break;
        }
        REQUIRE(fixture.editor.key({.keysym = XKB_KEY_Tab, .text = {}}));
    }
    REQUIRE(reached);
    CHECK(fixture.writes == 1);
    CHECK(core::parse_config(fixture.text, "test").config.keybindings.at("action:inspect").empty());
}
