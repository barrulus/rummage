#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <string>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "rummage/core/config_edit.hpp"
#include "rummage/core/file.hpp"
#include "rummage/core/i18n.hpp"
#include "rummage/ui/offscreen.hpp"
#include "rummage/views/confirm_dialog.hpp"
#include "rummage/views/settings_dialog.hpp"
#include "rummage/views/shortcuts.hpp"
#include "temp_dir.hpp"

using namespace std::chrono_literals;
namespace core = rummage::core;
namespace views = rummage::views;

namespace {
bool settle(core::EventLoop& loop, const std::function<bool()>& ready) {
    bool done = false;
    const auto poll = loop.add_timer(1ms, 1ms, [&] {
        if (ready()) {
            done = true;
            loop.quit(0);
        }
    });
    const auto timeout = loop.add_timer(3s, 0ms, [&] { loop.quit(1); });
    loop.run();
    loop.remove(poll);
    loop.remove(timeout);
    return done;
}

struct Fixture {
    rummage::test::TempDir dir;
    std::filesystem::path path = dir.path / "config.toml";
    core::EventLoop loop;
    core::WorkerPool pool{loop, 1};
    views::SettingsDialog dialog{pool, [] {}};
    int applied{0};

    void focus(std::string query) {
        dialog.search(std::move(query));
        const auto results = dialog.search_results();
        REQUIRE(results.size() == 1);
        dialog.jump_to_result(results.front().field);
    }

    void open() {
        dialog.open(path, [] {}, [this](const core::ConfigResult&) { ++applied; });
        REQUIRE(settle(loop, [&] { return !dialog.busy(); }));
        REQUIRE(dialog.error().empty());
    }
};
}  // namespace

TEST_CASE("settings: text edits preserve comments, Unicode, and unrelated tables") {
    const std::string original =
        "# My settings\n[icon]\ntheme = 'éclair' # keep this\nfollow_accent = "
        "false\n\n[font]\nfamily = 'Inter'\n";
    const auto edited =
        core::edit_config_text(original, {{"icon", "theme", "\"builtin\""},
                                          {"appearance", "preview_position", "'sidebar'"},
                                          {"font", "monospace", "'Iosevka'"}});
    REQUIRE_MESSAGE(edited.error.empty(), edited.error);
    CHECK(edited.text.find("theme = \"builtin\" # keep this") != std::string::npos);
    CHECK(edited.text.find("follow_accent = false") != std::string::npos);
    CHECK(edited.text.find("family = 'Inter'") != std::string::npos);
    const auto config = core::parse_config(edited.text, "test");
    REQUIRE(config.problems.empty());
    CHECK(config.config.appearance.preview_position == "sidebar");
    CHECK(config.config.font.monospace == "Iosevka");
}

TEST_CASE("settings: existing actions and inline tables retain unrelated fields") {
    const std::string original =
        "icon = { theme = 'system' }\n[[actions]]\nid = 'hello'\nlabel = 'Hello'\nargv = ['echo', "
        "'hello'] # command\nselection = 'any'\n";
    const auto edited = core::edit_config_text(original, {{"icon", "follow_accent", "false"},
                                                          {"actions[0]", "label", "'Bonjour'"},
                                                          {"actions[0]", "enabled", "false"}});
    REQUIRE_MESSAGE(edited.error.empty(), edited.error);
    CHECK(edited.text.find("argv = ['echo', 'hello'] # command") != std::string::npos);
    const auto config = core::parse_config(edited.text, "test");
    REQUIRE(config.problems.empty());
    REQUIRE(config.config.actions.size() == 1);
    CHECK(config.config.actions[0].label == "Bonjour");
    CHECK_FALSE(config.config.actions[0].enabled);
    CHECK_FALSE(config.config.icon.follow_accent);
}

TEST_CASE("settings: motion controls save live and preserve per-event overrides") {
    Fixture f;
    REQUIRE_FALSE(core::write_file_atomically(f.path, "[animations.events]\nfile_copy = 'none'\n"));
    f.open();
    REQUIRE(f.dialog.edit("animations", "enabled", "false"));
    REQUIRE(f.dialog.edit("animations", "reduce_motion", "always"));
    REQUIRE(f.dialog.edit("animations", "quality", "low"));
    REQUIRE(f.dialog.edit("animations", "duration_scale", "0.5"));
    f.dialog.save();
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    REQUIRE_MESSAGE(f.dialog.error().empty(), f.dialog.error());
    CHECK(f.applied == 1);
    const auto loaded = core::load_config(f.path);
    REQUIRE(loaded.problems.empty());
    CHECK_FALSE(loaded.config.animations.enabled);
    CHECK(loaded.config.animations.duration_scale == 0.5F);
    CHECK(loaded.config.animations.reduce_motion == core::MotionConfig::Reduce::always);
    CHECK(loaded.config.animations.events[3] == core::MotionEffect::none);
}

TEST_CASE("settings: adding a field to the second action preserves its array position") {
    const auto edited = core::edit_config_text(
        "[[actions]]\nid='first'\nlabel='First'\nargv=['echo']\n"
        "[[actions]]\nid='second'\nlabel='Second'\nargv=['echo']\n",
        {{"actions[1]", "enabled", "false"}});
    REQUIRE_MESSAGE(edited.error.empty(), edited.error);
    const auto parsed = core::parse_config(edited.text, "test");
    REQUIRE(parsed.problems.empty());
    REQUIRE(parsed.config.actions.size() == 2);
    CHECK(parsed.config.actions[0].enabled);
    CHECK_FALSE(parsed.config.actions[1].enabled);
}

TEST_CASE("settings: invalid replacement cannot inject another setting") {
    CHECK_FALSE(
        core::edit_config_text("", {{"icon", "theme", "'system'\nextra = true"}}).error.empty());
    const auto quoted = core::quote_config_string("hello\n\"world\"");
    CHECK(core::edit_config_text("", {{"font", "family", quoted}}).error.empty());
}

TEST_CASE("settings: Close commits to the selected file and waits for persistence") {
    Fixture f;
    const std::string original = "# preserved\n[icon]\ntheme = 'system'\n";
    REQUIRE_FALSE(core::write_file_atomically(f.path, original));
    f.open();
    REQUIRE(f.dialog.edit("icon", "theme", "builtin"));
    f.dialog.close();
    REQUIRE(f.dialog.is_open());
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    CHECK_FALSE(f.dialog.is_open());
    CHECK(core::load_config(f.path).config.icon.theme == "builtin");
    f.open();
    REQUIRE(f.dialog.edit("appearance", "preview_position", "sidebar"));
    f.dialog.save();
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    REQUIRE_MESSAGE(f.dialog.error().empty(), f.dialog.error());
    CHECK(f.dialog.is_open());
    CHECK(f.applied == 2);
    CHECK(core::load_config(f.path).config.appearance.preview_position == "sidebar");
    CHECK(core::read_text_file(f.path).text.starts_with("# preserved\n"));
}

TEST_CASE("settings: validation and concurrent edits protect the configuration") {
    Fixture f;
    REQUIRE_FALSE(core::write_file_atomically(f.path, "# original\n"));
    f.open();
    REQUIRE(f.dialog.edit("keybindings", "toggle_split", "Ctrl+Q"));
    f.dialog.save();
    CHECK_FALSE(f.dialog.error().empty());
    CHECK(core::read_text_file(f.path).text == "# original\n");
    CHECK(views::keys_for(views::Command::toggle_split) == "F3");
    REQUIRE(f.dialog.edit("keybindings", "toggle_split", "F8"));
    REQUIRE_FALSE(core::write_file_atomically(f.path, "# edited externally\n"));
    f.dialog.save();
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    CHECK_FALSE(f.dialog.error().empty());
    CHECK(core::read_text_file(f.path).text == "# edited externally\n");
    CHECK(f.applied == 0);
}

TEST_CASE("settings: create and disable a custom action without running it") {
    Fixture f;
    f.open();
    f.dialog.add_action();
    REQUIRE(f.dialog.edit("new:0", "id", "hello"));
    REQUIRE(f.dialog.edit("new:0", "argv", "['echo', '{file}']"));
    f.dialog.save();
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    REQUIRE_MESSAGE(f.dialog.error().empty(), f.dialog.error());
    REQUIRE(core::load_config(f.path).config.actions.size() == 1);
    f.open();
    REQUIRE(f.dialog.edit("actions[0]", "enabled", "false"));
    f.dialog.save();
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    REQUIRE_MESSAGE(f.dialog.error().empty(), f.dialog.error());
    CHECK_FALSE(core::load_config(f.path).config.actions[0].enabled);
}

