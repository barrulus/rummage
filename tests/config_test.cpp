#include "rummage/core/config.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "rummage/core/file.hpp"
#include "temp_dir.hpp"

using rummage::core::Config;
using rummage::core::ConfigResult;
using rummage::core::default_config_path;
using rummage::core::load_config;
using rummage::core::parse_config;

using rummage::test::TempDir;

TEST_CASE("rename: migrate Nordend settings without replacing files or symlinks") {
    TempDir dir;
    const auto old = dir.path / "nordend";
    const auto current = dir.path / "rummage";
    std::filesystem::create_directories(old);
    for (const auto* name : {"config.toml", "colors.toml", "state.toml"}) {
        const std::string content = std::string{"# original "} + name + "\n";
        REQUIRE_FALSE(rummage::core::write_file_atomically(old / name, content));
        CHECK(rummage::core::migrate_nordend_file(current / name).empty());
        CHECK(rummage::core::read_text_file(current / name).text == content);
        CHECK(rummage::core::read_text_file(old / name).text == content);
        REQUIRE_FALSE(rummage::core::write_file_atomically(current / name, "# new settings\n"));
        CHECK(rummage::core::migrate_nordend_file(current / name).empty());
        CHECK(rummage::core::read_text_file(current / name).text == "# new settings\n");
    }
    CHECK(rummage::core::migrate_nordend_file(current / "missing.toml").empty());
    CHECK_FALSE(std::filesystem::exists(current / "missing.toml"));
    std::filesystem::create_directory(old / "bad.toml");
    CHECK_FALSE(rummage::core::migrate_nordend_file(current / "bad.toml").empty());
    CHECK_FALSE(std::filesystem::exists(current / "bad.toml"));
    REQUIRE_FALSE(rummage::core::write_file_atomically(old / "link.toml", "# legacy\n"));
    std::filesystem::create_symlink(dir.path / "absent", current / "link.toml");
    CHECK(rummage::core::migrate_nordend_file(current / "link.toml").empty());
    CHECK(std::filesystem::is_symlink(current / "link.toml"));
    CHECK_FALSE(std::filesystem::exists(dir.path / "absent"));
    CHECK(rummage::core::migrate_nordend_file({}).empty());
}

TEST_CASE("context menu config: nested layouts and per-context overrides") {
    const auto result = parse_config(R"(
[context_menu]
items = [
  { id = "open", label = "Open this" },
  { label = "My Actions", items = [
    { label = "Scripts", items = [{ id = "action:script" }] },
    { label = "External", items = [{ id = "action:extract_here" }] },
  ] },
  { separator = true },
  { remaining = true },
]
entry = [{ id = "copy" }]
background = [{ id = "new_file" }]
trash = [{ id = "restore" }]
sidebar = []
[[actions]]
id = "script"
label = "Script"
argv = ["script", "{file}"]
)",
                                     "menu.toml");
    REQUIRE(result.problems.empty());
    CHECK(result.context_menu_valid);
    const auto& layout = result.config.context_menu;
    REQUIRE(layout.items);
    REQUIRE(layout.items->size() == 4);
    CHECK((*layout.items)[1].items[0].items[0].id == "action:script");
    CHECK((*layout.items)[2].separator);
    CHECK((*layout.items)[3].remaining);
    REQUIRE(layout.entry);
    CHECK(layout.entry->front().id == "copy");
    REQUIRE(layout.background);
    REQUIRE(layout.trash);
    REQUIRE(layout.sidebar);
    CHECK(layout.sidebar->empty());
}

