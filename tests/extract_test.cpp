// Extracting from an archive (ROADMAP 0.9.0, step 4): run_extract with the engine in this process
// (tests/main.cpp), and Ctrl+C, Ctrl+V and "Extract Here" in the Browser.
#include "rummage/fs/extract.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/archive_folder.hpp"
#include "rummage/fs/archive_origin.hpp"
#include "rummage/fs/compress.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/fs/transfer.hpp"
#include "rummage/views/browser.hpp"
#include "temp_dir.hpp"

using rummage::fs::run_extract;
using rummage::fs::TransferCallbacks;
using rummage::fs::TransferKind;
using rummage::fs::TransferResult;
using rummage::test::TempDir;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

const fs::path kArchives = fs::path{RUMMAGE_TEST_DATA} / "archive";

// A ustar member: type '0' file, '1' hard link, '2' symbolic link, '5' folder.
std::string tar_member(std::string_view name, std::string_view data, char type = '0',
                       std::string_view link = {}, unsigned mode = 0644) {
    std::string block(512, '\0');
    std::ranges::copy(name, block.begin());
    const auto octal = [&block](std::size_t at, std::size_t width, std::uint64_t value) {
        std::string text(width - 1, '0');
        for (std::size_t i = width - 1; i-- > 0 && value > 0; value >>= 3U) {
            text[i] = static_cast<char>('0' + (value & 7U));
        }
        std::ranges::copy(text, block.begin() + static_cast<std::ptrdiff_t>(at));
    };
    octal(100, 8, mode);
    octal(124, 12, type == '0' ? data.size() : 0);
    octal(136, 12, 1794744000);
    block[156] = type;
    std::ranges::copy(link, block.begin() + 157);
    std::ranges::copy(std::string_view{"ustar\0"
                                       "00",
                                       8},
                      block.begin() + 257);
    std::ranges::fill(block.begin() + 148, block.begin() + 156, ' ');
    unsigned sum = 0;
    for (const char c : block) {
        sum += static_cast<unsigned char>(c);
    }
    octal(148, 7, sum);
    std::string out = block;
    if (type == '0') {
        out += data;
        out.append((512 - (data.size() % 512)) % 512, '\0');
    }
    return out;
}

std::string tar_end() { return std::string(1024, '\0'); }

std::string read_file(const fs::path& path) {
    std::error_code error;
    const std::uintmax_t size = fs::file_size(path, error);
    if (error) {
        return {};
    }
    std::ifstream in{path, std::ios::binary};
    std::string bytes(static_cast<std::size_t>(size), '\0');
    in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

std::vector<std::string> names_in(const fs::path& folder) {
    std::vector<std::string> names;
    for (const fs::directory_entry& entry : fs::directory_iterator{folder}) {
        names.push_back(entry.path().filename().string());
    }
    std::ranges::sort(names);
    return names;
}

// No staging folder is left behind.
bool no_staging(const fs::path& folder) {
    return std::ranges::none_of(names_in(folder), [](const std::string& name) {
        return name.starts_with(".rummage-extract-");
    });
}

TransferResult extract(const fs::path& source, std::vector<std::string> names,
                       const fs::path& destination, std::string into = {},
                       const TransferCallbacks& callbacks = {}) {
    rummage::fs::forget_archive_trees();
    const rummage::core::CancelToken token;
    return run_extract({.kind = TransferKind::copy,
                        .source = source,
                        .names = std::move(names),
                        .destination = destination,
                        .force_copy = false,
                        .extract_into = std::move(into)},
                       token, callbacks);
}

bool run_until(rummage::core::EventLoop& loop, const std::function<bool()>& done) {
    bool ok = false;
    const auto poll = loop.add_timer(1ms, 1ms, [&] {
        if (done()) {
            ok = true;
            loop.quit(0);
        }
    });
    const auto watchdog = loop.add_timer(5s, 0ms, [&loop] { loop.quit(1); });
    loop.run();
    loop.remove(poll);
    loop.remove(watchdog);
    return ok;
}

}  // namespace