TEST_CASE("settings: implicit tables and resetting a binding preserve other settings") {
    const std::string text =
        "icon.theme = 'system'\n[keybindings]\ntoggle_split = 'F8' # personal key\ncopy = "
        "'Ctrl+C'\n";
    const auto result = core::edit_config_text(
        text, {{"icon", "follow_accent", "false"}, {"keybindings", "toggle_split", "", true}});
    REQUIRE_MESSAGE(result.error.empty(), result.error);
    CHECK(result.text.find("# personal key") != std::string::npos);
    const auto parsed = core::parse_config(result.text, "test");
    REQUIRE(parsed.problems.empty());
    CHECK_FALSE(parsed.config.keybindings.contains("toggle_split"));
    CHECK(parsed.config.keybindings.at("copy") == "Ctrl+C");
    CHECK_FALSE(parsed.config.icon.follow_accent);
}

TEST_CASE("settings: saving through a symlink retains the symlink") {
    Fixture f;
    const auto target = f.dir.path / "shared.toml";
    REQUIRE_FALSE(core::write_file_atomically(target, "# shared\n"));
    std::filesystem::create_symlink(target, f.path);
    f.open();
    REQUIRE(f.dialog.edit("appearance", "color_scheme", "dark"));
    f.dialog.save();
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    REQUIRE_MESSAGE(f.dialog.error().empty(), f.dialog.error());
    CHECK(std::filesystem::is_symlink(f.path));
    CHECK(core::load_config(target).config.appearance.color_scheme == "dark");
}

TEST_CASE("settings: scrolling and scrollbar drag reach controls in a short window") {
    Fixture f;
    f.open();
    namespace ui = rummage::ui;
    ui::Offscreen gl{400, 360};
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    ui::IconSet icons{fonts};
    const auto draw = [&] {
        renderer.begin_frame(400, 360, 1, {});
        f.dialog.draw(renderer, {0, 0, 400, 360}, rummage::theme::Palette{}, icons);
        renderer.end_frame();
    };
    draw();
    SUBCASE("wheel") {
        f.dialog.pointer(
            {.kind = ui::PointerEvent::Kind::scroll, .x = 100, .y = 200, .wheel_y = 1000});
    }
    SUBCASE("scrollbar capture") {
        CHECK(f.dialog.cursor_at(381, 174) == ui::CursorShape::pointer);
        f.dialog.pointer({.kind = ui::PointerEvent::Kind::press,
                          .x = 381,
                          .y = 174,
                          .button = ui::PointerButton::left});
        f.dialog.pointer({.kind = ui::PointerEvent::Kind::motion, .x = 500, .y = 500});
        f.dialog.pointer({.kind = ui::PointerEvent::Kind::release,
                          .x = 500,
                          .y = 500,
                          .button = ui::PointerButton::left});
        f.dialog.pointer({.kind = ui::PointerEvent::Kind::motion, .x = 381, .y = 120});
    }
    draw();
    f.dialog.pointer({.kind = ui::PointerEvent::Kind::press,
                      .x = 100,
                      .y = 216,
                      .button = ui::PointerButton::left});
    f.dialog.key({.keysym = XKB_KEY_a, .text = {}, .modifiers = {.ctrl = true}});
    CHECK(f.dialog.copy_selection(false) == "[]");
    f.dialog.pointer({.kind = ui::PointerEvent::Kind::press,
                      .x = 381,
                      .y = 165,
                      .button = ui::PointerButton::left});
    f.dialog.pointer({.kind = ui::PointerEvent::Kind::release,
                      .x = 381,
                      .y = 165,
                      .button = ui::PointerButton::left});
    draw();
    CHECK(f.dialog.cursor_at(100, 254) == ui::CursorShape::pointer);
    // Switching categories cancels an in-progress scrollbar drag.
    f.dialog.pointer({.kind = ui::PointerEvent::Kind::press,
                      .x = 381,
                      .y = 174,
                      .button = ui::PointerButton::left});
    f.dialog.key({.keysym = XKB_KEY_2, .text = {}, .modifiers = {.alt = true}});
    f.dialog.key({.keysym = XKB_KEY_1, .text = {}, .modifiers = {.alt = true}});
    f.dialog.pointer({.kind = ui::PointerEvent::Kind::motion, .x = 500, .y = 500});
    draw();
    CHECK(f.dialog.cursor_at(100, 254) == ui::CursorShape::pointer);
}

TEST_CASE("settings: action and folder shortcuts save together without duplicating drafts") {
    Fixture f;
    f.open();
    f.dialog.add_action();
    REQUIRE(f.dialog.edit("new:0", "id", "hello"));
    REQUIRE(f.dialog.edit("new:0", "argv", "['echo', '{file}']"));
    f.dialog.add_keybinding();
    REQUIRE(f.dialog.edit("binding:0", "target", "action:hello"));
    REQUIRE(f.dialog.edit("binding:0", "chord", "Ctrl+Alt+J"));
    f.dialog.add_keybinding();
    REQUIRE(f.dialog.edit("binding:1", "target", "go:~/Projects"));
    REQUIRE(f.dialog.edit("binding:1", "chord", "Ctrl+Alt+Y"));
    f.dialog.save();
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    REQUIRE_MESSAGE(f.dialog.error().empty(), f.dialog.error());
    f.dialog.save();
    const auto config = core::load_config(f.path);
    REQUIRE(config.problems.empty());
    REQUIRE(config.config.actions.size() == 1);
    CHECK(config.config.keybindings.at("action:hello") == "Ctrl+Alt+J");
    CHECK(config.config.keybindings.at("go:~/Projects") == "Ctrl+Alt+Y");
    CHECK(f.dialog.edit("keybindings", "action:hello", "Default"));
    CHECK(f.dialog.edit("keybindings", "go:~/Projects", "Default"));
    f.dialog.save();
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    CHECK(core::load_config(f.path).config.keybindings.empty());
}

TEST_CASE(
    "settings: new bindings reject invalid targets, duplicate targets and conflicting chords") {
    Fixture f;
    const std::string original = "# unchanged\n[keybindings]\n'go:~/Projects' = 'Ctrl+Alt+Y'\n";
    REQUIRE_FALSE(core::write_file_atomically(f.path, original));
    f.open();
    f.dialog.add_keybinding();
    for (const auto& [target, chord] :
         std::vector<std::pair<std::string, std::string>>{{"", "F8"},
                                                          {"action:missing", "F8"},
                                                          {"go:relative", "F8"},
                                                          {"go:~/Projects", "F8"},
                                                          {"go:/tmp", "Ctrl+Q"}}) {
        REQUIRE(f.dialog.edit("binding:0", "target", target));
        REQUIRE(f.dialog.edit("binding:0", "chord", chord));
        f.dialog.save();
        CHECK_FALSE(f.dialog.error().empty());
        CHECK_FALSE(f.dialog.busy());
        CHECK(core::read_text_file(f.path).text == original);
    }
    REQUIRE(f.dialog.edit("binding:0", "target", "go:/tmp"));
    REQUIRE(f.dialog.edit("binding:0", "chord", "F8"));
    f.dialog.add_keybinding();
    REQUIRE(f.dialog.edit("binding:1", "target", "go:/tmp"));
    REQUIRE(f.dialog.edit("binding:1", "chord", "F9"));
    f.dialog.save();
    CHECK_FALSE(f.dialog.error().empty());
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    CHECK(core::load_config(f.path).config.keybindings.at("go:/tmp") == "F8");
    f.dialog.close();
    CHECK(f.dialog.is_open());  // The duplicate draft needs explicit discard.
}

TEST_CASE("settings: interface size, terminal arguments and program opening persist") {
    Fixture f;
    f.open();
    REQUIRE(f.dialog.edit("font", "size", "17.5"));
    REQUIRE(f.dialog.edit("terminal", "program", "kitty"));
    REQUIRE(f.dialog.edit("terminal", "args", "['--title', 'My terminal']"));
    REQUIRE(f.dialog.edit("open", "programs", "false"));
    f.dialog.save();
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    REQUIRE_MESSAGE(f.dialog.error().empty(), f.dialog.error());
    auto loaded = core::load_config(f.path);
    REQUIRE(loaded.problems.empty());
    CHECK(loaded.config.font.size == 17.5F);
    CHECK(loaded.config.terminal.program == "kitty");
    CHECK(loaded.config.terminal.args == std::vector<std::string>{"--title", "My terminal"});
    CHECK_FALSE(loaded.config.open.programs);
    f.open();
    REQUIRE(f.dialog.edit("terminal", "program", ""));
    f.dialog.save();
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    REQUIRE_MESSAGE(f.dialog.error().empty(), f.dialog.error());
    CHECK(core::load_config(f.path).config.terminal.program.empty());
}

