#include "rummage/fs/permissions.hpp"

#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include <doctest/doctest.h>

#include "temp_dir.hpp"

using rummage::fs::ModeResult;
using rummage::test::TempDir;
namespace fs = std::filesystem;

namespace {

unsigned mode_of(const fs::path& path) {
    struct stat info{};
    REQUIRE(::stat(path.c_str(), &info) == 0);
    return info.st_mode & 07777U;
}

}  // namespace

TEST_CASE("running_mode: u+x to allow, every x away to stop, other bits kept") {
    using rummage::fs::running_mode;
    CHECK(running_mode(0644, true) == 0744);
    CHECK(running_mode(0600, true) == 0700);
    CHECK(running_mode(0755, true) == 0755);
    CHECK(running_mode(0755, false) == 0644);
    CHECK(running_mode(0711, false) == 0600);
    CHECK(running_mode(04755, false) == 04644);  // setuid kept: not ours to change
}

TEST_CASE("set_running changes regular files only, says what changed, undoes and redoes") {
    const TempDir dir;
    const fs::path app = dir.write("Tool.AppImage", "x");
    const fs::path script = dir.write("run.sh", "#!/bin/sh\n");
    fs::create_directory(dir.path / "folder");
    ::chmod(app.c_str(), 0644);
    ::chmod(script.c_str(), 0755);

    const ModeResult allowed =
        rummage::fs::set_running({app, script, dir.path / "folder", dir.path / "gone"}, true);
    CHECK(mode_of(app) == 0744);
    CHECK(mode_of(script) == 0755);  // already allowed: unchanged, not listed
    CHECK(mode_of(dir.path / "folder") == mode_of(dir.path / "folder"));
    REQUIRE(allowed.changed.size() == 1);
    CHECK(allowed.changed[0].path == app);
    CHECK(allowed.changed[0].before == 0644);
    CHECK(allowed.changed[0].after == 0744);
    REQUIRE(allowed.errors.size() == 1);
    CHECK(allowed.errors[0] == "gone: No such file or directory");

    // Undo: the bits before; redo: the bits after.
    const ModeResult undone = rummage::fs::restore_modes(allowed.changed, true);
    CHECK(mode_of(app) == 0644);
    REQUIRE(undone.changed.size() == 1);
    CHECK(undone.changed[0].before == 0744);
    CHECK(undone.changed[0].after == 0644);
    const ModeResult redone = rummage::fs::restore_modes(undone.changed, true);
    CHECK(mode_of(app) == 0744);
    CHECK(redone.errors.empty());

    // Changed by someone else since: left alone, and said.
    ::chmod(app.c_str(), 0600);
    const ModeResult stale = rummage::fs::restore_modes(redone.changed, true);
    CHECK(mode_of(app) == 0600);
    CHECK(stale.changed.empty());
    CHECK(stale.errors == std::vector<std::string>{"Tool.AppImage: changed since"});

    // Stop: every execute bit goes.
    const ModeResult stopped = rummage::fs::set_running({script}, false);
    CHECK(mode_of(script) == 0644);
    CHECK(stopped.changed.size() == 1);
}

TEST_CASE("set_running through a link changes its target, as chmod does") {
    const TempDir dir;
    const fs::path target = dir.write("tool", "x");
    ::chmod(target.c_str(), 0644);
    fs::create_symlink(target, dir.path / "link");
    const ModeResult result = rummage::fs::set_running({dir.path / "link"}, true);
    CHECK(result.changed.size() == 1);
    CHECK(mode_of(target) == 0744);
}

TEST_CASE("properties permissions: owned files and directories, including unreadable items") {
    const TempDir dir;
    const auto file = dir.write("file", "contents");
    const auto folder = dir.path / "folder";
    fs::create_directory(folder);
    for (const auto& path : {file, folder}) {
        REQUIRE(::chmod(path.c_str(), 0000) == 0);
        struct stat info{};
        REQUIRE(::lstat(path.c_str(), &info) == 0);
        CHECK(rummage::fs::set_permissions(path, info.st_dev, info.st_ino, 0000, 0750).empty());
        CHECK(mode_of(path) == 0750);
    }
}

TEST_CASE("properties permissions: reject replaced items, links and stale modes") {
    const TempDir dir;
    const auto file = dir.write("file", "contents");
    REQUIRE(::chmod(file.c_str(), 0644) == 0);
    struct stat info{};
    REQUIRE(::lstat(file.c_str(), &info) == 0);
    SUBCASE("mode changed externally") {
        REQUIRE(::chmod(file.c_str(), 0600) == 0);
        CHECK_FALSE(
            rummage::fs::set_permissions(file, info.st_dev, info.st_ino, 0644, 0755).empty());
        CHECK(mode_of(file) == 0600);
    }
    SUBCASE("replacement has a different inode") {
        fs::rename(file, dir.path / "old");
        dir.write("file", "replacement");
        REQUIRE(::chmod(file.c_str(), 0600) == 0);
        CHECK_FALSE(
            rummage::fs::set_permissions(file, info.st_dev, info.st_ino, 0644, 0755).empty());
        CHECK(mode_of(file) == 0600);
    }
    SUBCASE("link cannot redirect a permission edit") {
        fs::create_symlink(file, dir.path / "link");
        CHECK_FALSE(
            rummage::fs::set_permissions(dir.path / "link", info.st_dev, info.st_ino, 0644, 0755)
                .empty());
        CHECK(mode_of(file) == 0644);
    }
    SUBCASE("special bits cannot be added through the editor") {
        CHECK(rummage::fs::set_permissions(file, info.st_dev, info.st_ino, 0644, 04755).empty());
        CHECK(mode_of(file) == 0755);
    }
}