TEST_CASE("context menu config: legacy sidebar defaults gain new actions without rewriting files") {
    std::string entries = R"(
  { id = "mount" },
  { id = "unmount" },
  { id = "eject", separator = true },
  { id = "remove_bookmark" },
  { id = "empty_trash" },
)";
    std::size_t count = 5;
    SUBCASE("original default") {}
    SUBCASE("default with bookmark editing") {
        entries = R"(
  { id = "rename_bookmark" },
  { id = "choose_bookmark_folder" },
  { id = "move_bookmark_up", separator = true },
  { id = "move_bookmark_down" },
  { id = "mount" },
  { id = "unmount" },
  { id = "eject", separator = true },
  { id = "remove_bookmark", separator = true },
  { id = "empty_trash" },
)";
        count = 9;
    }
    TempDir dir;
    const auto target = dir.path / "managed.toml";
    const auto path = dir.path / "config.toml";
    const std::string text = "[context_menu]\nsidebar = [" + entries +
                             "]\nentry = []\nbackground = [{id = 'new_folder'}]\n";
    REQUIRE_FALSE(rummage::core::write_file_atomically(target, text));
    std::filesystem::permissions(target, std::filesystem::perms::owner_read);
    std::filesystem::create_symlink(target, path);
    for (int reload = 0; reload < 2; ++reload) {
        const auto result = load_config(path);
        REQUIRE(result.problems.empty());
        REQUIRE(result.context_menu_valid);
        REQUIRE(result.config.context_menu.sidebar);
        const auto& sidebar = *result.config.context_menu.sidebar;
        REQUIRE(sidebar.size() == count + 1);
        CHECK(sidebar.front().remaining);
        CHECK(sidebar.back().id == "empty_trash");
        REQUIRE(result.config.context_menu.entry);
        CHECK(result.config.context_menu.entry->empty());
        REQUIRE(result.config.context_menu.background);
        CHECK(result.config.context_menu.background->front().id == "new_folder");
        CHECK(std::filesystem::is_symlink(path));
        CHECK(rummage::core::read_text_file(target).text == text);
    }
}

TEST_CASE("context menu config: customised sidebar layouts retain their exact filtering") {
    for (const std::string entries : {
             "",
             "{id='mount'}, {id='unmount'}, {id='eject',separator=true}, {id='remove_bookmark'}",
             "{id='unmount'}, {id='mount'}, {id='eject',separator=true}, "
             "{id='remove_bookmark'}, {id='empty_trash'}",
             "{id='mount',label='Connect'}, {id='unmount'}, {id='eject',separator=true}, "
             "{id='remove_bookmark'}, {id='empty_trash'}",
             "{id='mount'}, {id='unmount'}, {id='eject'}, {id='remove_bookmark'}, "
             "{id='empty_trash'}",
             "{label='Devices',items=[{id='mount'},{id='unmount'}]}, "
             "{id='eject',separator=true}, {id='remove_bookmark'}, {id='empty_trash'}",
             "{id='mount'}, {id='unmount'}, {id='eject',separator=true}, "
             "{id='remove_bookmark'}, {id='empty_trash'}, {id='properties'}",
             "{remaining=true}, {id='mount'}, {id='unmount'}, {id='eject',separator=true}, "
             "{id='remove_bookmark'}, {id='empty_trash'}",
         }) {
        CAPTURE(entries);
        const auto result = parse_config("[context_menu]\nsidebar=[" + entries + "]", "menu.toml");
        // The same tree in another context provides an unmigrated reference.
        const auto reference = parse_config("[context_menu]\nitems=[" + entries + "]", "menu.toml");
        REQUIRE(result.problems.empty());
        REQUIRE(reference.problems.empty());
        const auto equal = [](const auto& self, const auto& a, const auto& b) -> bool {
            if (a.size() != b.size())
                return false;
            for (std::size_t i = 0; i < a.size(); ++i) {
                if (a[i].id != b[i].id || a[i].label != b[i].label ||
                    a[i].separator != b[i].separator || a[i].remaining != b[i].remaining ||
                    !self(self, a[i].items, b[i].items))
                    return false;
            }
            return true;
        };
        REQUIRE(result.config.context_menu.sidebar);
        REQUIRE(reference.config.context_menu.items);
        CHECK(equal(equal, *result.config.context_menu.sidebar,
                    *reference.config.context_menu.items));
    }
}