TEST_CASE("settings: language choice persists and reset follows the session after restart") {
    Fixture f;
    f.open();
    const auto active = std::string{rummage::i18n::language()};
    REQUIRE(f.dialog.edit("interface", "language", "de"));
    f.dialog.save();
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    REQUIRE(f.dialog.error().empty());
    CHECK(core::load_config(f.path).config.interface.language == "de");
    CHECK(rummage::i18n::language() == active);
    f.open();
    REQUIRE(f.dialog.reset_setting("interface", "language"));
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    CHECK(core::load_config(f.path).config.interface.language == "system");
}

TEST_CASE("settings: keyboard choices open a list and save the selected value") {
    Fixture f;
    f.open();
    f.dialog.key({.keysym = XKB_KEY_2, .text = {}, .modifiers = {.alt = true}});
    f.focus("appearance.color_scheme");
    // Open with Space, choose the final theme with End, and confirm with Enter.
    f.dialog.key({.keysym = XKB_KEY_space, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_End, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_Return, .text = {}});
    // Reopening starts at the current value; Space also confirms a menu choice.
    f.dialog.key({.keysym = XKB_KEY_Return, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_Up, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_space, .text = {}});
    f.dialog.save();
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    CHECK(f.dialog.error().empty());
    CHECK(core::load_config(f.path).config.appearance.color_scheme == "light");
}

TEST_CASE("settings: choice cancellation and Tab preserve values and move between dropdowns") {
    Fixture f;
    f.open();
    f.dialog.key({.keysym = XKB_KEY_2, .text = {}, .modifiers = {.alt = true}});
    f.focus("appearance.color_scheme");
    f.dialog.key({.keysym = XKB_KEY_Return, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_Down, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_Escape, .text = {}});
    CHECK(f.dialog.is_open());
    f.dialog.key({.keysym = XKB_KEY_Return, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_End, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_Tab, .text = {}});  // Reset this setting.
    f.dialog.key({.keysym = XKB_KEY_Tab, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_Return, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_Home, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_Down, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_Return, .text = {}});
    f.dialog.save();
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    CHECK(f.dialog.error().empty());
    const auto config = core::load_config(f.path);
    CHECK(config.config.appearance.color_scheme == "system");
    CHECK(config.config.icon.theme == "builtin");
}

TEST_CASE("settings: pointer choices select from a menu and outside clicks only dismiss") {
    Fixture f;
    f.open();
    namespace ui = rummage::ui;
    ui::Offscreen gl{800, 700};
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    ui::IconSet icons{fonts};
    const auto draw = [&] {
        renderer.begin_frame(800, 700, 1, {});
        f.dialog.draw(renderer, {0, 0, 800, 700}, rummage::theme::Palette{}, icons);
        renderer.end_frame();
    };
    const auto click = [&](float x, float y) {
        f.dialog.pointer({.kind = ui::PointerEvent::Kind::press,
                          .x = x,
                          .y = y,
                          .button = ui::PointerButton::left});
    };
    f.dialog.key({.keysym = XKB_KEY_2, .text = {}, .modifiers = {.alt = true}});
    draw();
    click(560, 280);  // Colour mode, below palette source.
    draw();
    click(560, 373);  // Third menu choice: dark.
    f.dialog.key({.keysym = XKB_KEY_2, .text = {}, .modifiers = {.alt = true}});
    draw();
    click(560, 280);
    draw();
    click(700, 648);  // Close button is behind the open menu: dismiss only.
    CHECK(f.dialog.is_open());
    draw();
    click(560, 464);  // Follow palette accent is an immediate toggle.
    f.dialog.save();
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    CHECK(f.dialog.error().empty());
    const auto config = core::load_config(f.path);
    CHECK(config.config.appearance.color_scheme == "dark");
    CHECK_FALSE(config.config.icon.follow_accent);
}

TEST_CASE("settings: automatic choice saves serialize edits made during an earlier write") {
    Fixture f;
    f.open();
    f.dialog.key({.keysym = XKB_KEY_2, .text = {}, .modifiers = {.alt = true}});
    f.focus("appearance.color_scheme");
    f.dialog.key({.keysym = XKB_KEY_Return, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_End, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_Return, .text = {}});
    REQUIRE(f.dialog.busy());
    // Do not run the loop yet: the first write's completion is still pending.
    f.dialog.key({.keysym = XKB_KEY_Tab, .text = {}});  // Reset this setting.
    f.dialog.key({.keysym = XKB_KEY_Tab, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_Return, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_Home, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_Down, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_Return, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_Tab, .text = {}});    // Skip the icon-theme reset.
    f.dialog.key({.keysym = XKB_KEY_Tab, .text = {}});    // Move to the accent toggle.
    f.dialog.key({.keysym = XKB_KEY_space, .text = {}});  // Toggle accent while saving.
    f.dialog.close();
    CHECK(f.dialog.is_open());
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    CHECK_FALSE(f.dialog.is_open());
    CHECK(f.dialog.error().empty());
    const auto loaded = core::load_config(f.path);
    CHECK(loaded.config.appearance.color_scheme == "dark");
    CHECK(loaded.config.icon.theme == "builtin");
    CHECK_FALSE(loaded.config.icon.follow_accent);
    CHECK(f.applied == 2);
}

TEST_CASE("settings: invalid fields and incomplete drafts do not block independent preferences") {
    Fixture f;
    f.open();
    f.dialog.add_action();
    REQUIRE(f.dialog.edit("new:0", "id", "unfinished"));
    REQUIRE(f.dialog.edit("font", "size", "200"));
    REQUIRE(f.dialog.edit("appearance", "color_scheme", "dark"));
    f.dialog.save();
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    CHECK_FALSE(f.dialog.error().empty());
    auto loaded = core::load_config(f.path);
    CHECK(loaded.config.appearance.color_scheme == "dark");
    CHECK(loaded.config.font.size == core::Config{}.font.size);
    CHECK(loaded.config.actions.empty());
    f.dialog.close();
    CHECK(f.dialog.is_open());
    // Correcting drafts retains the other edits and commits the action once.
    REQUIRE(f.dialog.edit("font", "size", "17"));
    REQUIRE(f.dialog.edit("new:0", "argv", "['echo', '{file}']"));
    f.dialog.close();
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    CHECK_FALSE(f.dialog.is_open());
    REQUIRE(f.dialog.error().empty());
    loaded = core::load_config(f.path);
    REQUIRE(loaded.config.actions.size() == 1);
    CHECK(loaded.config.actions[0].id == "unfinished");
    CHECK(loaded.config.font.size == 17);
}

TEST_CASE("settings: ignored options preserve working pages and survive edits") {
    Fixture f;
    const std::string original =
        "[appearance]\nfuture_appearance_option = true # keep for another version\n"
        "color_scheme = 'dark'\n[future_settings]\ncustom_option = 'preserve me'\n";
    REQUIRE_FALSE(core::write_file_atomically(f.path, original));
    f.open();
    REQUIRE(f.dialog.editable());
    f.focus("Startup folder");
    f.focus("Colour mode");
    REQUIRE(f.dialog.edit("appearance", "color_scheme", "light"));
    f.dialog.save();
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    REQUIRE(f.dialog.error().empty());
    // Exercise both the text-field commit and editor/reset apply_edits paths.
    REQUIRE(f.dialog.reset_setting("icon", "theme"));
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    REQUIRE(f.dialog.error().empty());
    const auto text = core::read_text_file(f.path).text;
    CHECK(text.find("future_appearance_option = true # keep for another version") !=
          std::string::npos);
    CHECK(text.find("custom_option = 'preserve me'") != std::string::npos);
    CHECK(core::load_config(f.path).config.appearance.color_scheme == "light");
    CHECK(f.applied > 0);
    f.dialog.close();
    REQUIRE_FALSE(f.dialog.is_open());
    f.open();
    CHECK(f.dialog.editable());
}

