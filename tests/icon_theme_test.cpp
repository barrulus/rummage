#include "rummage/ui/icon_theme.hpp"

#include <algorithm>
#include <chrono>

#include <doctest/doctest.h>

#include "rummage/formats/image/load.hpp"
#include "rummage/picture/png.hpp"
#include "rummage/svg/svg.hpp"
#include "scoped_env.hpp"
#include "temp_dir.hpp"

using rummage::test::ScopedEnv;
using rummage::test::TempDir;
using rummage::ui::IconTheme;
namespace fs = std::filesystem;

namespace {

using namespace std::chrono_literals;

void settle(rummage::core::EventLoop& loop, std::chrono::milliseconds time = 150ms) {
    loop.add_timer(time, 0ms, [&] { loop.quit(); });
    loop.run();
}

void write(const fs::path& root, const std::string& name, std::string_view text) {
    fs::create_directories((root / name).parent_path());
    std::ofstream{root / name} << text;
}

constexpr std::string_view kSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="40" height="20"><rect width="40" height="20" fill="#ff0000" opacity="0.5"/></svg>)";

}  // namespace

TEST_CASE("icon theme: desktop discovery preserves source precedence and fallbacks") {
    const TempDir dir;
    const ScopedEnv config{"XDG_CONFIG_HOME", dir.path.string()};
    write(dir.path, "gtk-3.0/settings.ini", "[Settings]\ngtk-icon-theme-name=gtk3\n");
    write(dir.path, "gtk-4.0/settings.ini", "[Settings]\ngtk-icon-theme-name=gtk4\n");
    write(dir.path, "kdeglobals", "[Icons]\nTheme=kde\n");
    write(dir.path, "qt6ct/qt6ct.conf", "[Appearance]\nicon_theme=qt\n");
    {
        const ScopedEnv desktop{"XDG_CURRENT_DESKTOP", "KDE"};
        CHECK(rummage::ui::desktop_icon_theme() == "kde");
    }
    const ScopedEnv desktop{"XDG_CURRENT_DESKTOP", "GNOME"};
    CHECK(rummage::ui::desktop_icon_theme() == "gtk3");
    write(dir.path, "gtk-3.0/settings.ini", "[Settings]\ngtk-icon-theme-name=../invalid\n");
    CHECK(rummage::ui::desktop_icon_theme() == "gtk4");
    fs::remove(dir.path / "gtk-4.0/settings.ini");
    CHECK(rummage::ui::desktop_icon_theme() == "kde");
    fs::remove(dir.path / "kdeglobals");
    CHECK(rummage::ui::desktop_icon_theme() == "qt");
    fs::remove(dir.path / "qt6ct/qt6ct.conf");
    CHECK(rummage::ui::desktop_icon_theme() == "hicolor");
}

TEST_CASE("icon theme watcher: missing sources, writes, replacements and source removal") {
    const TempDir dir;
    const auto root = dir.path / "config";
    const ScopedEnv config{"XDG_CONFIG_HOME", root.string()};
    const ScopedEnv desktop{"XDG_CURRENT_DESKTOP", "GNOME"};
    rummage::core::EventLoop loop;
    std::vector<std::string> changes;
    const rummage::ui::DesktopIconThemeWatcher watcher{
        loop, [&](const std::string& name) { changes.push_back(name); }};
    CHECK(watcher.theme() == "hicolor");
    CHECK_FALSE(fs::exists(root));
    write(root, "qt6ct/qt6ct.conf", "[Appearance]\nicon_theme=qt\n");
    settle(loop);
    REQUIRE(changes == std::vector<std::string>{"qt"});
    write(root, "gtk-3.0/settings.ini", "[Settings]\ngtk-icon-theme-name=first\n");
    settle(loop);
    REQUIRE(watcher.theme() == "first");
    write(root, "gtk-3.0/new.ini", "[Settings]\ngtk-icon-theme-name=second\n");
    fs::rename(root / "gtk-3.0/new.ini", root / "gtk-3.0/settings.ini");
    settle(loop);
    REQUIRE(watcher.theme() == "second");
    const auto count = changes.size();
    write(root, "gtk-3.0/settings.ini", "[Settings]\ngtk-icon-theme-name=second\nother=value\n");
    write(root, "gtk-3.0/irrelevant.ini", "unrelated");
    write(root, "qt6ct/qt6ct.conf", "[Appearance]\nicon_theme=lower-priority\n");
    settle(loop);
    CHECK(changes.size() == count);
    fs::remove(root / "gtk-3.0/settings.ini");
    settle(loop);
    CHECK(watcher.theme() == "lower-priority");
    fs::remove(root / "qt6ct/qt6ct.conf");
    settle(loop);
    CHECK(watcher.theme() == "hicolor");
    CHECK(changes ==
          std::vector<std::string>{"qt", "first", "second", "lower-priority", "hicolor"});
}