TEST_CASE("extract: a whole zip into a folder named after it, as it was") {
    TempDir dir;
    fs::copy_file(kArchives / "basic.zip", dir.path / "basic.zip");
    fs::create_directories(dir.path / "out");
    const TransferResult result =
        extract(dir.path / "basic.zip", {"a.txt", "città.txt", "docs"}, dir.path / "out", "basic");
    CHECK(result.refused.empty());
    CHECK(result.errors.empty());
    CHECK(result.placed == std::vector<std::string>{"basic"});
    CHECK(result.files_done == 5);
    CHECK(result.bytes_done == 13 + 8 + 12 + 12 + 5120);
    const fs::path out = dir.path / "out" / "basic";
    CHECK(names_in(out) == std::vector<std::string>{"a.txt", "città.txt", "docs"});
    CHECK(read_file(out / "a.txt") == "hello from a\n");
    CHECK(read_file(out / "docs" / "repeat.txt").size() == 5120);
    CHECK(no_staging(dir.path / "out"));
    REQUIRE(result.items.size() == 1);
    CHECK(result.items.front().from == dir.path / "basic.zip");
    CHECK(result.items.front().to == out);

    // Again: the name is taken, it gets "basic 2"; nothing is replaced.
    CHECK(extract(dir.path / "basic.zip", {"a.txt", "docs"}, dir.path / "out", "basic").placed ==
          std::vector<std::string>{"basic 2"});
    // A file from a folder inside, next to one of the same name: "a 2.txt".
    std::ofstream{dir.path / "out" / "b.txt"} << "mine";
    const TransferResult one =
        extract(dir.path / "basic.zip" / "docs", {"b.txt"}, dir.path / "out");
    CHECK(one.placed == std::vector<std::string>{"b 2.txt"});
    CHECK(read_file(dir.path / "out" / "b.txt") == "mine");
    CHECK(read_file(dir.path / "out" / "b 2.txt") == "second file\n");
    CHECK(no_staging(dir.path / "out"));

    // Into a folder reached through a link (a folder on another disk): followed, as a copy is.
    fs::create_directory_symlink(dir.path / "out", dir.path / "linked");
    CHECK(extract(dir.path / "basic.zip", {"a.txt"}, dir.path / "linked").placed ==
          std::vector<std::string>{"a.txt"});
    CHECK(read_file(dir.path / "out" / "a.txt") == "hello from a\n");
}

TEST_CASE("extract: tar.gz, times and modes kept, setuid dropped") {
    TempDir dir;
    fs::copy_file(kArchives / "pax.tar.gz", dir.path / "pax.tar.gz");
    std::string tar = tar_member("run.sh", "#!/bin/sh\n", '0', {}, 04755) +
                      tar_member("secret", "x", '0', {}, 0) + tar_end();
    dir.write("modes.tar", tar);
    const TransferResult gz = extract(dir.path / "pax.tar.gz", {"a.txt", "docs"}, dir.path);
    CHECK(gz.errors.empty());
    CHECK(gz.placed == std::vector<std::string>{"a.txt", "docs"});
    struct stat info{};
    REQUIRE(::stat((dir.path / "a.txt").c_str(), &info) == 0);
    CHECK(info.st_mtim.tv_sec == 1794744000);
    CHECK((info.st_mode & 07777) == 0644);

    const TransferResult modes = extract(dir.path / "modes.tar", {"run.sh", "secret"}, dir.path);
    CHECK(modes.errors.empty());
    REQUIRE(::stat((dir.path / "run.sh").c_str(), &info) == 0);
    CHECK((info.st_mode & 07777) == 0755);  // no setuid
    REQUIRE(::stat((dir.path / "secret").c_str(), &info) == 0);
    CHECK((info.st_mode & 0400) != 0);  // readable by its owner
}