TEST_CASE("settings: ignored options do not hide invalid known settings") {
    Fixture f;
    const std::string original =
        "[appearance]\nfuture_appearance_option = true\n[font]\nsize = 'broken'\n";
    REQUIRE_FALSE(core::write_file_atomically(f.path, original));
    f.dialog.open(f.path, [] {}, [](const core::ConfigResult&) {});
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    CHECK_FALSE(f.dialog.editable());
    CHECK(f.dialog.error().find("font.size") != std::string::npos);
    CHECK(f.dialog.error().find("unknown key") == std::string::npos);
    f.dialog.save();
    f.dialog.close();
    CHECK(core::read_text_file(f.path).text == original);
}

TEST_CASE("settings: failed writes retain edits and Retry can persist them") {
    Fixture f;
    REQUIRE_FALSE(core::write_file_atomically(f.path, "# original\n"));
    f.open();
    REQUIRE(f.dialog.edit("startup", "mode", "folder"));
    REQUIRE(f.dialog.edit("startup", "folder", "/tmp"));
    REQUIRE_FALSE(core::write_file_atomically(f.path, "# external\n"));
    f.dialog.close();
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    CHECK(f.dialog.is_open());
    CHECK_FALSE(f.dialog.error().empty());
    CHECK(core::read_text_file(f.path).text == "# external\n");
    REQUIRE_FALSE(core::write_file_atomically(f.path, "# original\n"));
    f.dialog.save();
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    REQUIRE(f.dialog.error().empty());
    const auto loaded = core::load_config(f.path);
    CHECK(loaded.config.startup.mode == core::Config::Startup::Mode::folder);
    CHECK(loaded.config.startup.folder == "/tmp");
}

TEST_CASE("settings: discarding unsaved drafts keeps earlier automatic saves") {
    Fixture f;
    f.open();
    REQUIRE(f.dialog.edit("appearance", "color_scheme", "dark"));
    f.dialog.save();
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    REQUIRE(f.dialog.edit("font", "size", "invalid"));
    f.dialog.close();
    REQUIRE(f.dialog.is_open());
    namespace ui = rummage::ui;
    ui::Offscreen gl{400, 360};
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    ui::IconSet icons{fonts};
    renderer.begin_frame(400, 360, 1, {});
    f.dialog.draw(renderer, {0, 0, 400, 360}, rummage::theme::Palette{}, icons);
    renderer.end_frame();
    f.dialog.pointer({.kind = ui::PointerEvent::Kind::press,
                      .x = 300,
                      .y = 275,
                      .button = ui::PointerButton::left});
    CHECK_FALSE(f.dialog.is_open());
    CHECK(core::load_config(f.path).config.appearance.color_scheme == "dark");
    CHECK(core::load_config(f.path).config.font.size == core::Config{}.font.size);
}

TEST_CASE("settings: translated compact navigation preserves keyboard editing and dropdown focus") {
    namespace ui = rummage::ui;

    struct RestoreLanguage {
        ~RestoreLanguage() { rummage::i18n::set_language("en"); }
    } restore;

    rummage::i18n::set_language("de");
    Fixture f;
    REQUIRE_FALSE(core::write_file_atomically(
        f.path, "[appearance]\nfuture_appearance_option = true # another version's option\n"));
    f.open();
    ui::Offscreen surface{960, 700};
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    ui::IconSet icons{fonts};
    const auto draw = [&](const std::string& name, int width = 400, int height = 360) {
        renderer.begin_frame(width, height, 1, {});
        f.dialog.draw(renderer, {0, 0, static_cast<float>(width), static_cast<float>(height)},
                      rummage::theme::Palette{}, icons);
        renderer.end_frame();
        // Optional review artifacts, following the motion tests' offscreen-frame convention.
        if (const char* output = std::getenv("RUMMAGE_SETTINGS_FRAMES")) {
            std::vector<unsigned char> pixels(static_cast<std::size_t>(width * height * 4));
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            std::ofstream file{std::filesystem::path{output} / (name + ".ppm"), std::ios::binary};
            file << "P6\n" << width << ' ' << height << "\n255\n";
            for (int y = height - 1; y >= 0; --y)
                for (int x = 0; x < width; ++x)
                    file.write(reinterpret_cast<const char*>(pixels.data() + (y * width + x) * 4),
                               3);
        }
    };
    draw("wide-general-de", 960, 700);
    draw("narrow-general-de", 400, 700);
    draw("compact-general-de");
    f.dialog.key({.keysym = XKB_KEY_Return, .text = {}});  // Compact category selector.
    f.dialog.key({.keysym = XKB_KEY_Down, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_Return, .text = {}});  // Appearance.
    draw("compact-appearance-de");
    f.dialog.key({.keysym = XKB_KEY_Tab, .text = {}});  // Search.
    f.dialog.key({.keysym = XKB_KEY_Tab, .text = {}});  // Palette source.
    f.dialog.key({.keysym = XKB_KEY_Tab, .text = {}});  // Reset palette source.
    f.dialog.key({.keysym = XKB_KEY_Tab, .text = {}});  // Colour mode.
    f.dialog.key({.keysym = XKB_KEY_Return, .text = {}});
    draw("compact-choice-de");
    f.dialog.key({.keysym = XKB_KEY_Escape, .text = {}});
    REQUIRE(f.dialog.is_open());
    f.dialog.key({.keysym = XKB_KEY_Tab, .text = {}});  // Reset colour mode.
    f.dialog.key({.keysym = XKB_KEY_Tab, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_Return, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_Home, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_Down, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_Return, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_Tab, .text = {}});
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    CHECK(core::load_config(f.path).config.icon.theme == "builtin");
    REQUIRE(f.dialog.edit("font", "size", "invalid"));
    f.dialog.close();
    REQUIRE(f.dialog.is_open());
    draw("compact-unsaved-de");
}

TEST_CASE("dialogs: confirmation buttons participate in keyboard focus") {
    views::ConfirmDialog dialog{[] {}};
    bool confirmed = true;
    dialog.open("Remove file?", "A file", "Remove", [&](bool answer) { confirmed = answer; });
    dialog.key({.keysym = XKB_KEY_Tab, .text = {}});
    dialog.key({.keysym = XKB_KEY_Return, .text = {}});
    CHECK_FALSE(confirmed);
    CHECK_FALSE(dialog.is_open());
    std::optional<std::string> password;
    dialog.open_password("archive.zip", false, [&](auto answer) { password = std::move(answer); });
    dialog.paste("secret");
    dialog.key({.keysym = XKB_KEY_ISO_Left_Tab, .text = {}});
    dialog.key({.keysym = XKB_KEY_Return, .text = {}});
    CHECK(password == "secret");
}

TEST_CASE("settings: source selection saves independently of colour mode") {
    Fixture f;
    f.open();
    f.dialog.key({.keysym = XKB_KEY_2, .text = {}, .modifiers = {.alt = true}});
    // First field in Appearance: source. Select Desktop with the keyboard.
    f.focus("appearance.palette_source");
    f.dialog.key({.keysym = XKB_KEY_Return, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_Down, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_Return, .text = {}});
    REQUIRE(f.dialog.edit("appearance", "color_scheme", "dark"));
    f.dialog.save();
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    REQUIRE(f.dialog.error().empty());
    const auto loaded = core::load_config(f.path);
    CHECK(loaded.config.appearance.palette_source == "desktop");
    CHECK(loaded.config.appearance.color_scheme == "dark");
}

TEST_CASE("settings: active palette reports applied state, live updates and source fallback") {
    Fixture f;
    using Theme = rummage::theme::Theme;
    using Source = Theme::Source;
    Theme theme{f.dir.path / "colors.toml"};
    f.dialog.set_theme_status_handler([&] { return theme.status(); });
    f.open();
    CHECK(f.dialog.palette_status() == "Active palette: Built-in");
    REQUIRE(f.dialog.edit("appearance", "palette_source", "file"));
    CHECK(f.dialog.palette_status() == "Active palette: Built-in");  // unsaved
    theme.set_source(Source::file);
    CHECK(f.dialog.palette_status() == "Active palette: Built-in (palette file unavailable)");
    f.dir.write("colors.toml", "mode = 'light'\n[colors]\n");
    theme.reload();
    CHECK(f.dialog.palette_status() == "Active palette: Palette file");
    theme.set_source(Source::desktop);
    CHECK(f.dialog.palette_status() == "Active palette: Built-in (desktop unavailable)");
    theme.set_desktop(rummage::theme::Desktop{.dark = true, .accent = std::nullopt});
    CHECK(f.dialog.palette_status() == "Active palette: Desktop");
    theme.set_dark_override(false);
    CHECK(f.dialog.palette_status() == "Active palette: Desktop · light override");
    theme.set_dark_override(true);
    CHECK(f.dialog.palette_status() == "Active palette: Desktop · dark override");
    theme.set_dark_override(std::nullopt);
    theme.set_source(Source::automatic);
    CHECK(f.dialog.palette_status() == "Active palette: Palette file");
    theme.set_source(Source::kde);
    CHECK(f.dialog.palette_status() == "Active palette: Built-in (KDE palette unavailable)");
    REQUIRE(f.dialog.edit("appearance", "palette_source", "kde"));
    f.dialog.save();
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    CHECK(core::load_config(f.path).config.appearance.palette_source == "kde");
}