TEST_CASE("icon theme watcher: replaced directories, symlinks and coalesced changes") {
    const TempDir dir;
    const auto root = dir.path / "config";
    const ScopedEnv config{"XDG_CONFIG_HOME", root.string()};
    const ScopedEnv desktop{"XDG_CURRENT_DESKTOP", "KDE"};
    write(root, "kdeglobals", "[Icons]\nTheme=initial\n");
    rummage::core::EventLoop loop;
    std::vector<std::string> changes;
    const rummage::ui::DesktopIconThemeWatcher watcher{
        loop, [&](const std::string& name) { changes.push_back(name); }};
    fs::rename(root, dir.path / "old-config");
    write(root, "kdeglobals", "[Icons]\nTheme=replaced\n");
    settle(loop);
    CHECK(watcher.theme() == "replaced");
    // Switching a config symlink and editing its external target both refresh discovery.
    const auto target = dir.write("target.ini", "[Icons]\nTheme=linked\n");
    fs::create_symlink(target, root / "kdeglobals.new");
    fs::rename(root / "kdeglobals.new", root / "kdeglobals");
    settle(loop);
    CHECK(watcher.theme() == "linked");
    dir.write("target.ini", "[Icons]\nTheme=edited-target\n");
    settle(loop);
    CHECK(watcher.theme() == "edited-target");
    const auto replacement = dir.write("replacement.ini", "[Icons]\nTheme=replaced-target\n");
    fs::rename(replacement, target);
    settle(loop);
    CHECK(watcher.theme() == "replaced-target");
    const auto count = changes.size();
    for (const auto name : {"intermediate", "latest"})
        dir.write("target.ini", std::string{"[Icons]\nTheme="} + name + '\n');
    settle(loop);
    CHECK(watcher.theme() == "latest");
    CHECK(changes.size() == count + 1);
    fs::remove(target);
    settle(loop);
    CHECK(watcher.theme() == "hicolor");
    dir.write("target.ini", "[Icons]\nTheme=recreated-target\n");
    settle(loop);
    CHECK(watcher.theme() == "recreated-target");
    fs::remove_all(root);
    settle(loop);
    CHECK(watcher.theme() == "hicolor");
    write(root, "kdeglobals", "[Icons]\nTheme=recreated\n");
    settle(loop);
    CHECK(watcher.theme() == "recreated");
}

TEST_CASE("icon theme watcher: missing symlink directories can appear after launch") {
    const TempDir dir;
    const auto root = dir.path / "config";
    const auto target = dir.path / "external";
    fs::create_directories(root);
    fs::create_directory_symlink(target, root / "gtk-4.0");
    const ScopedEnv config{"XDG_CONFIG_HOME", root.string()};
    const ScopedEnv desktop{"XDG_CURRENT_DESKTOP", "GNOME"};
    rummage::core::EventLoop loop;
    const rummage::ui::DesktopIconThemeWatcher watcher{loop, {}};
    CHECK(watcher.theme() == "hicolor");
    CHECK_FALSE(fs::exists(target));
    write(target, "settings.ini", "[Settings]\ngtk-icon-theme-name=linked-directory\n");
    settle(loop);
    CHECK(watcher.theme() == "linked-directory");
    fs::rename(target, dir.path / "old-external");
    settle(loop);
    CHECK(watcher.theme() == "hicolor");
    write(target, "settings.ini", "[Settings]\ngtk-icon-theme-name=new-directory\n");
    settle(loop);
    CHECK(watcher.theme() == "new-directory");
}

TEST_CASE("icon theme watcher: destruction cancels a pending change") {
    const TempDir dir;
    const ScopedEnv config{"XDG_CONFIG_HOME", dir.path.string()};
    rummage::core::EventLoop loop;
    int changes = 0;
    {
        const rummage::ui::DesktopIconThemeWatcher watcher{loop, [&](const auto&) { ++changes; }};
        write(dir.path, "gtk-3.0/settings.ini", "[Settings]\ngtk-icon-theme-name=late\n");
        settle(loop, 10ms);
    }
    settle(loop);
    CHECK(changes == 0);
}

