#include "rummage/fs/annotations.hpp"

#include <filesystem>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <doctest/doctest.h>

#include "temp_dir.hpp"
#include "xattr_support.hpp"
using namespace rummage::fs;
using rummage::test::TempDir;
namespace fs = std::filesystem;

TEST_CASE("annotations: selected fields preserve others and detect external changes") {
    const TempDir dir;
    const auto file = dir.write("file", "data");
    const auto target = permission_target(file);
    const Annotations value{
        .tags = "travel, blue", .rating = "4", .comment = "Trip notes", .error = {}};
    if (!rummage::test::user_xattrs_available(file)) {
        CHECK_FALSE(read_annotations(file).error.empty());
        CHECK_FALSE(write_annotations(target, {}, value, 7).empty());
        const auto after = read_annotations(file);
        CHECK(after.tags.empty());
        CHECK(after.rating.empty());
        CHECK(after.comment.empty());
        CHECK_FALSE(after.error.empty());
        CHECK(fs::file_size(file) == 4);
        return;
    }
    REQUIRE(write_annotations(target, read_annotations(file), value, 7).empty());
    CHECK(read_annotations(file) == value);
    auto changed = value;
    changed.comment = "Updated";
    REQUIRE(write_annotations(target, value, changed, kAnnotationComment).empty());
    CHECK(read_annotations(file).tags == value.tags);
    CHECK_FALSE(write_annotations(target, value, {}, kAnnotationComment).empty());
    CHECK(read_annotations(file).comment == "Updated");
    changed.rating = "6";
    CHECK_FALSE(
        write_annotations(target, read_annotations(file), changed, kAnnotationRating).empty());
    REQUIRE(write_annotations(target, read_annotations(file), {}, 7).empty());
    CHECK(read_annotations(file) == Annotations{});
}

TEST_CASE("annotations: native attributes survive rename and reject links and replacement") {
    const TempDir dir;
    const auto file = dir.write("file", "data");
    const bool xattrs = rummage::test::user_xattrs_available(file);
    if (xattrs) {
        REQUIRE(::setxattr(file.c_str(), "user.xdg.tags", "external", 8, 0) == 0);
        CHECK(read_annotations(file).tags == "external");
    } else {
        CHECK_FALSE(read_annotations(file).error.empty());
    }
    const auto target = permission_target(file);
    fs::rename(file, dir.path / "renamed");
    CHECK(read_annotations(dir.path / "renamed").tags == (xattrs ? "external" : ""));
    CHECK(read_annotations(dir.path / "renamed").error.empty() == xattrs);
    dir.write("file", "replacement");
    CHECK_FALSE(write_annotations(target, {}, {}, 7).empty());
    fs::create_symlink(dir.path / "renamed", dir.path / "link");
    CHECK_FALSE(read_annotations(dir.path / "link/").error.empty());
    CHECK_FALSE(write_annotations(permission_target(dir.path / "link"), {}, {}, 7).empty());
    CHECK(read_annotations(dir.path / "renamed").tags == (xattrs ? "external" : ""));
    CHECK(read_annotations(dir.path / "renamed").error.empty() == xattrs);
}

TEST_CASE("annotations: read-only item reports a write failure without inventing persistence") {
    if (::geteuid() == 0)
        return;
    const TempDir dir;
    const auto file = dir.write("file", "data");
    REQUIRE(::chmod(file.c_str(), 0400) == 0);
    CHECK_FALSE(write_annotations(permission_target(file), {},
                                  {.tags = "denied", .rating = {}, .comment = {}, .error = {}}, 1)
                    .empty());
    CHECK(read_annotations(file).tags.empty());
}
