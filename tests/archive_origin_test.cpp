#include "rummage/fs/archive_origin.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <doctest/doctest.h>

#include "rummage/fs/transfer.hpp"
#include "temp_dir.hpp"
#include "xattr_support.hpp"

namespace fs = std::filesystem;
using namespace rummage::fs;
using rummage::test::TempDir;

TEST_CASE(
    "archive origin: preserves mode with provenance, copy retains it, approval is per inode") {
    TempDir dir;
    const auto file = dir.write("tool", "binary");
    const bool xattrs = rummage::test::user_xattrs_available(file);
    const int fd = open(file.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    REQUIRE(fchmod(fd, 0755) == 0);
    CHECK_FALSE(mark_archive_origin(fd, "https://example.test/download.zip"));
    CHECK(archive_confirmation_required(file) == xattrs);
    CHECK(archive_confirmation_required(fd) == xattrs);
    struct stat info{};
    REQUIRE(fstat(fd, &info) == 0);
    CHECK((info.st_mode & 0777) == (xattrs ? 0755 : 0644));
    fs::create_directory(dir.path / "copied");
    const auto result = run_transfer(
        {.source = dir.path, .names = {"tool"}, .destination = dir.path / "copied"}, {}, {});
    CHECK(result.errors.empty());
    CHECK(archive_confirmation_required(dir.path / "copied/tool") == xattrs);
    REQUIRE(stat((dir.path / "copied/tool").c_str(), &info) == 0);
    CHECK((info.st_mode & 0777) == (xattrs ? 0755 : 0644));
    fs::rename(file, dir.path / "original");
    dir.write("tool", "replacement");
    const int replacement = open(file.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE(replacement >= 0);
    CHECK_FALSE(mark_archive_origin(replacement, "archive"));
    CHECK_FALSE(approve_archive_program(fd));
    CHECK_FALSE(archive_confirmation_required(dir.path / "original"));
    CHECK_FALSE(archive_confirmation_required(fd));
    CHECK(archive_confirmation_required(file) == xattrs);
    CHECK(archive_confirmation_required(dir.path / "copied/tool") == xattrs);
    REQUIRE(stat((dir.path / "copied/tool").c_str(), &info) == 0);
    CHECK((info.st_mode & 0777) == (xattrs ? 0755 : 0644));
    close(replacement);
    close(fd);
}

TEST_CASE("archive origin: unwritable xattrs fall back to nonexecutable, never undone by copy") {
    TempDir dir;
    const auto file = dir.write("readonly", "binary");
    const int fd = open(file.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    REQUIRE(fchmod(fd, 0555) == 0);
    // The owner cannot write user xattrs without file write permission (Linux inode_permission).
    if (fsetxattr(fd, "user.rummage.test", "x", 1, 0) == 0) {
        close(fd);
        MESSAGE(
            "privileged environment bypasses xattr write permission; fallback fixture unavailable");
        return;
    }
    CHECK_FALSE(mark_archive_origin(fd, "archive"));
    struct stat info{};
    REQUIRE(fstat(fd, &info) == 0);
    CHECK((info.st_mode & 0111) == 0);
    fs::create_directory(dir.path / "out");
    const auto result = run_transfer(
        {.source = dir.path, .names = {"readonly"}, .destination = dir.path / "out"}, {}, {});
    CHECK(result.errors.empty());
    REQUIRE(stat((dir.path / "out/readonly").c_str(), &info) == 0);
    CHECK((info.st_mode & 0111) == 0);
    close(fd);
}

TEST_CASE("archive origin: tree marking ignores symlinks and cancellation prevents publication") {
    TempDir dir;
    fs::create_directories(dir.path / "tree/nested");
    const auto nested = dir.write("tree/nested/run", "binary");
    const auto outside = dir.write("outside", "untouched");
    const bool xattrs = rummage::test::user_xattrs_available(nested);
    fs::permissions(nested, fs::perms::owner_all);
    fs::permissions(outside, fs::perms::owner_all);
    fs::create_symlink(outside, dir.path / "tree/link");
    CHECK_FALSE(mark_archive_tree(dir.path / "tree", "archive", {}));
    CHECK(archive_confirmation_required(nested) == xattrs);
    CHECK((fs::status(nested).permissions() & fs::perms::owner_exec) ==
          (xattrs ? fs::perms::owner_exec : fs::perms::none));
    CHECK((fs::status(outside).permissions() & fs::perms::owner_exec) != fs::perms::none);
    CHECK_FALSE(archive_confirmation_required(outside));
    rummage::core::CancelToken token;
    token.cancel();
    CHECK(mark_archive_tree(dir.path / "tree", "archive", token) == std::errc::operation_canceled);
}

TEST_CASE(
    "archive origin: final copy mode cannot undo fallback for a marked read-only executable") {
    TempDir dir;
    const auto file = dir.write("marked", "binary");
    const bool xattrs = rummage::test::user_xattrs_available(file);
    const int fd = open(file.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    REQUIRE_FALSE(mark_archive_origin(fd, "archive"));
    // Only retain execute bits when provenance was actually stored. Setting them
    // after the no-xattr fallback would simulate explicit user retrusting instead.
    REQUIRE(fchmod(fd, xattrs ? 0555 : 0444) == 0);
    CHECK(archive_confirmation_required(fd) == xattrs);
    fs::create_directory(dir.path / "out");
    const auto result = run_transfer(
        {.source = dir.path, .names = {"marked"}, .destination = dir.path / "out"}, {}, {});
    CHECK(result.errors.empty());
    struct stat info{};
    REQUIRE(stat((dir.path / "out/marked").c_str(), &info) == 0);
    CHECK(((info.st_mode & 0111) == 0 || archive_confirmation_required(dir.path / "out/marked")));
    close(fd);
}
