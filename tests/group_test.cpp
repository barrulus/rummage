#include "rummage/fs/group.hpp"

#include <fstream>

#include <doctest/doctest.h>

#include "temp_dir.hpp"
namespace fs = std::filesystem;
namespace rfs = rummage::fs;
using rummage::test::TempDir;

TEST_CASE("group: move with sidecars, undo removes empty folder, redo recreates it") {
    TempDir temp;
    temp.write("P1.ORF", "photo");
    temp.write("P1.ORF.xmp", "sidecar");
    fs::create_directory(temp.path / "sub");
    rfs::TransferRequest request;
    request.kind = rfs::TransferKind::group;
    request.source = temp.path;
    request.names = {"P1.ORF", "sub"};
    request.destination = temp.path / "Selected";
    auto result = rfs::run_group(request, {}, {});
    REQUIRE(result.refused.empty());
    REQUIRE(result.errors.empty());
    REQUIRE(result.items.size() == 2);
    CHECK(fs::exists(request.destination / "P1.ORF.xmp"));
    CHECK_FALSE(fs::exists(temp.path / "P1.ORF"));
    for (const auto& item : result.items)
        CHECK(item.to.parent_path() == request.destination);
    request.group_folder = result.group_folder;
    std::swap(request.source, request.destination);
    request.ungroup = true;
    result = rfs::run_group(request, {}, {});
    REQUIRE(result.refused.empty());
    REQUIRE(result.errors.empty());
    CHECK_FALSE(fs::exists(request.source));
    CHECK(fs::exists(temp.path / "P1.ORF.xmp"));
    request.group_folder = result.group_folder;
    std::swap(request.source, request.destination);
    request.ungroup = false;
    result = rfs::run_group(request, {}, {});
    REQUIRE(result.refused.empty());
    CHECK(result.items.size() == 2);
    CHECK(fs::exists(request.destination / "P1.ORF.xmp"));
}

TEST_CASE("group: occupied and replaced folders are refused, added contents survive undo") {
    TempDir temp;
    temp.write("a", "a");
    rfs::TransferRequest request;
    request.kind = rfs::TransferKind::group;
    request.source = temp.path;
    request.names = {"a"};
    request.destination = temp.path / "Selected";
    fs::create_directory(request.destination);
    CHECK_FALSE(rfs::run_group(request, {}, {}).refused.empty());
    CHECK(fs::exists(temp.path / "a"));
    fs::remove(request.destination);
    const auto made = rfs::run_group(request, {}, {});
    REQUIRE(made.items.size() == 1);
    std::ofstream{request.destination / "new"} << "keep";
    request.group_folder = made.group_folder;
    std::swap(request.source, request.destination);
    request.ungroup = true;
    const auto undone = rfs::run_group(request, {}, {});
    CHECK(undone.items.size() == 1);
    CHECK(fs::exists(request.source / "new"));
    CHECK(fs::exists(temp.path / "a"));
    fs::rename(request.source, temp.path / "Original");
    fs::create_directory(request.source);
    CHECK_FALSE(rfs::run_group(request, {}, {}).refused.empty());
}

TEST_CASE("group: cancelled or failed moves do not leave empty scaffolding") {
    TempDir temp;
    temp.write("a", "a");
    rfs::TransferRequest request;
    request.kind = rfs::TransferKind::group;
    request.source = temp.path;
    request.names = {"missing"};
    request.destination = temp.path / "Selected";
    auto result = rfs::run_group(request, {}, {});
    CHECK_FALSE(result.errors.empty());
    CHECK_FALSE(fs::exists(request.destination));
    rummage::core::CancelToken cancelled;
    cancelled.cancel();
    result = rfs::run_group(request, cancelled, {});
    CHECK(result.cancelled);
    CHECK_FALSE(fs::exists(request.destination));
    for (const auto& entry : fs::directory_iterator(temp.path))
        CHECK_FALSE(entry.path().filename().string().starts_with(".rummage-group-"));
}