TEST_CASE("context menu config: invalid trees reject the entire layout") {
    for (const auto* invalid : {
             "context_menu = 1",
             "[context_menu]\nunknown = []",
             "[context_menu]\nitems = 'open'",
             "[context_menu]\nitems = ['open']",
             "[context_menu]\nitems = [{}]",
             "[context_menu]\nitems = [{id = 'typo'}]",
             "[context_menu]\nitems = [{id = 'action:missing'}]",
             "[context_menu]\nitems = [{id = ''}]",
             "[context_menu]\nitems = [{id = true}]",
             "[context_menu]\nitems = [{label = 'Empty'}]",
             "[context_menu]\nitems = [{items = []}]",
             "[context_menu]\nitems = [{label = 'Bad', items = 1}]",
             "[context_menu]\nitems = [{id = 'open', items = []}]",
             "[context_menu]\nitems = [{id = 'open', remaining = true}]",
             "[context_menu]\nitems = [{id = 'open', typo = true}]",
             "[context_menu]\nitems = [{separator = 1}]",
             "[context_menu]\nitems = [{remaining = 'true'}]",
             "[context_menu]\nitems = [{remaining = true, label = 'Bad'}]",
             "[context_menu]\nitems = [{remaining = true}, {label = 'Group', items = [{remaining = "
             "true}]}]",
             "[context_menu]\nitems = [{id = 'open'}, {label = 'Group', items = [{id = 'open'}]}]",
         }) {
        CAPTURE(std::string{invalid});
        const auto result = parse_config(invalid, "menu.toml");
        CHECK_FALSE(result.problems.empty());
        CHECK_FALSE(result.validation_errors.empty());
        CHECK_FALSE(result.context_menu_valid);
        CHECK_FALSE(result.config.context_menu.items);
        CHECK_FALSE(result.config.context_menu.entry);
    }
    const auto syntax = parse_config("[context_menu", "menu.toml");
    CHECK_FALSE(syntax.context_menu_valid);
}

namespace {

ConfigResult parse(std::string_view text) { return parse_config(text, "test.toml"); }

void check_defaults(const Config& config) {
    const Config defaults;
    CHECK(config.previews.media == defaults.previews.media);
    CHECK(config.previews.pdf == defaults.previews.pdf);
    CHECK(config.windows.reuse == defaults.windows.reuse);
    CHECK(config.windows.separate_settings == defaults.windows.separate_settings);
    CHECK(config.keyboard.type_to_jump == defaults.keyboard.type_to_jump);
    CHECK(config.keyboard.vim == defaults.keyboard.vim);
    CHECK(config.keyboard.wrap == defaults.keyboard.wrap);
    CHECK(config.status_bar.text_size == defaults.status_bar.text_size);
    CHECK(config.appearance.background_opacity == defaults.appearance.background_opacity);
    CHECK(config.appearance.palette_source == defaults.appearance.palette_source);
    CHECK(config.appearance.color_scheme == defaults.appearance.color_scheme);
    CHECK(config.font.family == defaults.font.family);
    CHECK(config.font.monospace == defaults.font.monospace);
    CHECK(config.pointer.double_click_ms == defaults.pointer.double_click_ms);
    CHECK(config.pointer.double_click_distance == defaults.pointer.double_click_distance);
    CHECK(config.icon.follow_accent == defaults.icon.follow_accent);
    CHECK(config.icon.theme == defaults.icon.theme);
    CHECK(config.icon.places == defaults.icon.places);
}

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

// Sets an environment variable for one scope, restoring the old value after.
struct ScopedEnv {
    std::string name;
    bool had_value{false};
    std::string old_value;

    ScopedEnv(std::string variable, const char* value) : name{std::move(variable)} {
        if (const char* old = std::getenv(name.c_str()); old != nullptr) {
            had_value = true;
            old_value = old;
        }
        if (value != nullptr) {
            setenv(name.c_str(), value, 1);
        } else {
            unsetenv(name.c_str());
        }
    }

    ~ScopedEnv() {
        if (had_value) {
            setenv(name.c_str(), old_value.c_str(), 1);
        } else {
            unsetenv(name.c_str());
        }
    }

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;
    ScopedEnv(ScopedEnv&&) = delete;
    ScopedEnv& operator=(ScopedEnv&&) = delete;
};

}  // namespace

TEST_CASE("an empty file gives the defaults and no problems") {
    const ConfigResult result = parse("");
    CHECK(result.from_file);
    CHECK(result.problems.empty());
    check_defaults(result.config);
    CHECK(result.config.animations == rummage::core::MotionConfig{});
}

