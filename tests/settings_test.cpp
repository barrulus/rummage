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
    const auto draw = [&] {
        renderer.begin_frame(400, 360, 1, {});
        f.dialog.draw(renderer, {0, 0, 400, 360}, rummage::theme::Palette{});
        renderer.end_frame();
    };
    draw();
    f.dialog.pointer(
        {.kind = rummage::ui::PointerEvent::Kind::scroll, .x = 100, .y = 160, .wheel_y = 20});
    draw();
    f.dialog.pointer({.kind = rummage::ui::PointerEvent::Kind::press,
                      .x = 100,
                      .y = 160,
                      .button = rummage::ui::PointerButton::left});
    f.dialog.key({.keysym = XKB_KEY_a, .text = {}, .modifiers = {.ctrl = true}});
    CHECK(f.dialog.copy_selection(false) == "monospace");
    f.dialog.pointer(
        {.kind = rummage::ui::PointerEvent::Kind::scroll, .x = 100, .y = 160, .scroll_y = -10000});
    draw();
    CHECK(f.dialog.cursor_at(100, 160) == rummage::ui::CursorShape::pointer);
}
