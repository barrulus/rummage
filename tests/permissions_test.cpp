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