TEST_CASE("status bar text size accepts finite logical pixels from 8 to 32") {
    for (const std::string value : {"8", "12.0", "15", "15.5", "32"}) {
        const auto result = parse("[status_bar]\ntext_size = " + value);
        CAPTURE(value);
        CHECK(result.problems.empty());
        CHECK(result.config.status_bar.text_size == doctest::Approx(std::stof(value)));
    }
    for (const std::string value : {"7.9", "32.1", "nan", "inf", "-inf", "true", "\"15\""}) {
        const auto result = parse("[status_bar]\ntext_size = " + value);
        CAPTURE(value);
        REQUIRE(result.problems.size() == 1);
        CHECK(contains(result.problems.front(), "status_bar.text_size"));
        CHECK(result.config.status_bar.text_size == 12.0F);
    }
    CHECK_FALSE(parse("status_bar = 15").problems.empty());
    CHECK_FALSE(parse("[status_bar]\nsize = 15").problems.empty());
}

TEST_CASE("icon themes follow desktop settings, accept an override and can be disabled") {
    CHECK(parse("").config.icon.theme == "system");
    CHECK(parse("[icon]\ntheme = \"builtin\"").config.icon.theme == "builtin");
    CHECK_FALSE(parse("[icon]\ntheme = 42").problems.empty());
    CHECK_FALSE(parse("[icon]\ntheme = \"\"").problems.empty());
}

TEST_CASE("place icons: named icons and custom files are read independently") {
    const auto result = parse(R"(
[icon.places]
home = "builtin:home"
documents = "folder-documents"
pictures = "/tmp/my pictures.svg"
trash_full = "builtin:trash-filled"
downloads = 42
music = ""
typo = "folder"
)");
    REQUIRE(result.config.icon.places.size() == 4);
    CHECK(result.config.icon.places.at("home") == "builtin:home");
    CHECK(result.config.icon.places.at("documents") == "folder-documents");
    CHECK(result.config.icon.places.at("pictures") == "/tmp/my pictures.svg");
    CHECK(result.config.icon.places.at("trash_full") == "builtin:trash-filled");
    CHECK(result.problems.size() == 3);
    CHECK_FALSE(parse("[icon]\nplaces = false").problems.empty());
    CHECK(parse("").config.icon.places.empty());  // removing overrides restores defaults
}

TEST_CASE("every key is read") {
    const ConfigResult result = parse(R"(
[font]
family = "Inter:weight=medium"
monospace = "JetBrains Mono"
size = 15

[pointer]
double_click_ms = 250
double_click_distance = 0

[icon]
follow_accent = false
theme = "candy-icons"
)");
    CHECK(result.problems.empty());
    CHECK(result.config.font.family == "Inter:weight=medium");
    CHECK(result.config.icon.theme == "candy-icons");
    CHECK(result.config.font.monospace == "JetBrains Mono");
    CHECK(result.config.pointer.double_click_ms == 250);
    CHECK(result.config.pointer.double_click_distance == 0);
    CHECK_FALSE(result.config.icon.follow_accent);
}

TEST_CASE("dotted keys work like sections") {
    const ConfigResult result = parse("pointer.double_click_ms = 500\n");
    CHECK(result.problems.empty());
    CHECK(result.config.pointer.double_click_ms == 500);
}

TEST_CASE("a syntax error ignores the whole file and says where") {
    const ConfigResult result = parse(R"([pointer]
double_click_ms = 250
double_click_distance = = 3
)");
    CHECK_FALSE(result.from_file);
    REQUIRE(result.problems.size() == 1);
    CHECK(result.problems[0].starts_with("test.toml:3:"));
    CHECK(contains(result.problems[0], "whole file is ignored"));
    CHECK(result.validation_errors == result.problems);
    check_defaults(result.config);
}

TEST_CASE("unknown sections and keys are reported, the rest still applies") {
    const ConfigResult result = parse(R"([pointer]
dubble_click_ms = 250
double_click_distance = 8

[colours]
accent = "#ff0000"
)");
    CHECK(result.from_file);
    REQUIRE(result.problems.size() == 2);
    CHECK(result.problems[0] == "test.toml:2:1: unknown key 'pointer.dubble_click_ms'; ignored");
    CHECK(contains(result.problems[1], "test.toml:5:"));
    CHECK(contains(result.problems[1], "'colours'"));
    CHECK(result.config.pointer.double_click_distance == 8);
    CHECK(result.config.pointer.double_click_ms == Config{}.pointer.double_click_ms);
    CHECK(result.validation_errors.empty());
}

