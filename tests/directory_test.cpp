#include "rummage/fs/directory.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <vector>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/sort.hpp"
#include "temp_dir.hpp"

using rummage::core::CancelToken;
using rummage::core::EventLoop;
using rummage::core::WorkerPool;
using rummage::fs::Entry;
using rummage::fs::EntryKind;
using rummage::fs::Listing;
using rummage::fs::load_directory;
using rummage::fs::read_directory;
using rummage::fs::ReadOptions;
using rummage::fs::ReadResult;
using rummage::test::TempDir;
using namespace std::chrono_literals;

namespace {

std::vector<Entry> read_all(const std::filesystem::path& path, ReadResult& result,
                            std::vector<std::size_t>* chunk_sizes = nullptr,
                            const ReadOptions& options = {}) {
    std::vector<Entry> entries;
    result = read_directory(path, CancelToken{}, options, [&](std::vector<Entry> chunk) {
        if (chunk_sizes != nullptr) {
            chunk_sizes->push_back(chunk.size());
        }
        std::ranges::move(chunk, std::back_inserter(entries));
    });
    std::ranges::sort(entries, rummage::fs::SortOrder{});
    return entries;
}

const Entry* find(const std::vector<Entry>& entries, std::string_view name) {
    const auto it = std::ranges::find(entries, name, &Entry::name);
    return it == entries.end() ? nullptr : &*it;
}

}  // namespace

TEST_CASE("read_directory: kinds, sizes, links, hidden files") {
    const TempDir dir;
    dir.write("hello.txt", "hello");
    dir.write(".hidden", "");
    std::filesystem::create_directory(dir.path / "sub");
    std::filesystem::create_symlink("hello.txt", dir.path / "link-to-file");
    std::filesystem::create_directory_symlink("sub", dir.path / "link-to-dir");
    std::filesystem::create_symlink("missing", dir.path / "dangling");

    ReadResult result;
    const std::vector<Entry> entries = read_all(dir.path, result);
    CHECK_FALSE(result.error);
    CHECK(result.count == 6);
    REQUIRE(entries.size() == 6);

    const Entry* hello = find(entries, "hello.txt");
    REQUIRE(hello != nullptr);
    CHECK(hello->kind == EntryKind::file);
    CHECK(hello->size == 5);
    CHECK(hello->modified_ns > 0);
    CHECK_FALSE(hello->is_link);
    CHECK(find(entries, ".hidden")->hidden());
    CHECK(find(entries, "sub")->is_directory());

    const Entry* link_dir = find(entries, "link-to-dir");
    CHECK(link_dir->is_directory());
    CHECK(link_dir->is_link);
    CHECK(find(entries, "link-to-file")->kind == EntryKind::file);
    CHECK(find(entries, "link-to-file")->size == 5);
    CHECK(find(entries, "dangling")->kind == EntryKind::other);
    CHECK(find(entries, "dangling")->is_link);
}

TEST_CASE("read_directory: chunks grow from first_chunk to max_chunk") {
    const TempDir dir;
    for (int i = 0; i < 100; ++i) {
        dir.write("f" + std::to_string(i), "");
    }
    ReadResult result;
    std::vector<std::size_t> sizes;
    const auto entries = read_all(dir.path, result, &sizes, {.first_chunk = 4, .max_chunk = 16});
    CHECK(entries.size() == 100);
    CHECK(sizes == std::vector<std::size_t>{4, 8, 16, 16, 16, 16, 16, 8});
}

TEST_CASE("read_directory: errors") {
    const TempDir dir;
    ReadResult result;
    CHECK(read_all(dir.path / "absent", result).empty());
    CHECK(result.error == std::errc::no_such_file_or_directory);
    dir.write("file", "");
    CHECK(read_all(dir.path / "file", result).empty());
    CHECK(result.error == std::errc::not_a_directory);
}

TEST_CASE("read_directory: a cancelled read stops and delivers nothing more") {
    const TempDir dir;
    for (int i = 0; i < 500; ++i) {
        dir.write("f" + std::to_string(i), "");
    }
    const CancelToken token;
    std::size_t delivered = 0;
    read_directory(dir.path, token, {.first_chunk = 10, .max_chunk = 10},
                   [&](const std::vector<Entry>& chunk) {
                       delivered += chunk.size();
                       token.cancel();
                   });
    CHECK(delivered == 10);
}