TEST_CASE("settings: atomic replacement checks resolved directory rather than file mode") {
    Fixture f;
    const auto target_dir = f.dir.path / "declared";
    std::filesystem::create_directories(target_dir);
    const auto target = target_dir / "settings.toml";
    REQUIRE_FALSE(core::write_file_atomically(target, "[font]\nsize=16\n"));
    std::filesystem::create_symlink(target, f.path);
    std::filesystem::permissions(target, std::filesystem::perms::owner_read);
    CHECK(core::atomic_write_target(f.path).error.empty());
    f.open();
    CHECK(f.dialog.editable());
    REQUIRE(f.dialog.edit("font", "size", "17"));
    f.dialog.save();
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    CHECK(std::filesystem::is_symlink(f.path));
    CHECK(core::load_config(target).config.font.size == 17);
    std::filesystem::permissions(
        target_dir, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
    CHECK_FALSE(f.dialog.edit("font", "size", "18"));
    CHECK_FALSE(f.dialog.editable());
    CHECK_FALSE(f.dialog.read_only_reason().empty());
    CHECK_FALSE(f.dialog.reset_setting("font", "size"));
    CHECK_FALSE(f.dialog.apply_edits({{"font", "size", "19"}}).empty());
    CHECK(core::load_config(target).config.font.size == 17);
    std::filesystem::permissions(target_dir, std::filesystem::perms::owner_all);
    REQUIRE(f.dialog.edit("font", "size", "18"));
    f.dialog.save();
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    CHECK(core::load_config(target).config.font.size == 18);
}

TEST_CASE("settings: read-only configuration permits search and prevents all edits") {
    Fixture f;
    REQUIRE_FALSE(core::write_file_atomically(f.path, "[font]\nsize=16\n"));
    std::filesystem::permissions(
        f.dir.path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
    f.open();
    CHECK_FALSE(f.dialog.editable());
    f.dialog.search("interface text");
    REQUIRE(f.dialog.search_results().size() == 1);
    f.dialog.jump_to_result(f.dialog.search_results().front().field);
    f.dialog.key({.keysym = XKB_KEY_a, .text = {}, .modifiers = {.ctrl = true}});
    CHECK(f.dialog.copy_selection(true) == "16");
    f.dialog.paste("23");
    f.dialog.key({.keysym = XKB_KEY_BackSpace, .text = {}});
    f.dialog.save();
    CHECK(core::load_config(f.path).config.font.size == 16);
    CHECK(f.applied == 0);
    f.dialog.close();
    CHECK_FALSE(f.dialog.is_open());
    std::filesystem::permissions(f.dir.path, std::filesystem::perms::owner_all);
}

TEST_CASE("settings: search finds descriptions across categories and jumps to editing") {
    Fixture f;
    f.open();
    f.dialog.search("RESTART required");
    const auto results = f.dialog.search_results();
    REQUIRE(results.size() == 2);
    CHECK(results.front().section == 1);
    f.dialog.jump_to_result(results.front().field);
    f.dialog.key({.keysym = XKB_KEY_a, .text = {}, .modifiers = {.ctrl = true}});
    f.dialog.paste("Test Font");
    f.dialog.save();
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    CHECK(core::load_config(f.path).config.font.family == "Test Font");
    f.dialog.search("missing setting name");
    CHECK(f.dialog.search_results().empty());
}

TEST_CASE("settings: scalar resets restore parser defaults and preserve unrelated drafts") {
    Fixture f;
    REQUIRE_FALSE(
        core::write_file_atomically(f.path,
                                    "# custom\n[font]\nsize=22\n[terminal]\nprogram='foot'\n"
                                    "[animations.events]\nfile_copy='none'\n[[actions]]\nid='hello'"
                                    "\nlabel='Hello'\nargv=['echo']\n"));
    f.open();
    REQUIRE(f.dialog.edit("terminal", "program", "kitty"));
    REQUIRE(f.dialog.edit("font", "size", "invalid"));
    REQUIRE(f.dialog.reset_setting("font", "size"));
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    CHECK(core::load_config(f.path).config.font.size == core::Config{}.font.size);
    f.dialog.save();
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    CHECK(core::load_config(f.path).config.terminal.program == "kitty");
    REQUIRE(f.dialog.reset_category(1));
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    CHECK(core::load_config(f.path).config.actions.size() == 1);
    CHECK(core::read_text_file(f.path).text.find("file_copy='none'") != std::string::npos);
    CHECK_FALSE(f.dialog.reset_category(3));
}

TEST_CASE("settings: shared editor validates and serializes accepted configuration") {
    Fixture f;
    f.open();
    REQUIRE(f.dialog.apply_edits({{"font", "size", "18"}}).empty());
    CHECK(f.dialog.accepted_config().config.font.size == 18);
    CHECK_FALSE(f.dialog.apply_edits({{"font", "size", "'bad'"}}).empty());
    CHECK(f.dialog.accepted_config().config.font.size == 18);
    REQUIRE(f.dialog.apply_edits({{"terminal", "program", "'kitty'"}}).empty());
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    CHECK(core::load_config(f.path).config.font.size == 18);
    CHECK(core::load_config(f.path).config.terminal.program == "kitty");
}

TEST_CASE("settings: palette status distinguishes rejection and retained last valid colours") {
    Fixture f;
    using Theme = rummage::theme::Theme;
    Theme theme{f.dir.path / "colors.toml"};
    f.dialog.set_theme_status_handler([&] { return theme.status(); });
    f.open();
    f.dir.write("colors.toml", "mode = 'light'\n[colors]\nprimary = '#336699'\n");
    theme.reload();
    CHECK(f.dialog.palette_status().find("Palette file") != std::string::npos);
    f.dir.write("colors.toml", "[colors\n");
    theme.reload();
    CHECK(f.dialog.palette_status().find("reload failed; last valid palette") != std::string::npos);
    Theme rejected{f.dir.path / "colors.toml"};
    f.dialog.set_theme_status_handler([&] { return rejected.status(); });
    CHECK(f.dialog.palette_status().find("palette file rejected") != std::string::npos);
}

TEST_CASE("settings: global search includes menu contexts and action labels") {
    Fixture f;
    REQUIRE_FALSE(core::write_file_atomically(
        f.path, "[[actions]]\nid='greeting'\nlabel='Say hello'\nargv=['echo','hello']\n"));
    f.open();
    f.dialog.search("separators");
    const auto menus = f.dialog.search_results();
    REQUIRE(menus.size() == 5);
    CHECK(menus.front().section == 4);
    f.dialog.jump_to_result(menus.front().field);
    f.dialog.search("Say hello");
    const auto actions = f.dialog.search_results();
    REQUIRE_FALSE(actions.empty());
    CHECK(actions.back().section == 3);
    f.dialog.jump_to_result(actions.back().field);
    f.dialog.close();
    CHECK_FALSE(f.dialog.is_open());
    CHECK(f.applied == 0);
}

TEST_CASE("settings: embedded editors return keyboard focus to host and dismiss popups first") {
    Fixture f;
    f.open();
    namespace ui = rummage::ui;
    ui::Offscreen gl{800, 700};
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    ui::IconSet icons{fonts};
    const auto draw = [&] {
        renderer.begin_frame(800, 700, 1, {});
        f.dialog.draw(renderer, {0, 0, 800, 700}, rummage::theme::Palette{}, icons);
        renderer.end_frame();
    };
    for (const auto category : {XKB_KEY_3, XKB_KEY_4, XKB_KEY_5}) {
        f.dialog.key({.keysym = static_cast<std::uint32_t>(category),
                      .text = {},
                      .modifiers = {.alt = true}});
        draw();
        f.dialog.key({.keysym = XKB_KEY_f, .text = {}, .modifiers = {.ctrl = true}});
        f.dialog.key({.keysym = XKB_KEY_Tab, .text = {}});
        draw();
        f.dialog.key({.keysym = XKB_KEY_ISO_Left_Tab, .text = {}, .modifiers = {.shift = true}});
        f.dialog.paste("restart");
        f.dialog.key({.keysym = XKB_KEY_a, .text = {}, .modifiers = {.ctrl = true}});
        CHECK(f.dialog.copy_selection(false) == "restart");
        f.dialog.key({.keysym = XKB_KEY_BackSpace, .text = {}});
    }
    draw();
    f.dialog.key({.keysym = XKB_KEY_Tab, .text = {}});
    draw();
    f.dialog.key({.keysym = XKB_KEY_Return, .text = {}});
    draw();
    f.dialog.pointer({.kind = ui::PointerEvent::Kind::press,
                      .x = 700,
                      .y = 648,
                      .button = ui::PointerButton::left});
    CHECK(f.dialog.is_open());
    f.dialog.close();
    CHECK_FALSE(f.dialog.is_open());
}

TEST_CASE("settings: custom action recording receives host shortcuts and Escape cancels") {
    Fixture f;
    REQUIRE_FALSE(
        core::write_file_atomically(f.path,
                                    "[keybindings]\nselect_pattern=''\n[[actions]]\nid='greeting'"
                                    "\nlabel='Say hello'\nargv=['echo','hello']\n"));
    f.open();
    namespace ui = rummage::ui;
    ui::Offscreen gl{800, 700};
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    ui::IconSet icons{fonts};
    const auto draw = [&] {
        renderer.begin_frame(800, 700, 1, {});
        f.dialog.draw(renderer, {0, 0, 800, 700}, rummage::theme::Palette{}, icons);
        renderer.end_frame();
    };
    f.dialog.search("Say hello");
    const auto matches = f.dialog.search_results();
    REQUIRE_FALSE(matches.empty());
    f.dialog.jump_to_result(matches.back().field);
    draw();  // First component control: Back to actions.
    for (int i = 0; i < 9; ++i) {
        f.dialog.key({.keysym = XKB_KEY_Tab, .text = {}});
        draw();
    }
    f.dialog.key({.keysym = XKB_KEY_Return, .text = {}});  // Record shortcut.
    draw();
    f.dialog.key({.keysym = XKB_KEY_Escape, .text = {}});  // Cancel, don't close Settings.
    CHECK(f.dialog.is_open());
    CHECK(f.applied == 0);
    draw();
    f.dialog.key({.keysym = XKB_KEY_Return, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_s, .text = {}, .modifiers = {.ctrl = true}});
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    const auto config = core::load_config(f.path);
    REQUIRE(config.config.keybindings.contains("action:greeting"));
    CHECK(config.config.keybindings.at("action:greeting") == "Ctrl+s");
    draw();
    f.dialog.key({.keysym = XKB_KEY_Return, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_f, .text = {}, .modifiers = {.ctrl = true}});
    CHECK_FALSE(f.dialog.error().empty());  // Conflict is reported, not global search.
    CHECK(core::load_config(f.path).config.keybindings.at("action:greeting") == "Ctrl+s");
}

TEST_CASE("settings: action search only offers forms that can be opened") {
    Fixture f;
    f.open();
    for (const auto* query : {"copy_location", "copy_name", "extract_here", "extract_to"}) {
        f.dialog.search(query);
        for (const auto& match : f.dialog.search_results())
            CHECK(match.section != 3);
    }
}

TEST_CASE("settings: KDE diagnostics describe the selected source") {
    Fixture f;
    using Theme = rummage::theme::Theme;
    Theme::Status status{Theme::Source::kde, Theme::Source::builtin, std::nullopt};
    status.file_state = Theme::FileState::invalid;
    status.file_problems = {"unrelated colors.toml problem"};
    f.dialog.set_theme_status_handler([&] { return status; });
    f.open();
    CHECK(f.dialog.palette_status() == "Active palette: Built-in (KDE palette unavailable)");
    status.kde_state = Theme::FileState::invalid;
    status.kde_problems = {"kdeglobals: incomplete colour groups"};
    CHECK(f.dialog.palette_status().find("palette file rejected") != std::string::npos);
    CHECK(status.problems() == status.kde_problems);
    status.active = Theme::Source::kde;
    status.retaining_last_valid_kde = true;
    CHECK(f.dialog.palette_status() == "Active palette: KDE (reload failed; last valid palette)");
    status.dark_override = false;
    CHECK(f.dialog.palette_status().ends_with("light override"));
}

TEST_CASE("settings: effects preview respects reduced motion and saves shader controls") {
    Fixture f;
    f.open();
    REQUIRE(f.dialog.edit("animations", "hologram_spread", "5"));
    REQUIRE(f.dialog.edit("animations", "portal_rotation", "4"));
    REQUIRE(f.dialog.edit("animations.ambient", "background", "true"));
    REQUIRE(f.dialog.edit("animations.ambient", "selection_glow", "true"));
    REQUIRE(f.dialog.edit("animations.ambient", "fps", "20"));
    REQUIRE(f.dialog.edit("animations.ambient", "shader", "/tmp/example.frag"));
    f.dialog.save();
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    const auto config = core::load_config(f.path);
    REQUIRE(config.problems.empty());
    REQUIRE_MESSAGE(f.dialog.error().empty(), f.dialog.error());
    CHECK(config.config.animations.hologram_spread == 5);
    CHECK(config.config.animations.portal_rotation == 4);
    CHECK(config.config.animations.ambient.selection_glow);
    CHECK(config.config.animations.ambient.shader == "/tmp/example.frag");
    f.dialog.preview_effect(core::MotionEvent::file_copy);
    CHECK(f.dialog.previewing());
    CHECK(f.dialog.animating());
    CHECK_FALSE(f.dialog.ambient_interval());
    f.dialog.select_effect_slot(core::kMotionEvents.size());
    REQUIRE(f.dialog.ambient_interval());
    CHECK(*f.dialog.ambient_interval() == 50ms);
    f.dialog.set_system_reduced(true);
    CHECK_FALSE(f.dialog.animating());
    CHECK_FALSE(f.dialog.ambient_interval());
    f.dialog.preview_effect(core::MotionEvent::file_move);
    CHECK_FALSE(f.dialog.animating());
    f.dialog.close();
    CHECK_FALSE(f.dialog.previewing());
    CHECK_FALSE(f.dialog.ambient_interval());
}

TEST_CASE("settings: nested motion edits preserve inline, dotted and explicit tables") {
    for (const auto original : {"# keep\n[animations]\nenabled = true\n",
                                "# keep\n[animations.events]\nfile_copy = 'fade'\n",
                                "# keep\nanimations = { enabled = true }\n",
                                "# keep\nanimations = { ambient = { background = false } }\n",
                                "# keep\nanimations.ambient.background = false\n",
                                "# keep\n[animations.ambient]\nbackground = false\n"}) {
        CAPTURE(original);
        const auto edited =
            core::edit_config_text(original, {{"animations.ambient", "selection_glow", "true"},
                                              {"animations.ambient", "background", "true"},
                                              {"animations.events", "file_move", "'fade'"},
                                              {"animations.durations", "file_move", "800"}});
        REQUIRE_MESSAGE(edited.error.empty(), edited.error);
        CHECK(edited.text.find("# keep") != std::string::npos);
        const auto parsed = core::parse_config(edited.text, "test");
        REQUIRE(parsed.problems.empty());
        CHECK(parsed.config.animations.ambient.background);
        CHECK(parsed.config.animations.ambient.selection_glow);
        CHECK(parsed.config.animations.events[4] == core::MotionEffect::fade);
        CHECK(parsed.config.animations.durations_ms[4] == 800);
    }
}

TEST_CASE("settings: every effect slot previews live drafts and persists its own mapping") {
    Fixture f;
    f.open();
    for (std::size_t slot = 0; slot < core::kMotionEvents.size(); ++slot) {
        CAPTURE(slot);
        const auto name = core::kMotionEvents[slot];
        f.dialog.select_effect_slot(slot);
        CHECK(f.dialog.effect_slot() == slot);
        CHECK(f.dialog.previewing());
        REQUIRE(f.dialog.edit("animations.events", name, "slide"));
        REQUIRE(f.dialog.edit("animations.durations", name, "800"));
        const auto frame = f.dialog.preview_frame();
        REQUIRE(frame);
        CHECK(frame->effect == core::MotionEffect::slide);
        CHECK_FALSE(f.dialog.ambient_interval());
    }
    f.dialog.save();
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    REQUIRE(f.dialog.error().empty());
    const auto parsed = core::load_config(f.path);
    REQUIRE(parsed.problems.empty());
    for (std::size_t slot = 0; slot < core::kMotionEvents.size(); ++slot) {
        CHECK(parsed.config.animations.events[slot] == core::MotionEffect::slide);
        CHECK(parsed.config.animations.durations_ms[slot] == 800);
    }
    f.dialog.key({.keysym = XKB_KEY_2, .text = {}, .modifiers = {.alt = true}});
    CHECK_FALSE(f.dialog.previewing());
    CHECK_FALSE(f.dialog.animating());
    f.dialog.key({.keysym = XKB_KEY_6, .text = {}, .modifiers = {.alt = true}});
    CHECK(f.dialog.previewing());
    CHECK(f.dialog.animating());
    f.dialog.search("Copy files");
    const auto results = f.dialog.search_results();
    REQUIRE(results.size() == 2);
    CHECK(results[0].label == "Copy files · Effect");
    CHECK(results[1].label == "Copy files · Duration");
    f.dialog.jump_to_result(results[0].field);
    CHECK(f.dialog.effect_slot() == static_cast<std::size_t>(core::MotionEvent::file_copy));
}

TEST_CASE("settings: effect parameter typing refreshes before commit and ignores invalid drafts") {
    Fixture f;
    f.open();
    f.focus("animations.hologram_spread");
    CHECK(f.dialog.effect_slot() == static_cast<std::size_t>(core::MotionEvent::file_copy));
    f.dialog.key({.keysym = XKB_KEY_a, .text = {}, .modifiers = {.ctrl = true}});
    f.dialog.paste("7");
    REQUIRE(f.dialog.preview_frame());
    CHECK(f.dialog.preview_frame()->hologram_spread == 7);
    CHECK(f.dialog.accepted_config().config.animations.hologram_spread == 2);
    f.dialog.key({.keysym = XKB_KEY_a, .text = {}, .modifiers = {.ctrl = true}});
    f.dialog.paste("-");
    REQUIRE(f.dialog.preview_frame());
    CHECK(f.dialog.preview_frame()->hologram_spread == 7);
    f.dialog.key({.keysym = XKB_KEY_a, .text = {}, .modifiers = {.ctrl = true}});
    f.dialog.paste("4");
    REQUIRE(f.dialog.preview_frame());
    CHECK(f.dialog.preview_frame()->hologram_spread == 4);
    f.dialog.key({.keysym = XKB_KEY_Return, .text = {}});
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    CHECK(core::load_config(f.path).config.animations.hologram_spread == 4);
    f.focus("animations.events.search_results");
    CHECK(f.dialog.effect_slot() == static_cast<std::size_t>(core::MotionEvent::search_results));
    REQUIRE(f.dialog.preview_frame());
    CHECK(f.dialog.preview_frame()->effect == core::MotionEffect::scan);
}

TEST_CASE(
    "settings: slot selector reaches persistent effects and effect dropdown replays immediately") {
    Fixture f;
    f.open();
    namespace ui = rummage::ui;
    ui::Offscreen gl{900, 750};
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    ui::IconSet icons{fonts};
    const auto draw = [&] {
        renderer.begin_frame(900, 750, 1, {});
        f.dialog.draw(renderer, {0, 0, 900, 750}, rummage::theme::Palette{}, icons);
        renderer.end_frame();
    };
    f.dialog.select_effect_slot(0);
    draw();
    f.dialog.key({.keysym = XKB_KEY_Return, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_End, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_Return, .text = {}});
    CHECK(f.dialog.effect_slot() == core::kMotionEvents.size() + 1);
    CHECK_FALSE(f.dialog.animating());
    REQUIRE(f.dialog.edit("animations.ambient", "background", "true"));
    CHECK_FALSE(f.dialog.ambient_interval());
    REQUIRE(f.dialog.edit("animations.ambient", "selection_glow", "true"));
    CHECK(f.dialog.ambient_interval());
    f.focus("animations.events.directory_open");
    draw();
    f.dialog.key({.keysym = XKB_KEY_Return, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_End, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_Return, .text = {}});
    REQUIRE(f.dialog.preview_frame());
    CHECK(f.dialog.preview_frame()->effect == core::MotionEffect::scan);
    CHECK_FALSE(f.dialog.ambient_interval());
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
}

TEST_CASE("settings: translated effects stay searchable and save stable effect identifiers") {
    namespace ui = rummage::ui;
    namespace i18n = rummage::i18n;

    struct RestoreLanguage {
        ~RestoreLanguage() { i18n::set_language("en"); }
    } restore;

    ui::Offscreen surface{1000, 800};
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    ui::IconSet icons{fonts};
    for (const auto language : {"de", "es", "fr", "ja", "pt_BR", "ru", "zh_CN"}) {
        CAPTURE(language);
        i18n::set_language(language);
        Fixture f;
        f.open();
        f.dialog.select_effect_slot(static_cast<std::size_t>(core::MotionEvent::file_copy));
        f.dialog.set_system_reduced(true);
        const auto draw = [&](const std::string& name) {
            renderer.begin_frame(1000, 800, 1, {});
            f.dialog.draw(renderer, {0, 0, 1000, 800}, rummage::theme::Palette{}, icons);
            renderer.end_frame();
            if (const char* output = std::getenv("RUMMAGE_SETTINGS_FRAMES")) {
                std::vector<unsigned char> pixels(1000 * 800 * 4);
                glReadPixels(0, 0, 1000, 800, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
                std::ofstream file{std::filesystem::path{output} / (name + "-" + language + ".ppm"),
                                   std::ios::binary};
                file << "P6\n1000 800\n255\n";
                for (int y = 799; y >= 0; --y)
                    for (int x = 0; x < 1000; ++x)
                        file.write(
                            reinterpret_cast<const char*>(pixels.data() + (y * 1000 + x) * 4), 3);
            }
            CHECK(glGetError() == GL_NO_ERROR);
        };
        draw("effects");
        CHECK(i18n::tr("Hologram") != "Hologram");
        const auto description = i18n::tr("Choose the animation for this slot");
        CHECK(description != "Choose the animation for this slot");
        f.dialog.search(std::string{description});
        const auto results = f.dialog.search_results();
        REQUIRE(results.size() == core::kMotionEvents.size());
        CHECK(results.front().label.starts_with(i18n::tr("Open directory")));
        f.dialog.jump_to_result(
            results[static_cast<std::size_t>(core::MotionEvent::file_copy)].field);
        draw("effects-focused");
        f.dialog.key({.keysym = XKB_KEY_Return, .text = {}});
        draw("effects-choices");
        f.dialog.key({.keysym = XKB_KEY_End, .text = {}});
        f.dialog.key({.keysym = XKB_KEY_Return, .text = {}});
        REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
        CHECK(core::load_config(f.path).config.animations.events[static_cast<std::size_t>(
                  core::MotionEvent::file_copy)] == core::MotionEffect::scan);
    }
}

TEST_CASE("settings: translated rows grow without overlapping text and controls") {
    namespace ui = rummage::ui;
    ui::Offscreen surface{1000, 800};
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    const std::string label = "Ausführbare Programme starten";
    const std::string description = "Anwendungen aus der Dateiansicht öffnen erlauben";
    const std::string choice = "Tabs und Bereiche wiederherstellen";
    for (const float scale : {1.0F, 1.5F, 2.0F}) {
        renderer.begin_frame(1000, 800, scale, {});
        const float choice_width = fonts.layout(choice, 13, scale).width / scale + 44;
        for (const float width : {220.0F, 340.0F, 500.0F, 648.0F}) {
            CAPTURE(scale);
            CAPTURE(width);
            const auto row = views::measure_setting_row(renderer, width, label, description,
                                                        choice_width, choice);
            CHECK(row.label.bottom() <= row.description.y);
            CHECK(row.description.bottom() < row.height);
            CHECK(row.control.bottom() < row.height);
            CHECK(row.control.right() <= width);
            CHECK(row.control.intersect(row.label).empty());
            CHECK(row.control.intersect(row.description).empty());
            if (width == 220) {
                CHECK(row.description.height > 16);
                CHECK(row.control.height > 32);
                CHECK(row.height > 94);
            }
            if (width == 648) {
                CHECK(row.control.x > row.label.right());
                CHECK(row.control.width >= choice_width);
            }
        }
        renderer.end_frame();
    }
}

TEST_CASE("settings: the full translated reset button is clickable") {
    namespace ui = rummage::ui;

    struct RestoreLanguage {
        ~RestoreLanguage() { rummage::i18n::set_language("en"); }
    } restore;

    SUBCASE("German") { rummage::i18n::set_language("de"); }
    SUBCASE("French") { rummage::i18n::set_language("fr"); }
    Fixture f;
    f.open();
    REQUIRE(f.dialog.edit("startup", "mode", "home"));
    f.dialog.save();
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    REQUIRE(core::load_config(f.path).config.startup.mode == core::Config::Startup::Mode::home);

    ui::Offscreen surface{960, 700};
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    ui::IconSet icons{fonts};
    renderer.begin_frame(960, 700, 1, {});
    f.dialog.draw(renderer, {0, 0, 960, 700}, rummage::theme::Palette{}, icons);
    const float reset_width =
        std::max(150.0F, fonts.layout(rummage::i18n::tr("Reset to default"), 13, 1).width + 24);
    renderer.end_frame();
    // The right edge remains clickable with both short and long translated labels.
    f.dialog.pointer({.kind = ui::PointerEvent::Kind::press,
                      .x = 244 + reset_width - 6,
                      .y = 238,
                      .button = ui::PointerButton::left});
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    CHECK(core::load_config(f.path).config.startup.mode == core::Config{}.startup.mode);
}

TEST_CASE("settings: existing shortcut conflicts do not block appearance edits or resets") {
    Fixture f;
    REQUIRE_FALSE(
        core::write_file_atomically(f.path,
                                    "[keybindings]\nnew_folder = 'Ctrl+n'\n[appearance]\n"
                                    "background_opacity = 0.8\ntransparent_settings = true\n"));
    f.open();
    const auto original = core::load_config(f.path);
    REQUIRE(original.validation_errors.empty());
    REQUIRE_FALSE(views::configure_shortcuts(original.config.keybindings, {}, false).empty());

    // Text-field commit must not attach a pre-existing shortcut error to Appearance.
    REQUIRE(f.dialog.edit("appearance", "background_opacity", "0.4"));
    REQUIRE(f.dialog.edit("appearance", "transparent_settings", "false"));
    f.dialog.save();
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    REQUIRE_MESSAGE(f.dialog.error().empty(), f.dialog.error());
    CHECK(core::load_config(f.path).config.appearance.background_opacity == 0.4F);
    CHECK_FALSE(core::load_config(f.path).config.appearance.transparent_settings);
    CHECK(core::load_config(f.path).config.keybindings.at("new_folder") == "Ctrl+n");
    if (const char* output = std::getenv("RUMMAGE_SETTINGS_FRAMES")) {
        namespace ui = rummage::ui;
        ui::Offscreen surface{960, 800};
        ui::FontSystem fonts;
        ui::Renderer renderer{fonts};
        ui::IconSet icons{fonts};
        for (const auto& [query, name] :
             {std::pair{"appearance.background_opacity", "inherited-conflict-appearance"},
              std::pair{"keybindings.new_folder", "inherited-conflict-keyboard"}}) {
            f.focus(query);
            renderer.begin_frame(960, 800, 1, {});
            f.dialog.draw(renderer, {0, 0, 960, 800}, rummage::theme::Palette{}, icons, true);
            renderer.end_frame();
            std::vector<unsigned char> pixels(960 * 800 * 4);
            glReadPixels(0, 0, 960, 800, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            std::ofstream file{std::filesystem::path{output} / (std::string{name} + ".ppm"),
                               std::ios::binary};
            file << "P6\n960 800\n255\n";
            for (int y = 799; y >= 0; --y)
                for (int x = 0; x < 960; ++x)
                    file.write(reinterpret_cast<const char*>(pixels.data() + (y * 960 + x) * 4), 3);
        }
    }

    // The logical-edit and reset paths must follow the same validation policy.
    REQUIRE(f.dialog.apply_edits({{"appearance", "transparent_settings", "true"}}).empty());
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    CHECK(core::load_config(f.path).config.appearance.transparent_settings);
    REQUIRE(f.dialog.reset_setting("appearance", "background_opacity"));
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    REQUIRE(f.dialog.reset_setting("appearance", "transparent_settings"));
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    CHECK(core::load_config(f.path).config.appearance.background_opacity == 1.0F);
    CHECK_FALSE(core::load_config(f.path).config.appearance.transparent_settings);
    REQUIRE(f.dialog.error().empty());
    f.dialog.close();
    REQUIRE_FALSE(f.dialog.is_open());
    f.open();

    // A malformed known value still fails even though shortcut validation is unrelated.
    CHECK_FALSE(f.dialog.apply_edits({{"appearance", "background_opacity", "'invalid'"}}).empty());
    CHECK(core::load_config(f.path).config.appearance.background_opacity == 1.0F);

    // Editing shortcuts still rejects conflicts, then permits correcting the old one.
    CHECK_FALSE(f.dialog.apply_edits({{"keybindings", "new_folder", "'Ctrl+q'"}}).empty());
    CHECK(core::load_config(f.path).config.keybindings.at("new_folder") == "Ctrl+n");
    REQUIRE(f.dialog.edit("keybindings", "new_folder", "Ctrl+q"));
    f.dialog.save();
    CHECK(f.dialog.error().find("conflicts") != std::string::npos);
    CHECK(core::load_config(f.path).config.keybindings.at("new_folder") == "Ctrl+n");
    REQUIRE(f.dialog.edit("keybindings", "new_folder", "Ctrl+Alt+n"));
    f.dialog.save();
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    REQUIRE_MESSAGE(f.dialog.error().empty(), f.dialog.error());
    CHECK(core::load_config(f.path).config.keybindings.at("new_folder") == "Ctrl+Alt+n");
    REQUIRE(views::configure_shortcuts(core::load_config(f.path).config.keybindings, {}, false)
                .empty());
}

TEST_CASE("settings: transparency can be toggled, persisted, and reset") {
    Fixture f;
    f.open();
    f.focus("Transparent Settings");
    f.dialog.key({.keysym = XKB_KEY_space, .text = {}});
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    CHECK(core::load_config(f.path).config.appearance.transparent_settings);
    REQUIRE(f.dialog.reset_setting("appearance", "transparent_settings"));
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    CHECK_FALSE(core::load_config(f.path).config.appearance.transparent_settings);
}

TEST_CASE("settings: separate window fills resized content and preserves background alpha") {
    namespace ui = rummage::ui;
    Fixture f;
    f.open();
    ui::Offscreen surface{1200, 1000};
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    ui::IconSet icons{fonts};
    const rummage::theme::Palette palette;
    for (const auto size :
         {ui::Rect{0, 0, 880, 740}, ui::Rect{0, 0, 1200, 1000}, ui::Rect{0, 0, 400, 360}}) {
        const int width = static_cast<int>(size.width), height = static_cast<int>(size.height);
        for (const float opacity : {1.0F, 0.4F, 0.0F}) {
            renderer.begin_frame(width, height, 1, palette.surface, opacity);
            f.dialog.draw(renderer, size, palette, icons, true);
            renderer.end_frame();
            std::vector<unsigned char> pixels(static_cast<std::size_t>(width * height * 4));
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            // All four corners belong to Settings, without a surrounding scrim or margin.
            for (const auto point : {std::pair{2, 2}, std::pair{width - 3, 2},
                                     std::pair{2, height - 3}, std::pair{width - 3, height - 3}}) {
                const auto offset =
                    static_cast<std::size_t>((point.second * width + point.first) * 4);
                CHECK(pixels[offset + 3] == doctest::Approx(opacity * 255).epsilon(0.01));
                CHECK(pixels[offset] ==
                      doctest::Approx(palette.surface_container_high.r * opacity * 255)
                          .epsilon(0.06));
            }
            // The Close button stays solid, even with a fully transparent background.
            const auto button =
                static_cast<std::size_t>((height - 1 - (height - 28)) * width + width - 50) * 4;
            CHECK(pixels[button + 3] == 255);
            if (const char* output = std::getenv("RUMMAGE_SETTINGS_FRAMES")) {
                const auto name =
                    "window-" + std::to_string(width) + "-" + std::to_string(opacity) + ".ppm";
                std::ofstream file{std::filesystem::path{output} / name, std::ios::binary};
                file << "P6\n" << width << ' ' << height << "\n255\n";
                for (int y = height - 1; y >= 0; --y)
                    for (int x = 0; x < width; ++x)
                        file.write(
                            reinterpret_cast<const char*>(pixels.data() + (y * width + x) * 4), 3);
            }
        }
    }
}