TEST_CASE("a wrong type keeps the default and names both types") {
    const ConfigResult result = parse(R"([pointer]
double_click_ms = "fast"
double_click_distance = 4.5

[font]
family = 12

[icon]
follow_accent = "no"
)");
    REQUIRE(result.problems.size() == 4);
    CHECK(result.problems[0] ==
          "test.toml:2:19: 'pointer.double_click_ms' must be an integer, got a string; using 400");
    CHECK(contains(result.problems[1], "got a number with decimals"));
    CHECK(contains(result.problems[2], "'font.family' must be a string, got an integer"));
    CHECK(result.problems[3] ==
          "test.toml:9:17: 'icon.follow_accent' must be true or false, got a string; using true");
    check_defaults(result.config);
}

TEST_CASE("values out of range keep the default") {
    const ConfigResult result = parse(R"([pointer]
double_click_ms = 99
double_click_distance = 51
)");
    REQUIRE(result.problems.size() == 2);
    CHECK(contains(result.problems[0], "between 100 and 2000, got 99"));
    CHECK(contains(result.problems[1], "between 0 and 50, got 51"));
    check_defaults(result.config);

    const ConfigResult limits = parse(R"([pointer]
double_click_ms = 2000
double_click_distance = 50
)");
    CHECK(limits.problems.empty());
    CHECK(limits.config.pointer.double_click_ms == 2000);
    CHECK(limits.config.pointer.double_click_distance == 50);
}

TEST_CASE("an empty font family keeps the default") {
    const ConfigResult result = parse("font.family = \"\"\n");
    REQUIRE(result.problems.size() == 1);
    CHECK(contains(result.problems[0], "must not be empty"));
    check_defaults(result.config);
}

TEST_CASE("a section given as a plain value is reported") {
    const ConfigResult result = parse("font = \"Inter\"\n");
    REQUIRE(result.problems.size() == 1);
    CHECK(contains(result.problems[0], "'font' must be a section ([font]), got a string"));
    check_defaults(result.config);
}

TEST_CASE("load_config: missing file, real file, directory") {
    const TempDir dir;

    const ConfigResult missing = load_config(dir.path / "absent.toml");
    CHECK_FALSE(missing.from_file);
    CHECK(missing.problems.empty());
    check_defaults(missing.config);

    const std::filesystem::path file = dir.write("config.toml", "pointer.double_click_ms = 300\n");
    const ConfigResult loaded = load_config(file);
    CHECK(loaded.from_file);
    CHECK(loaded.problems.empty());
    CHECK(loaded.config.pointer.double_click_ms == 300);

    const std::filesystem::path broken = dir.write("broken.toml", "[pointer\n");
    const ConfigResult bad = load_config(broken);
    REQUIRE(bad.problems.size() == 1);
    CHECK(bad.problems[0].starts_with(broken.string() + ":1:"));

    const ConfigResult directory = load_config(dir.path);
    REQUIRE(directory.problems.size() == 1);
    CHECK(contains(directory.problems[0], "not a regular file"));
    CHECK(directory.validation_errors == directory.problems);
    CHECK(bad.validation_errors == bad.problems);
    CHECK(missing.validation_errors.empty());
}

TEST_CASE("the example config parses clean and matches the defaults") {
    const ConfigResult result = load_config(RUMMAGE_EXAMPLE_CONFIG);
    CHECK(result.from_file);
    CHECK(result.problems.empty());
    check_defaults(result.config);
    CHECK(result.config.animations == rummage::core::MotionConfig{});
}

TEST_CASE("default_config_path follows XDG_CONFIG_HOME, then HOME") {
    const ScopedEnv home{"HOME", "/home/someone"};
    {
        const ScopedEnv xdg{"XDG_CONFIG_HOME", "/tmp/xdg"};
        CHECK(default_config_path() == "/tmp/xdg/rummage/config.toml");
    }
    {
        const ScopedEnv xdg{"XDG_CONFIG_HOME", "relative/ignored"};
        CHECK(default_config_path() == "/home/someone/.config/rummage/config.toml");
    }
    {
        const ScopedEnv xdg{"XDG_CONFIG_HOME", nullptr};
        CHECK(default_config_path() == "/home/someone/.config/rummage/config.toml");
        const ScopedEnv no_home{"HOME", nullptr};
        CHECK(default_config_path().empty());
    }
}