TEST_CASE("load_directory: chunks on the loop thread, then done, into a sorted Listing") {
    const TempDir dir;
    for (int i = 0; i < 300; ++i) {
        dir.write("IMG_" + std::to_string(i) + ".ORF", "");
    }
    EventLoop loop;
    loop.add_timer(10s, 0ms, [&loop] {
        FAIL_CHECK("event loop watchdog fired");
        loop.quit(-1);
    });
    WorkerPool pool{loop, 2};
    Listing listing;
    int chunks = 0;
    bool done_after_chunks = false;
    load_directory(
        pool, dir.path,
        [&](std::vector<Entry> chunk) {
            ++chunks;
            listing.add(std::move(chunk));
        },
        [&](const ReadResult& result) {
            CHECK_FALSE(result.error);
            CHECK(result.count == 300);
            done_after_chunks = listing.entries().size() == 300;
            loop.quit(0);
        },
        {.first_chunk = 16, .max_chunk = 64});
    CHECK(loop.run() == 0);
    CHECK(done_after_chunks);
    CHECK(chunks == 6);  // 16 32 64 64 64 60
    CHECK(listing.entries().front().name == "IMG_0.ORF");
    CHECK(listing.entries().back().name == "IMG_299.ORF");
}

TEST_CASE(
    "read_directory: property columns include permissions, identities, dates and link targets") {
    const TempDir dir;
    dir.write("file", "contents");
    std::filesystem::permissions(dir.path / "file", std::filesystem::perms::owner_read |
                                                        std::filesystem::perms::owner_write);
    std::filesystem::create_symlink("file", dir.path / "link");
    std::filesystem::create_symlink("missing", dir.path / "dangling");
    ReadResult result;
    const auto entries = read_all(dir.path, result);
    REQUIRE_FALSE(result.error);
    const auto* file = find(entries, "file");
    REQUIRE(file);
    CHECK(file->permissions == "-rw-------");
    CHECK_FALSE(file->owner.empty());
    CHECK_FALSE(file->group.empty());
    REQUIRE(file->accessed_ns);
    CHECK(*file->accessed_ns > 0);
    const auto* link = find(entries, "link");
    REQUIRE(link);
    CHECK(link->link_target == "file");
    CHECK(link->permissions == file->permissions);
    const auto* dangling = find(entries, "dangling");
    REQUIRE(dangling);
    CHECK(dangling->link_target == "missing");
    CHECK(dangling->permissions.starts_with('l'));
    // Reloads use the same metadata path as the initial directory chunks.
    std::filesystem::permissions(dir.path / "file", std::filesystem::perms::owner_read);
    const auto refreshed = rummage::fs::read_entries(dir.path, {"file"}, {});
    REQUIRE(refreshed[0]);
    CHECK(refreshed[0]->permissions == "-r--------");
    CHECK(refreshed[0]->owner == file->owner);
}

TEST_CASE("file_permissions: special bits format without changing filesystem permissions") {
    // Nix builds forbid creating setuid/setgid files. Exercise their display directly,
    // while the directory test above verifies real metadata with ordinary permissions.
    using rummage::fs::file_permissions;
    CHECK(file_permissions(S_IFREG | S_IRUSR | S_IWUSR | S_ISUID) == "-rwS------");
    CHECK(file_permissions(S_IFREG | S_IRWXU | S_ISUID) == "-rws------");
    CHECK(file_permissions(S_IFREG | S_IRUSR | S_IWUSR | S_IRGRP | S_ISGID) == "-rw-r-S---");
    CHECK(file_permissions(S_IFREG | S_IRUSR | S_IWUSR | S_IRGRP | S_IXGRP | S_ISGID) ==
          "-rw-r-s---");
    CHECK(file_permissions(S_IFDIR | S_IRWXU | S_ISVTX) == "drwx-----T");
    CHECK(file_permissions(S_IFDIR | S_IRWXU | S_IXOTH | S_ISVTX) == "drwx-----t");
}