TEST_CASE("icon theme: sizes, inherited themes, split roots and missing names") {
    TempDir dir;
    const fs::path user = dir.path / "user";
    const fs::path system = dir.path / "system";
    write(user, "candy/index.theme", R"([Icon Theme]
Directories=small,scalable
Inherits=parent
[small]
Size=16
Type=Fixed
[scalable]
Size=48
Type=Scalable
MinSize=32
MaxSize=512
)");
    write(user, "candy/small/folder.svg", kSvg);
    write(user, "candy/scalable/folder.svg", kSvg);
    write(system, "candy/scalable/video-x-generic.svg", kSvg);
    write(user, "parent/index.theme",
          "[Icon Theme]\nDirectories=48\nInherits=candy\n[48]\nSize=48\n");
    write(user, "parent/48/text-x-generic.svg", kSvg);
    write(system, "hicolor/index.theme", "[Icon Theme]\nDirectories=48\n[48]\nSize=48\n");
    write(system, "hicolor/48/fallback.svg", kSvg);
    const IconTheme theme{"candy", {user, system}};
    CHECK(theme.find("folder", 16) == user / "candy/small/folder.svg");
    CHECK(theme.find("folder", 384) == user / "candy/scalable/folder.svg");
    CHECK(theme.find("video-x-generic", 192) == system / "candy/scalable/video-x-generic.svg");
    CHECK(theme.find("text-x-generic", 64) == user / "parent/48/text-x-generic.svg");
    CHECK(theme.find("fallback", 64) == system / "hicolor/48/fallback.svg");
    CHECK(theme.find("missing", 64).empty());
    CHECK(theme.find("../folder", 64).empty());
}

TEST_CASE("icon theme: a selected theme wins over a closer inherited size") {
    TempDir dir;
    write(dir.path, "selected/index.theme",
          "[Icon Theme]\nDirectories=16\nInherits=parent\n[16]\nSize=16\nType=Fixed\n");
    write(dir.path, "selected/16/folder.svg", kSvg);
    write(dir.path, "parent/index.theme",
          "[Icon Theme]\nDirectories=128\n[128]\nSize=128\nType=Fixed\n");
    write(dir.path, "parent/128/folder.svg", kSvg);
    const IconTheme theme{"selected", {dir.path}};
    CHECK(theme.find("folder", 128) == dir.path / "selected/16/folder.svg");
}

TEST_CASE("icon theme: scale and user overrides are respected") {
    TempDir dir;
    write(dir.path, "theme/index.theme",
          "[Icon "
          "Theme]\nDirectories=32\nScaledDirectories=32@2\n[32]\nSize=32\nType=Fixed\n[32@2]\nSize="
          "32\nScale=2\nType=Fixed\n");
    write(dir.path, "theme/32/file.svg", kSvg);
    write(dir.path, "theme/32@2/file.svg", kSvg);
    const IconTheme theme{"theme", {dir.path}};
    CHECK(theme.find("file", 64) == dir.path / "theme/32@2/file.svg");
}

TEST_CASE("icon theme: SVG preserves proportions, colour and straight alpha") {
    TempDir dir;
    const auto file = dir.write("icon.svg", kSvg);
    const auto image = IconTheme::render(file, 80);
    REQUIRE_FALSE(image.empty());
    CHECK(image.width == 80);
    CHECK(image.height == 40);
    CHECK(image.rgba[0] == 255);
    CHECK(image.rgba[1] == 0);
    CHECK(image.rgba[2] == 0);
    CHECK(image.rgba[3] == doctest::Approx(128).epsilon(0.01));
    CHECK(IconTheme::render(dir.write("bad.svg", "not svg"), 48).empty());
    CHECK(IconTheme::render(dir.path / "missing.svg", 48).empty());
}

TEST_CASE("icon theme: PNG icons retain their pixels") {
    TempDir dir;
    const rummage::picture::Image original{
        .width = 2, .height = 1, .rgba = {0, 255, 0, 255, 0, 0, 255, 128}};
    const auto png = rummage::picture::encode_png(original);
    const auto file = dir.path / "icon.png";
    {
        std::ofstream output{file, std::ios::binary};
        output.write(reinterpret_cast<const char*>(png.data()),
                     static_cast<std::streamsize>(png.size()));
    }
    const auto image = IconTheme::render(file, 32);
    CHECK(image.rgba == original.rgba);
    CHECK(image.width == 2);
    CHECK(image.height == 1);
}

TEST_CASE("icon theme: absolute custom icons and unavailable overrides") {
    TempDir dir;
    const IconTheme theme{"missing", {dir.path}};
    const auto svg = dir.write("custom.svg", kSvg);
    CHECK(theme.find(svg.string(), 48) == svg);
    CHECK(theme.find((dir.path / "absent.svg").string(), 48).empty());
    CHECK(theme.find(dir.write("other.txt", kSvg).string(), 48).empty());
    fs::create_directory(dir.path / "directory.svg");
    CHECK(theme.find((dir.path / "directory.svg").string(), 48).empty());
    CHECK(theme.find("https://example.org/icon.svg", 48).empty());
}

