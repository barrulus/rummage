#include "rummage/fs/filesystem.hpp"

#include <doctest/doctest.h>

#include "temp_dir.hpp"

TEST_CASE("filesystem: mount facts choose the deepest component match and decode escapes") {
    const std::string mounts =
        "1 0 8:1 / / rw - ext4 /dev/root rw\n"
        "2 1 8:2 / /media/disk rw - btrfs /dev/sdb rw\n"
        "3 2 8:2 /other /media/disk/My\\040Folder ro - btrfs /dev/sdb ro\n";
    auto facts = rummage::fs::filesystem_mount(mounts, "/media/disk/My Folder/sub");
    CHECK(facts.mount_point == "/media/disk/My Folder");
    CHECK(facts.type == "btrfs");
    CHECK(facts.device == "/dev/sdb");
    CHECK(rummage::fs::filesystem_mount(mounts, "/media/diskette").mount_point == "/");
    CHECK(rummage::fs::filesystem_mount(mounts, "/").mount_point == "/");
    CHECK(rummage::fs::filesystem_mount("invalid\n", "/a").mount_point.empty());
}

TEST_CASE("filesystem: capacity query reads live facts and reports missing targets") {
    rummage::test::TempDir dir;
    const auto facts = rummage::fs::filesystem_facts(dir.path);
    REQUIRE_FALSE(facts.error);
    CHECK(facts.capacity > 0);
    CHECK(facts.available <= facts.free);
    CHECK(facts.free <= facts.capacity);
    CHECK_FALSE(facts.type.empty());
    std::filesystem::create_directory_symlink(dir.path, dir.path / "alias");
    const auto alias = rummage::fs::filesystem_facts(dir.path / "alias");
    CHECK(alias.mount_point == facts.mount_point);
    CHECK(alias.capacity == facts.capacity);
    CHECK(rummage::fs::filesystem_facts(dir.path / "missing").error);
}

TEST_CASE("filesystem: malformed mount records cannot hide a valid parent mount") {
    const std::string root = "1 0 8:1 / / rw - ext4 /dev/root rw\n";
    for (const std::string bad : {"", "-", "1 0", "2 1 8:2 / relative rw - ext4 /dev/x rw",
                                  "2 1 8:2 / /deep rw shared:1", "2 1 8:2 / /deep rw -"}) {
        CHECK(rummage::fs::filesystem_mount(root + bad + '\n', "/deep/child").mount_point == "/");
    }
    const auto facts = rummage::fs::filesystem_mount(
        root + "2 1 8:2 / /deep rw shared:1 master:2 - ext4 /dev/label\\134disk rw\n", "/deep");
    CHECK(facts.mount_point == "/deep");
    CHECK(facts.device == "/dev/label\\disk");
    CHECK(rummage::fs::filesystem_mount(root, "relative").mount_point.empty());
}