TEST_CASE(
    "batch permissions: explicit bits preserve mixed modes and recursion never follows links") {
    using namespace rummage::fs;
    const TempDir dir;
    fs::create_directories(dir.path / "tree/sub");
    const auto first = dir.write("tree/a", "a"), second = dir.write("tree/sub/b", "b");
    const auto outside = dir.write("outside", "untouched");
    REQUIRE(::chmod(first.c_str(), 0640) == 0);
    REQUIRE(::chmod(second.c_str(), 0604) == 0);
    REQUIRE(::chmod(outside.c_str(), 0600) == 0);
    fs::create_symlink(outside, dir.path / "tree/link");
    auto result = apply_permissions(
        {permission_target(dir.path / "tree")},
        {.mask = 0020, .values = 0020, .owner = {}, .group = {}, .recursive = true});
    CHECK(result.errors.empty());
    CHECK(mode_of(first) == 0660);
    CHECK(mode_of(second) == 0624);
    CHECK(mode_of(outside) == 0600);
    CHECK_FALSE(permission_target(dir.path / "tree/link/").error.empty());
}

TEST_CASE(
    "batch permissions: stale roots are not adopted for retry and partial success is reported") {
    using namespace rummage::fs;
    const TempDir dir;
    const auto first = dir.write("first", "a"), second = dir.write("second", "b");
    REQUIRE(::chmod(first.c_str(), 0644) == 0);
    REQUIRE(::chmod(second.c_str(), 0644) == 0);
    const auto a = permission_target(first), b = permission_target(second);
    REQUIRE(::chmod(second.c_str(), 0600) == 0);
    const auto result = apply_permissions(
        {a, b}, {.mask = 0100, .values = 0100, .owner = {}, .group = {}, .recursive = false});
    CHECK(result.changed == 1);
    CHECK(result.errors.size() == 1);
    REQUIRE(result.refreshed.size() == 1);
    CHECK(result.refreshed.front().path == first);
    CHECK(result.refreshed.front().mode == 0744);
    CHECK(mode_of(second) == 0600);
    const auto refused =
        apply_permissions({result.refreshed.front()}, {.mask = 0,
                                                       .values = 0,
                                                       .owner = std::string{"root\0suffix", 11},
                                                       .group = {},
                                                       .recursive = false});
    CHECK_FALSE(refused.errors.empty());
    const auto own =
        apply_permissions({result.refreshed.front()}, {.mask = 0,
                                                       .values = 0,
                                                       .owner = std::to_string(::geteuid()),
                                                       .group = std::to_string(::getegid()),
                                                       .recursive = false});
    CHECK(own.errors.empty());
}

TEST_CASE("batch permissions: overlapping recursive roots and hard links change once") {
    using namespace rummage::fs;
    const TempDir dir;
    fs::create_directory(dir.path / "folder");
    const auto file = dir.write("folder/file", "data");
    REQUIRE(::chmod(file.c_str(), 0600) == 0);
    fs::create_hard_link(file, dir.path / "alias");
    const auto result = apply_permissions(
        {permission_target(dir.path / "folder"), permission_target(file),
         permission_target(dir.path / "alias")},
        {.mask = 0020, .values = 0020, .owner = {}, .group = {}, .recursive = true});
    CHECK(result.errors.empty());
    CHECK(result.refreshed.size() == 3);
    CHECK(mode_of(file) == 0620);
}

TEST_CASE(
    "batch permissions: denied ownership reports partial mode success and real refreshed state") {
    if (::geteuid() == 0)
        return;  // This check needs ordinary user credentials.
    using namespace rummage::fs;
    const TempDir dir;
    const auto file = dir.write("file", "data");
    REQUIRE(::chmod(file.c_str(), 0600) == 0);
    const auto result = apply_permissions(
        {permission_target(file)},
        {.mask = 0100, .values = 0100, .owner = "0", .group = {}, .recursive = false});
    CHECK_FALSE(result.errors.empty());
    CHECK(result.changed == 1);
    REQUIRE(result.refreshed.size() == 1);
    CHECK(result.refreshed[0].mode == 0700);
    CHECK(result.refreshed[0].owner == ::geteuid());
}
