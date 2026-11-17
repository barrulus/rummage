#include <chrono>
#include <functional>
#include <string>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "rummage/core/config_edit.hpp"
#include "rummage/core/file.hpp"
#include "rummage/ui/offscreen.hpp"
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

TEST_CASE("settings: save uses the selected file and Cancel leaves it unchanged") {
    Fixture f;
    const std::string original = "# preserved\n[icon]\ntheme = 'system'\n";
    REQUIRE_FALSE(core::write_file_atomically(f.path, original));
    f.open();
    REQUIRE(f.dialog.edit("icon", "theme", "builtin"));
    REQUIRE(f.dialog.edit("appearance", "palette_source", "desktop"));
    f.dialog.close();
    CHECK(core::read_text_file(f.path).text == original);
    f.open();
    REQUIRE(f.dialog.edit("appearance", "preview_position", "sidebar"));
    f.dialog.save();
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    REQUIRE_MESSAGE(f.dialog.error().empty(), f.dialog.error());
    CHECK_FALSE(f.dialog.is_open());
    CHECK(f.applied == 1);
    CHECK(core::load_config(f.path).config.appearance.preview_position == "sidebar");
    CHECK(core::read_text_file(f.path).text.starts_with(original));
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

TEST_CASE("settings: mouse wheel and keyboard reach fields in a short window") {
    Fixture f;
    f.open();
    rummage::ui::Offscreen gl{400, 360};
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    const auto draw = [&] {
        renderer.begin_frame(400, 360, 1, {});
        f.dialog.draw(renderer, {0, 0, 400, 360}, rummage::theme::Palette{}, icons);
        renderer.end_frame();
    };
    draw();
    f.dialog.pointer(
        {.kind = rummage::ui::PointerEvent::Kind::scroll, .x = 100, .y = 160, .wheel_y = 1000});
    draw();
    f.dialog.pointer({.kind = rummage::ui::PointerEvent::Kind::press,
                      .x = 100,
                      .y = 160,
                      .button = rummage::ui::PointerButton::left});
    f.dialog.key({.keysym = XKB_KEY_a, .text = {}, .modifiers = {.ctrl = true}});
    CHECK(f.dialog.copy_selection(false) == "[]");
    f.dialog.pointer(
        {.kind = rummage::ui::PointerEvent::Kind::scroll, .x = 100, .y = 160, .scroll_y = -10000});
    draw();
    CHECK(f.dialog.cursor_at(100, 160) == rummage::ui::CursorShape::pointer);
}

TEST_CASE("settings: scrollbar drags outside the list and releases capture") {
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
    CHECK(f.dialog.cursor_at(365, 135) == ui::CursorShape::pointer);
    f.dialog.pointer({.kind = ui::PointerEvent::Kind::press,
                      .x = 365,
                      .y = 135,
                      .button = ui::PointerButton::left});
    f.dialog.pointer({.kind = ui::PointerEvent::Kind::motion, .x = 500, .y = 500});
    f.dialog.pointer({.kind = ui::PointerEvent::Kind::release,
                      .x = 500,
                      .y = 500,
                      .button = ui::PointerButton::left});
    f.dialog.pointer({.kind = ui::PointerEvent::Kind::motion, .x = 365, .y = 120});
    draw();
    f.dialog.pointer({.kind = ui::PointerEvent::Kind::press,
                      .x = 100,
                      .y = 160,
                      .button = ui::PointerButton::left});
    f.dialog.key({.keysym = XKB_KEY_a, .text = {}, .modifiers = {.ctrl = true}});
    CHECK(f.dialog.copy_selection(false) == "[]");
    // Click the track above the thumb to return to the start.
    f.dialog.pointer({.kind = ui::PointerEvent::Kind::press,
                      .x = 365,
                      .y = 121,
                      .button = ui::PointerButton::left});
    f.dialog.pointer({.kind = ui::PointerEvent::Kind::release,
                      .x = 365,
                      .y = 121,
                      .button = ui::PointerButton::left});
    draw();
    CHECK(f.dialog.cursor_at(100, 160) == ui::CursorShape::pointer);
    // Switching sections cancels an in-progress drag.
    f.dialog.pointer({.kind = ui::PointerEvent::Kind::press,
                      .x = 365,
                      .y = 135,
                      .button = ui::PointerButton::left});
    f.dialog.key({.keysym = XKB_KEY_2, .text = {}, .modifiers = {.alt = true}});
    f.dialog.key({.keysym = XKB_KEY_1, .text = {}, .modifiers = {.alt = true}});
    f.dialog.pointer({.kind = ui::PointerEvent::Kind::motion, .x = 500, .y = 500});
    draw();
    CHECK(f.dialog.cursor_at(100, 160) == ui::CursorShape::pointer);
}

TEST_CASE(
    "settings: new keybinding button is keyboard accessible and saves action and folder bindings") {
    Fixture f;
    f.open();
    f.dialog.add_action();
    REQUIRE(f.dialog.edit("new:0", "id", "hello"));
    REQUIRE(f.dialog.edit("new:0", "argv", "['echo', '{file}']"));
    f.dialog.key({.keysym = XKB_KEY_2, .text = {}, .modifiers = {.alt = true}});
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
    draw();
    for (int i = 0; i < 5; ++i)
        f.dialog.key({.keysym = XKB_KEY_ISO_Left_Tab, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_Return, .text = {}});
    draw();
    f.dialog.paste("action:hello");
    // Both new fields are visible immediately, including the chord below the target.
    f.dialog.pointer({.kind = ui::PointerEvent::Kind::press,
                      .x = 100,
                      .y = 515,
                      .button = ui::PointerButton::left});
    f.dialog.paste("Ctrl+Alt+J");
    f.dialog.add_keybinding();
    REQUIRE(f.dialog.edit("binding:1", "target", "go:~/Projects"));
    REQUIRE(f.dialog.edit("binding:1", "chord", "Ctrl+Alt+Y"));
    f.dialog.save();
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    REQUIRE_MESSAGE(f.dialog.error().empty(), f.dialog.error());
    const auto config = core::load_config(f.path);
    REQUIRE(config.problems.empty());
    CHECK(config.config.keybindings.at("action:hello") == "Ctrl+Alt+J");
    CHECK(config.config.keybindings.at("go:~/Projects") == "Ctrl+Alt+Y");
    f.open();
    CHECK(f.dialog.edit("keybindings", "action:hello", "Default"));
    CHECK(f.dialog.edit("keybindings", "go:~/Projects", "Default"));
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
    f.dialog.close();
    CHECK(core::read_text_file(f.path).text == original);
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

TEST_CASE("settings: keyboard choices open a list and save the selected value") {
    Fixture f;
    f.open();
    for (int i = 0; i < 4; ++i)
        f.dialog.key({.keysym = XKB_KEY_Tab, .text = {}});
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

TEST_CASE("settings: choice cancellation and Tab preserve values and text editing") {
    Fixture f;
    f.open();
    for (int i = 0; i < 4; ++i)
        f.dialog.key({.keysym = XKB_KEY_Tab, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_Return, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_Down, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_Escape, .text = {}});
    CHECK(f.dialog.is_open());
    f.dialog.key({.keysym = XKB_KEY_Return, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_End, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_Tab, .text = {}});
    f.dialog.key({.keysym = XKB_KEY_a, .text = {}, .modifiers = {.ctrl = true}});
    f.dialog.paste("builtin");
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
    draw();
    click(100, 242);  // Colour mode, below palette source.
    draw();
    click(100, 339);  // Third menu choice: dark.
    draw();
    click(100, 242);
    draw();
    click(400, 640);  // Cancel button is behind the open menu: dismiss only.
    CHECK(f.dialog.is_open());
    draw();
    f.dialog.pointer({.kind = ui::PointerEvent::Kind::scroll, .x = 100, .y = 250, .scroll_y = 144});
    draw();
    click(100, 386);  // Follow palette accent: On/Off choices, scrolled into view.
    draw();
    click(100, 455);  // Off.
    f.dialog.save();
    REQUIRE(settle(f.loop, [&] { return !f.dialog.busy(); }));
    CHECK(f.dialog.error().empty());
    const auto config = core::load_config(f.path);
    CHECK(config.config.appearance.color_scheme == "dark");
    CHECK_FALSE(config.config.icon.follow_accent);
}

TEST_CASE("settings: source selection saves independently of colour mode") {
    Fixture f;
    f.open();
    // First field: source. Select Desktop with the keyboard.
    for (int i = 0; i < 3; ++i)
        f.dialog.key({.keysym = XKB_KEY_Tab, .text = {}});
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
}