TEST_CASE("icon theme: folder catalogue deduplicates sizes and excludes broken icons") {
    TempDir dir;
    write(dir.path, "theme/index.theme",
          "[Icon Theme]\nDirectories=16,48\n[16]\nSize=16\n[48]\nSize=48\n");
    write(dir.path, "theme/16/folder-music.svg", kSvg);
    write(dir.path, "theme/48/folder-music.svg", kSvg);
    write(dir.path, "theme/48/folder-red.png", "png");
    write(dir.path, "theme/48/user-home.svg", kSvg);
    write(dir.path, "theme/48/folder-red-symbolic.svg", kSvg);
    write(dir.path, "theme/48/application.svg", kSvg);
    fs::create_symlink("missing", dir.path / "theme/48/folder-broken.svg");
    const IconTheme theme{"theme", {dir.path}};
    CHECK(theme.folder_icons() ==
          std::vector<std::string>{"folder-music", "folder-red", "user-home"});
}

TEST_CASE("icon theme: CSS and filters share the file preview renderer") {
    const TempDir dir;
    const auto file =
        dir.write("filtered.svg",
                  "<svg xmlns='http://www.w3.org/2000/svg' width='32' height='16'>"
                  "<style>g > rect.paint { fill: #00ff00; }</style>"
                  "<defs><filter id='offset' x='0' y='0' width='200%' height='100%'>"
                  "<feOffset dx='8'/></filter></defs>"
                  "<g><rect class='paint' width='8' height='16' filter='url(#offset)'/></g></svg>");
    for (const int size : {16, 48, 128}) {
        const auto icon = IconTheme::render(file, size);
        const auto preview =
            rummage::formats::image::load({.path = file,
                                           .max_width = static_cast<std::uint32_t>(size),
                                           .max_height = static_cast<std::uint32_t>(size)});
        REQUIRE_FALSE(icon.empty());
        REQUIRE(preview.failure == rummage::picture::Failure::none);
        CHECK(icon.width == preview.image.width);
        CHECK(icon.height == preview.image.height);
        CHECK(static_cast<bool>(icon.rgba == preview.image.rgba));
        // feOffset used to be dropped entirely by the theme renderer.
        CHECK(icon.rgba[3] == 0);
        const auto at = static_cast<std::size_t>(size / 3) * 4;
        CHECK(icon.rgba[at] == 0);
        CHECK(icon.rgba[at + 1] == 255);
        CHECK(icon.rgba[at + 3] == 255);
    }
}

TEST_CASE("icon theme: external images are ignored and input is bounded") {
    const TempDir dir;
    const auto external = dir.path / "external.png";
    const auto png =
        rummage::picture::encode_png({.width = 1, .height = 1, .rgba = {255, 0, 0, 255}});
    {
        std::ofstream output{external, std::ios::binary};
        output.write(reinterpret_cast<const char*>(png.data()),
                     static_cast<std::streamsize>(png.size()));
    }
    const auto file =
        dir.write("external.svg",
                  "<svg xmlns='http://www.w3.org/2000/svg' width='32' height='16'>"
                  "<image width='16' height='16' href='file://" +
                      external.string() +
                      "'/>"
                      "<image x='16' width='16' height='16' href='external.png'/></svg>");
    const auto image = IconTheme::render(file, 32);
    REQUIRE_FALSE(image.empty());
    CHECK(std::ranges::all_of(image.rgba, [](auto v) { return v == 0; }));
    std::filesystem::resize_file(file, rummage::svg::kMaxInputBytes + 1);
    CHECK(IconTheme::render(file, 32).empty());
}

TEST_CASE("icon theme: SVG text renders without preview font preparation") {
    const TempDir dir;
    const auto file = dir.write("text.svg",
                                "<svg xmlns='http://www.w3.org/2000/svg' width='64' height='32'>"
                                "<text x='2' y='24' font-size='20' fill='red'>SVG</text></svg>");
    const auto image = IconTheme::render(file, 64);
    REQUIRE(image.width == 64);
    REQUIRE(image.height == 32);
    unsigned painted = 0;
    for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
        if (image.rgba[i + 3] != 0) {
            CHECK(image.rgba[i] == 255);
            CHECK(image.rgba[i + 1] == 0);
            CHECK(image.rgba[i + 2] == 0);
            ++painted;
        }
    }
    CHECK(painted > 20);
}