TEST_CASE("background opacity accepts finite numbers from zero to one") {
    for (const std::string value : {"0", "1", "0.7", "1.0", "0.0"}) {
        const auto result = parse("[appearance]\nbackground_opacity = " + value);
        CHECK(result.problems.empty());
        CHECK(result.config.appearance.background_opacity == doctest::Approx(std::stof(value)));
    }
    for (const std::string value : {"-0.1", "1.1", "nan", "inf", "-inf", "true", "\"0.7\""}) {
        const auto result = parse("[appearance]\nbackground_opacity = " + value);
        CAPTURE(value);
        CHECK(result.problems.size() == 1);
        CHECK(result.config.appearance.background_opacity == 1.0F);
    }
}

TEST_CASE("config: keybindings accept overrides and disabling") {
    const auto result = parse("[keybindings]\nrename = 'Ctrl+r'\ntrash = ''\n");
    CHECK(result.problems.empty());
    CHECK(result.config.keybindings.at("rename") == "Ctrl+r");
    CHECK(result.config.keybindings.at("trash").empty());
    CHECK_FALSE(parse("[keybindings]\nrename = 12\n").problems.empty());
    CHECK_FALSE(parse("keybindings = true\n").problems.empty());
}

TEST_CASE("first run creates the full reference and preserves existing configs") {
    TempDir dir;
    const auto path = dir.path / "nested" / "rummage" / "config.toml";
    REQUIRE_FALSE(rummage::core::create_default_config(path));
    const auto created = rummage::core::read_text_file(path);
    CHECK(created.text == rummage::core::read_text_file(RUMMAGE_EXAMPLE_CONFIG).text);
    const auto loaded = load_config(path);
    CHECK(loaded.from_file);
    CHECK(loaded.problems.empty());
    check_defaults(loaded.config);

    for (const std::string content : {"", "[font]\nfamily = 'Custom'\n", "invalid ["}) {
        REQUIRE_FALSE(rummage::core::write_file_atomically(path, content));
        REQUIRE_FALSE(rummage::core::create_default_config(path));
        CHECK(rummage::core::read_text_file(path).text == content);
        // Exercise the publication race: another process has already created the target.
        REQUIRE_FALSE(rummage::core::write_file_atomically(path, "replacement", false));
        CHECK(rummage::core::read_text_file(path).text == content);
    }
    CHECK_FALSE(rummage::core::create_default_config({}));
}

TEST_CASE("first run preserves symlinks and reports creation failures") {
    TempDir dir;
    const auto target = dir.write("settings.toml", "# user settings\n");
    const auto link = dir.path / "config.toml";
    std::filesystem::create_symlink(target, link);
    CHECK_FALSE(rummage::core::create_default_config(link));
    CHECK(std::filesystem::is_symlink(link));
    CHECK(rummage::core::read_text_file(target).text == "# user settings\n");

    const auto missing = dir.path / "missing.toml";
    const auto dangling = dir.path / "dangling.toml";
    std::filesystem::create_symlink(missing, dangling);
    CHECK_FALSE(rummage::core::create_default_config(dangling));
    CHECK(std::filesystem::is_symlink(dangling));
    CHECK_FALSE(std::filesystem::exists(missing));
    CHECK(rummage::core::create_default_config(target / "config.toml"));
}

TEST_CASE("startup config: modes, paths and invalid values") {
    using Mode = Config::Startup::Mode;
    for (const auto& [word, mode] :
         {std::pair{"home", Mode::home}, std::pair{"folder", Mode::folder},
          std::pair{"last", Mode::last}, std::pair{"session", Mode::session}}) {
        const auto read =
            parse_config(std::string{"[startup]\nmode = '"} + word + "'\nfolder = '/some folder'\n",
                         "test.toml");
        CHECK(read.problems.empty());
        CHECK(read.config.startup.mode == mode);
        CHECK(read.config.startup.folder == "/some folder");
    }
    for (const auto* bad : {"mode = 'bad'", "mode = 1", "folder = ''", "folder = 'relative'",
                            "folder = '~someone/path'", "folder = 42", "unknown = true"}) {
        const auto read = parse_config(std::string{"[startup]\n"} + bad, "test.toml");
        CHECK_FALSE(read.problems.empty());
        CHECK(read.config.startup.mode == Mode::session);
        CHECK(read.config.startup.folder == "~");
    }
    CHECK(rummage::core::expand_folder_path("/a/../b") == "/b");
    CHECK(rummage::core::expand_folder_path("/a/") == "/a");
    CHECK(rummage::core::expand_folder_path("/") == "/");
    CHECK(rummage::core::expand_folder_path(std::string{"/a\0b", 4}).empty());
}