TEST_CASE("extract: parent components cannot escape through archive symlink chains") {
    TempDir dir;
    const std::string a = tar_member("top/a", "", '2', ".");
    const std::string b = tar_member("top/b", "", '2', "a/..");
    const std::string c = tar_member("top/c", "", '2', "b/..");
    for (const bool reverse : {false, true}) {
        CAPTURE(reverse);
        dir.write("chain.tar", tar_member("top/", "", '5') + (reverse ? c + b + a : a + b + c) +
                                   tar_member("top/safe", "", '2', "./a") +
                                   tar_member("top/dangling", "", '2', "missing") +
                                   tar_member("top/parent", "", '2', "a//../outside") + tar_end());
        const auto destination = dir.path / (reverse ? "reverse" : "forward");
        fs::create_directory(destination);
        const auto result = extract(dir.path / "chain.tar", {"top"}, destination);
        REQUIRE(result.refused.empty());
        CHECK(result.errors.size() == 3);
        for (const char* name : {"a", "safe", "dangling"})
            CHECK(fs::is_symlink(destination / "top" / name));
        for (const char* name : {"b", "c", "parent"})
            CHECK_FALSE(fs::exists(fs::symlink_status(destination / "top" / name)));
        CHECK(no_staging(destination));
    }
}

TEST_CASE("extract: links stay inside; nothing written goes through one") {
    TempDir dir;
    fs::create_directories(dir.path / "outside");
    std::string tar = tar_member("top/", "", '5') + tar_member("top/a.txt", "inside\n") +
                      tar_member("top/good", "", '2', "a.txt") +
                      tar_member("top/up", "", '2', "../top/a.txt") +
                      tar_member("top/escape", "", '2', "../../outside") +
                      tar_member("top/absolute", "", '2', "/etc/passwd") +
                      tar_member("top/same", "", '1', "top/a.txt") +
                      tar_member("top/lost", "", '1', "nowhere.txt") +
                      // A file "through" a link: written where its name says, never followed.
                      tar_member("top/escape/planted.txt", "no\n") + tar_end();
    dir.write("links.tar", tar);
    fs::create_directories(dir.path / "out");
    const TransferResult result = extract(dir.path / "links.tar", {"top"}, dir.path / "out");
    CHECK(result.placed == std::vector<std::string>{"top"});
    const fs::path top = dir.path / "out" / "top";
    CHECK(fs::is_symlink(top / "good"));
    CHECK(fs::read_symlink(top / "good") == "a.txt");
    // Up and in again is refused too: above the top, "top" may be another folder by then.
    CHECK_FALSE(fs::exists(fs::symlink_status(top / "up")));
    CHECK_FALSE(fs::exists(fs::symlink_status(top / "absolute")));
    CHECK(fs::hard_link_count(top / "a.txt") == 2);
    CHECK(read_file(top / "same") == "inside\n");
    CHECK_FALSE(fs::exists(fs::symlink_status(top / "lost")));
    // The tree makes "escape" a folder (a file under it): no link there, and nothing outside.
    CHECK(fs::is_empty(dir.path / "outside"));
    CHECK(read_file(top / "escape" / "planted.txt") == "no\n");
    const std::vector<std::string>& errors = result.errors;
    CHECK(std::ranges::any_of(errors, [](const std::string& error) {
        return error == "top/absolute: a link leading outside, left out";
    }));
    CHECK(std::ranges::any_of(errors, [](const std::string& error) {
        return error == "top/lost: a link to a file not extracted, left out";
    }));
    CHECK(no_staging(dir.path / "out"));
}

TEST_CASE("extract: encrypted entries are said and left out") {
    TempDir dir;
    fs::copy_file(kArchives / "aes.zip", dir.path / "aes.zip");
    fs::create_directories(dir.path / "out");
    const TransferResult result = extract(dir.path / "aes.zip", {"a.txt"}, dir.path / "out");
    CHECK(result.placed.empty());
    CHECK(result.errors == std::vector<std::string>{"a.txt: encrypted, left out"});
    CHECK(names_in(dir.path / "out").empty());
}

