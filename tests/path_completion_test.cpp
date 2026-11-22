#include "rummage/fs/path_completion.hpp"

#include <doctest/doctest.h>

#include "rummage/fs/places.hpp"
#include "temp_dir.hpp"
namespace fs = std::filesystem;
namespace rfs = rummage::fs;
using rummage::test::TempDir;

TEST_CASE("path completion: Unicode, hidden entries, relative paths and local URIs") {
    TempDir temp;
    fs::create_directory(temp.path / "Été");
    fs::create_directory(temp.path / "École");
    temp.write(".hidden", "");
    temp.write("notes.txt", "text");
    temp.write("a b", "text");
    auto matches = rfs::complete_path("~/É", temp.path, temp.path, {});
    REQUIRE(matches.error.empty());
    CHECK(matches.paths == std::vector<std::string>{(temp.path / "École").string() + "/",
                                                    (temp.path / "Été").string() + "/"});
    CHECK(rfs::complete_path("notes", temp.path, temp.path, {}).paths ==
          std::vector<std::string>{(temp.path / "notes.txt").string()});
    CHECK(rfs::complete_path("file://localhost" + temp.path.string() + "/a%20", temp.path,
                             temp.path, {})
              .paths == std::vector<std::string>{(temp.path / "a b").string()});
    CHECK(rfs::complete_path(temp.path.string() + "/", temp.path, temp.path, {}).paths.size() == 4);
    CHECK(rfs::complete_path(temp.path.string() + "/.", temp.path, temp.path, {}).paths.size() ==
          1);
    CHECK_FALSE(rfs::complete_path("smb://host/share", temp.path, temp.path, {}).error.empty());
    CHECK_FALSE(rfs::parse_location("file:///tmp/a%00b", temp.path));
    CHECK_FALSE(rfs::parse_location("file:///tmp/%zz", temp.path));
    CHECK_FALSE(rfs::parse_location("file://remote/tmp", temp.path));
    const auto target = rfs::resolve_path(temp.path / "notes.txt");
    CHECK(target.folder == temp.path);
    CHECK(target.file == "notes.txt");
    CHECK_FALSE(rfs::resolve_path(temp.path / "missing").error.empty());
}

TEST_CASE("path completion: archive folders and members are navigable") {
    const fs::path path = fs::path{RUMMAGE_TEST_DATA} / "archive" / "basic.zip";
    const auto matches = rfs::complete_path(path.string() + "/", "/", "/", {});
    INFO(matches.error);
    REQUIRE_FALSE(matches.paths.empty());
    CHECK(rfs::resolve_path(path).folder == path);
    CHECK(rfs::resolve_path(path / "a.txt").file == "a.txt");
    CHECK(rfs::resolve_path(path / "a.txt").folder == path);
}