TEST_CASE("terminal: configuration and automatic fallback") {
    const auto result = parse_config(
        "[terminal]\nprogram = 'ghostty'\nargs = ['--gtk-single-instance=false']\n", "test");
    REQUIRE(result.problems.empty());
    CHECK(result.config.terminal.program == "ghostty");
    CHECK(result.config.terminal.args == std::vector<std::string>{"--gtk-single-instance=false"});
    CHECK(parse_config("[terminal]\nprogram = ''\n", "test").problems.empty());
    for (const auto* invalid : {"program = 3", "args = 3", "args = ['ok', 1]"}) {
        CHECK_FALSE(parse_config(std::string{"[terminal]\n"} + invalid, "test").problems.empty());
    }
}

TEST_CASE("font size: 8 to 32, whole or not, else the default") {
    const Config defaults;
    CHECK(defaults.font.size == 13.0F);
    CHECK(defaults.font.zoom() == 1.0F);
    for (const std::string value : {"8", "13", "15", "17.5", "32", "32.0"}) {
        const auto result = parse("[font]\nsize = " + value);
        CAPTURE(value);
        CHECK(result.problems.empty());
        CHECK(result.config.font.size == doctest::Approx(std::stod(value)));
    }
    CHECK(parse("[font]\nsize = 26").config.font.zoom() == 2.0F);
    for (const std::string value : {"7.9", "33", "0", "-13", "nan", "inf", "\"15\"", "true"}) {
        const auto result = parse("[font]\nsize = " + value);
        CAPTURE(value);
        REQUIRE(result.problems.size() == 1);
        CHECK(contains(result.problems[0], "'font.size' must be"));
        CHECK(result.config.font.size == 13.0F);
    }
}

TEST_CASE("open: programs may be turned off; anything but a boolean is reported") {
    const auto off = parse("[open]\nprograms = false\n");
    CHECK(off.problems.empty());
    CHECK_FALSE(off.config.open.programs);
    const auto wrong = parse("[open]\nprograms = \"no\"\n");
    REQUIRE(wrong.problems.size() == 1);
    CHECK(contains(wrong.problems[0], "'open.programs'"));
    CHECK(wrong.config.open.programs);
    const auto unknown = parse("[open]\nscripts = true\n");
    CHECK(unknown.problems.size() == 1);
}

TEST_CASE("palette source: defaults preserve automatic precedence and values are validated") {
    CHECK(parse_config("", "test").config.appearance.palette_source == "auto");
    for (const auto value : {"auto", "desktop", "file", "builtin", "kde"}) {
        const auto result = parse_config(
            std::string{"[appearance]\npalette_source = '"} + value + "'\ncolor_scheme = 'light'\n",
            "test");
        REQUIRE(result.problems.empty());
        CHECK(result.config.appearance.palette_source == value);
        CHECK(result.config.appearance.color_scheme == "light");
    }
    for (const auto value : {"'system'", "'noctalia'", "''", "42", "false", "[]", "{}"}) {
        const auto result =
            parse_config(std::string{"[appearance]\npalette_source = "} + value + "\n", "test");
        REQUIRE(result.problems.size() == 1);
        CHECK(result.problems.front().find("appearance.palette_source") != std::string::npos);
        CHECK(result.config.appearance.palette_source == "auto");
    }
}

TEST_CASE("sidebar configuration validates bounded identity lists") {
    const auto parsed = rummage::core::parse_config(R"(
[sidebar]
order = ["place:home", "separator:0:0", "place:downloads"]
hidden = ["uuid:example", "place:music"]
)",
                                                    "sidebar.toml");
    REQUIRE(parsed.problems.empty());
    REQUIRE(parsed.config.sidebar.order);
    CHECK(parsed.config.sidebar.order->size() == 3);
    REQUIRE(parsed.config.sidebar.hidden);
    CHECK(parsed.config.sidebar.hidden->size() == 2);
    const auto invalid = rummage::core::parse_config("[sidebar]\nhidden = [42]\n", "bad.toml");
    CHECK_FALSE(invalid.problems.empty());
    CHECK_FALSE(invalid.config.sidebar.hidden);
}