TEST_CASE("extract: refused into an archive, or when not in one; a question can stop it") {
    TempDir dir;
    fs::copy_file(kArchives / "basic.zip", dir.path / "basic.zip");
    CHECK(extract(dir.path / "basic.zip", {"a.txt"}, dir.path / "basic.zip" / "docs").refused ==
          "Nothing inside an archive can be changed");
    fs::create_directories(dir.path / "plain");
    CHECK(extract(dir.path / "plain", {"x"}, dir.path).refused == "Not inside an archive");

    // Cancelled before it starts: nothing published, nothing left behind.
    fs::create_directories(dir.path / "out");
    rummage::core::CancelToken token;
    token.cancel();
    const TransferResult stopped = run_extract({.kind = TransferKind::copy,
                                                .source = dir.path / "basic.zip",
                                                .names = {"a.txt", "docs"},
                                                .destination = dir.path / "out",
                                                .force_copy = false},
                                               token, {});
    CHECK(stopped.cancelled);
    CHECK(names_in(dir.path / "out").empty());
}

TEST_CASE("extract: Ctrl+C inside an archive, Ctrl+V outside; Extract Here") {
    TempDir dir;
    fs::copy_file(kArchives / "basic.zip", dir.path / "basic.zip");
    fs::copy_file(kArchives / "notes.txt.gz", dir.path / "notes.txt.gz");
    fs::create_directories(dir.path / "out");
    rummage::fs::forget_archive_trees();
    rummage::core::EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    rummage::views::Browser browser{loop,    pool,
                                    watcher, rummage::fs::MimeDatabase::load(),
                                    [] {},   [](rummage::ui::CursorShape /*shape*/) {}};
    browser.set_home_trash(dir.path / "Trash");
    const auto settle = [&] { return run_until(loop, [&] { return !browser.loading(); }); };
    const auto press = [&](xkb_keysym_t keysym, rummage::ui::Modifiers modifiers = {}) {
        rummage::ui::KeyEvent event;
        event.keysym = keysym;
        event.modifiers = modifiers;
        browser.key(event, rummage::views::Browser::Clock::now());
    };

    browser.go_to(dir.path / "basic.zip", "docs");
    REQUIRE(settle());
    press(XKB_KEY_c, {.ctrl = true});
    browser.go_to(dir.path / "out", "");
    REQUIRE(settle());
    press(XKB_KEY_v, {.ctrl = true});
    REQUIRE(run_until(loop, [&] { return fs::exists(dir.path / "out" / "docs" / "b.txt"); }));
    REQUIRE(run_until(loop, [&] { return !browser.status_bar().transfer(); }));
    CHECK(browser.status_bar().message() == "Extracted 3 files (5.1 kB)");
    CHECK(read_file(dir.path / "out" / "docs" / "b.txt") == "second file\n");

    // Full-archive extraction stays covered by the external-backend integration tests.
}

TEST_CASE("extract: ZIP and tar.gz executable members retain provenance before publication") {
    for (const bool tar_gz : {false, true}) {
        TempDir dir;
        const auto file = dir.write("program", "ELF fixture");
        fs::permissions(file, fs::perms::owner_all);
        const auto made =
            rummage::fs::run_compress({.kind = TransferKind::compress,
                                       .source = dir.path,
                                       .names = {"program"},
                                       .destination = dir.path,
                                       .archive_name = tar_gz ? "test.tar.gz" : "test.zip",
                                       .tar_gz = tar_gz},
                                      {}, {});
        REQUIRE(made.placed.size() == 1);
        fs::create_directory(dir.path / "out");
        const auto unpacked =
            extract(dir.path / made.placed.front(), {"program"}, dir.path / "out");
        CHECK(unpacked.errors.empty());
        CHECK(rummage::fs::archive_confirmation_required(dir.path / "out/program"));
        CHECK((fs::status(dir.path / "out/program").permissions() & fs::perms::owner_exec) !=
              fs::perms::none);
    }
}