TEST_CASE("config: keyboard navigation preferences validate independently") {
    const auto enabled = parse("[keyboard]\ntype_to_jump = false\nvim = true\nwrap = true\n");
    REQUIRE(enabled.problems.empty());
    CHECK_FALSE(enabled.config.keyboard.type_to_jump);
    CHECK(enabled.config.keyboard.vim);
    CHECK(enabled.config.keyboard.wrap);
    const auto invalid = parse("[keyboard]\ntype_to_jump = 1\nvim = 'yes'\nwrap = false\n");
    CHECK(invalid.problems.size() == 2);
    CHECK(invalid.config.keyboard.type_to_jump);
    CHECK_FALSE(invalid.config.keyboard.vim);
    CHECK_FALSE(invalid.config.keyboard.wrap);
}

TEST_CASE("config: window policies validate independently") {
    const auto changed = parse("[windows]\nreuse = false\nseparate_settings = false\n");
    CHECK(changed.problems.empty());
    CHECK_FALSE(changed.config.windows.reuse);
    CHECK_FALSE(changed.config.windows.separate_settings);
    const auto invalid = parse("[windows]\nreuse = 1\nseparate_settings = 'yes'\n");
    CHECK(invalid.problems.size() == 2);
    CHECK(invalid.config.windows.reuse);
    CHECK(invalid.config.windows.separate_settings);
}

TEST_CASE("preview settings independently disable shipped helpers") {
    const auto result = parse_config("[previews]\nmedia = false\npdf = true\n", "test.toml");
    CHECK(result.problems.empty());
    CHECK_FALSE(result.config.previews.media);
    CHECK(result.config.previews.pdf);
    const auto invalid = parse_config("[previews]\nmedia = 'false'\npdf = 0\n", "test.toml");
    CHECK(invalid.problems.size() == 2);
    CHECK(invalid.config.previews.media);
    CHECK(invalid.config.previews.pdf);
}

TEST_CASE("config validation: ignored unknown settings are nonblocking and remain reported") {
    const auto result = parse(R"([appearance]
transparent_settings = true
[icon.places]
future_place = "folder"
[future_section]
new_option = true
)");
    REQUIRE(result.from_file);
    REQUIRE(result.problems.size() == 3);
    CHECK(contains(result.problems[0], "appearance.transparent_settings"));
    CHECK(contains(result.problems[1], "icon.places.future_place"));
    CHECK(contains(result.problems[2], "future_section"));
    CHECK(result.validation_errors.empty());
    CHECK(result.actions_valid);
    CHECK(result.context_menu_valid);
}

TEST_CASE("config validation: errors remain in file order alongside ignored unknown settings") {
    const auto result = parse(R"([pointer]
double_click_ms = "fast"
future_option = true
double_click_distance = 99
[appearance]
transparent_settings = true
[font]
family = 12
)");
    REQUIRE(result.problems.size() == 5);
    REQUIRE(result.validation_errors.size() == 3);
    CHECK(result.validation_errors[0] == result.problems[0]);
    CHECK(result.validation_errors[1] == result.problems[2]);
    CHECK(result.validation_errors[2] == result.problems[4]);
    CHECK(result.validation_errors[0].starts_with("test.toml:2:"));
    CHECK(result.validation_errors[1].starts_with("test.toml:4:"));
    CHECK(result.validation_errors[2].starts_with("test.toml:8:"));
}

TEST_CASE("config validation: ignored invalid values and unknown action fields stay blocking") {
    for (const auto* text : {
             "font = false",
             "[icon]\nplaces = false",
             "[[actions]]\nid = 'test'\nlabel = 'Test'\nargv = ['echo']\nunknown = true",
             "[context_menu]\nunknown = []",
             "[context_menu]\nitems = [{id = 'open', unknown = true}]",
         }) {
        CAPTURE(text);
        const auto result = parse(text);
        REQUIRE_FALSE(result.problems.empty());
        CHECK(result.validation_errors == result.problems);
    }
}
