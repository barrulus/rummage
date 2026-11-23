#include "rummage/fs/batch_rename.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <future>
#include <limits>
#include <poll.h>
#include <string>
#include <sys/inotify.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "rummage/fs/operations.hpp"
#include "rummage/ui/offscreen.hpp"
#include "rummage/views/batch_rename_dialog.hpp"
#include "rummage/views/browser.hpp"
#include "temp_dir.hpp"

namespace {
namespace fs = std::filesystem;
namespace ops = rummage::fs;
using rummage::test::TempDir;
using rummage::views::BatchRenameDialog;
using rummage::views::Browser;
using namespace std::chrono_literals;

std::string read(const fs::path& path) {
    std::ifstream input{path};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

ops::RenamePlan plan(const TempDir& dir, const std::vector<std::string>& names,
                     const ops::RenameOptions& options) {
    const auto snapshot = ops::rename_directory(dir.path);
    REQUIRE(snapshot.error.empty());
    return ops::plan_rename(snapshot.entries, names, options);
}

rummage::ui::KeyEvent key(xkb_keysym_t sym, std::string text = {},
                          rummage::ui::Modifiers mods = {}) {
    return {.keysym = sym, .text = std::move(text), .modifiers = mods};
}

bool until(rummage::core::EventLoop& loop, const std::function<bool()>& done) {
    bool ok = false;
    const auto timer = loop.add_timer(1ms, 1ms, [&] {
        if (done()) {
            ok = true;
            loop.quit(0);
        }
    });
    const auto timeout = loop.add_timer(5s, 0ms, [&] { loop.quit(1); });
    loop.run();
    loop.remove(timer);
    loop.remove(timeout);
    return ok;
}

struct WorkerGate {
    std::promise<void> promise;
    std::shared_future<void> ready{promise.get_future().share()};
    bool released{false};

    ~WorkerGate() { release(); }

    void release() {
        if (!released) {
            released = true;
            promise.set_value();
        }
    }

    void hold(rummage::core::WorkerPool& pool) {
        for (int i = 0; i < 2; ++i) {
            pool.submit(
                rummage::core::Priority::visible,
                [wait = ready](const auto&) {
                    wait.wait();
                    return true;
                },
                [](bool) {});
        }
    }
};

struct Fixture {
    TempDir dir;
    rummage::core::EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop, pool, watcher, rummage::fs::MimeDatabase::load(), [] {}, [](auto) {}};

    bool settle() {
        return until(loop, [&] { return !browser.loading(); });
    }

    void press(xkb_keysym_t sym, std::string text = {}, rummage::ui::Modifiers mods = {}) {
        CHECK(browser.key(key(sym, std::move(text), mods), Browser::Clock::now()));
    }
};
}  // namespace

TEST_CASE("batch rename: literal replacement, templates, numbering in supplied order") {
    TempDir dir;
    dir.write("Screenshot_A.png", "a");
    dir.write("Screenshot_B.png", "b");
    const auto result = plan(dir, {"Screenshot_B.png", "Screenshot_A.png"},
                             {.find = "Screenshot_",
                              .replace = "report-",
                              .pattern = "{name}-{n}{ext}",
                              .start = 3,
                              .step = 2,
                              .padding = 3});
    REQUIRE(result.error.empty());
    REQUIRE(result.changes.size() == 2);
    CHECK(result.changes[0].to == "report-B-003.png");
    CHECK(result.changes[1].to == "report-A-005.png");
    CHECK(fs::exists(dir.path / "Screenshot_A.png"));  // preview never writes
}

TEST_CASE("batch rename: directory names have no extension and take no photo sidecars") {
    TempDir dir;
    fs::create_directory(dir.path / "album.2026");
    dir.write("album.2026.xmp", "not a sidecar of a directory");
    const auto p = plan(dir, {"album.2026"}, {.pattern = "{name}-{n}{ext}"});
    REQUIRE(p.error.empty());
    REQUIRE(p.changes.size() == 1);
    CHECK(p.changes[0].to == "album.2026-01");
    REQUIRE(ops::rename_batch(dir.path, p.changes).error.empty());
    CHECK(fs::is_directory(dir.path / "album.2026-01"));
    CHECK(fs::exists(dir.path / "album.2026.xmp"));
    CHECK(plan(dir, {"album.2026-01"},
               {.target = ops::RenameOptions::Target::extension, .insert = "txt"})
              .changes.front()
              .to == "album.2026-01");
}

TEST_CASE("batch rename: target and ASCII case transformations preserve extensions and UTF-8") {
    using Options = ops::RenameOptions;
    TempDir dir;
    dir.write("Été PHOTO.JPG", "photo");
    auto options = Options{};
    const auto renamed = [&] {
        const auto result = plan(dir, {"Été PHOTO.JPG"}, options);
        REQUIRE(result.error.empty());
        return result.changes.front().to;
    };
    options.letter_case = Options::Case::lower;
    CHECK(renamed() == "Été photo.JPG");
    options.target = Options::Target::extension;
    CHECK(renamed() == "Été PHOTO.jpg");
    options.target = Options::Target::whole;
    CHECK(renamed() == "Été photo.jpg");
    options.letter_case = Options::Case::upper;
    CHECK(renamed() == "ÉTé PHOTO.JPG");
    options.letter_case = Options::Case::title;
    CHECK(renamed() == "Été Photo.Jpg");
    dir.write("trailing.", "x");
    CHECK(plan(dir, {"trailing."}, {}).changes.front().to == "trailing.");
    CHECK(plan(dir, {"trailing."}, {.target = Options::Target::whole}).changes.front().to ==
          "trailing.");
    dir.write(".hidden", "x");
    CHECK(plan(dir, {".hidden"}, {.letter_case = Options::Case::upper}).changes.front().to ==
          ".HIDDEN");
}

TEST_CASE("batch rename: character edits use scalar positions and clamp at the end") {
    TempDir dir;
    dir.write("aé😀z.txt", "x");
    const auto result = plan(dir, {"aé😀z.txt"}, {.position = 1, .remove = 2, .insert = "新"});
    REQUIRE(result.error.empty());
    CHECK(result.changes.front().to == "a新z.txt");
    const auto appended = plan(dir, {"aé😀z.txt"},
                               {.position = std::numeric_limits<std::uint64_t>::max(),
                                .remove = std::numeric_limits<std::uint64_t>::max(),
                                .insert = "!"});
    REQUIRE(appended.error.empty());
    CHECK(appended.changes.front().to == "aé😀z!.txt");
    CHECK_FALSE(plan(dir, {"aé😀z.txt"}, {.insert = "/"}).error.empty());
    const auto extension =
        plan(dir, {"aé😀z.txt"},
             {.target = ops::RenameOptions::Target::extension, .remove = 3, .insert = "md"});
    REQUIRE(extension.error.empty());
    CHECK(extension.changes.front().to == "aé😀z.md");
    const auto no_extension =
        plan(dir, {"aé😀z.txt"}, {.target = ops::RenameOptions::Target::extension, .remove = 3});
    REQUIRE(no_extension.error.empty());
    CHECK(no_extension.changes.front().to == "aé😀z");
}

TEST_CASE("batch rename: regex captures, invalid expressions and bounded replacement") {
    TempDir dir;
    dir.write("IMG_123.JPG", "x");
    auto options =
        ops::RenameOptions{.find = "^IMG_([0-9]+)$", .replace = "Photo-\\1", .regex = true};
    const auto result = plan(dir, {"IMG_123.JPG"}, options);
    REQUIRE(result.error.empty());
    CHECK(result.changes.front().to == "Photo-123.JPG");
    options.replace = "\\2";
    CHECK_FALSE(plan(dir, {"IMG_123.JPG"}, options).error.empty());
    options.find = "[";
    CHECK_FALSE(plan(dir, {"IMG_123.JPG"}, options).error.empty());
    options.find = "(IMG)\\1";
    CHECK_FALSE(plan(dir, {"IMG_123.JPG"}, options).error.empty());
    options.find = ".";
    options.replace = std::string(255, 'x');
    CHECK_FALSE(plan(dir, {"IMG_123.JPG"}, options).error.empty());
    dir.write(std::string(200, 'a') + "!", "x");
    options.find = "^(a+)+$";
    options.replace = "x";
    CHECK(plan(dir, {std::string(200, 'a') + "!"}, options).error.empty());
}

TEST_CASE("batch rename: UTC date and filesystem metadata tokens use the preview snapshot") {
    TempDir dir;
    dir.write("a.txt", "abc");
    auto entries = ops::rename_directory(dir.path).entries;
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].size == 3);
    REQUIRE(entries[0].modified.has_value());
    entries[0].modified = 0;
    const std::vector<std::string> names{"a.txt"};
    ops::RenameOptions options{.pattern = "{date}_{time}_{mtime}_{mtime_time}_{size}{ext}",
                               .timestamp = 86401};
    auto result = ops::plan_rename(entries, names, options);
    REQUIRE(result.error.empty());
    CHECK(result.changes.front().to == "1970-01-02_00-00-01_1970-01-01_00-00-00_3.txt");
    entries[0].modified.reset();
    CHECK_FALSE(ops::plan_rename(entries, names, options).error.empty());
    options.pattern = "{size}{ext}";
    entries[0].size.reset();
    CHECK_FALSE(ops::plan_rename(entries, names, options).error.empty());
    options.pattern = "{date}{ext}";
    options.timestamp.reset();
    CHECK_FALSE(ops::plan_rename(entries, names, options).error.empty());
}

TEST_CASE("batch rename: transforms retain collision checks and sidecar following") {
    TempDir dir;
    dir.write("PHOTO.ORF", "photo");
    dir.write("PHOTO.ORF.xmp", "sidecar");
    const ops::RenameOptions options{.target = ops::RenameOptions::Target::whole,
                                     .letter_case = ops::RenameOptions::Case::lower};
    const auto result = plan(dir, {"PHOTO.ORF", "PHOTO.ORF.xmp"}, options);
    REQUIRE(result.error.empty());
    REQUIRE(result.changes.size() == 2);
    CHECK(result.changes[0].to == "photo.orf");
    CHECK(result.changes[1].to == "photo.orf.xmp");
    CHECK(result.changes[1].sidecar);
    dir.write("photo.orf", "occupied");
    CHECK_FALSE(plan(dir, {"PHOTO.ORF"}, options).error.empty());
    CHECK(read(dir.path / "PHOTO.ORF") == "photo");
    CHECK(read(dir.path / "photo.orf") == "occupied");
}

TEST_CASE("batch rename: dialog transform choices are keyboard accessible and reset on reopen") {
    TempDir dir;
    dir.write("PHOTO.JPG", "x");
    BatchRenameDialog dialog{[] {}};
    dialog.open(ops::rename_directory(dir.path).entries, {"PHOTO.JPG"}, [](const auto&) {});
    for (int i = 0; i < 9; ++i)
        dialog.key(key(XKB_KEY_Tab));
    dialog.key(key(XKB_KEY_space));  // Target: Extension
    dialog.key(key(XKB_KEY_Tab));
    dialog.key(key(XKB_KEY_space));  // Lowercase
    REQUIRE(dialog.can_apply());
    CHECK(dialog.preview().changes.front().to == "PHOTO.jpg");
    dialog.key(key(XKB_KEY_Left));  // Case: Unchanged
    CHECK_FALSE(dialog.can_apply());
    dialog.close();
    dialog.open(ops::rename_directory(dir.path).entries, {"PHOTO.JPG"}, [](const auto&) {});
    CHECK_FALSE(dialog.can_apply());
    dialog.key(key(XKB_KEY_Tab));
    dialog.key(key(XKB_KEY_Tab));
    dialog.paste("{date}_{name}{ext}");
    REQUIRE(dialog.can_apply());
    const auto first = dialog.preview().changes.front().to;
    dialog.key(key(XKB_KEY_Tab));
    dialog.paste("2");
    CHECK(dialog.preview().changes.front().to == first);
}

TEST_CASE("batch rename: invalid names, templates, duplicate and occupied targets stay disabled") {
    TempDir dir;
    dir.write("a.txt", "a");
    dir.write("b.txt", "b");
    dir.write("taken.txt", "unrelated");
    for (const auto& pattern : {"", "../bad", "same.txt", "taken.txt", "{unknown}", "{", ".."}) {
        CAPTURE(pattern);
        CHECK_FALSE(plan(dir, {"a.txt", "b.txt"}, {.pattern = pattern}).error.empty());
    }
    CHECK_FALSE(plan(dir, {"a.txt"}, {.pattern = std::string(256, 'a')}).error.empty());
    CHECK_FALSE(plan(dir, {"a.txt", "b.txt"},
                     {.pattern = "{n}", .start = std::numeric_limits<std::uint64_t>::max()})
                    .error.empty());
    CHECK_FALSE(plan(dir, {"a.txt"}, {.step = 0}).error.empty());
    CHECK(read(dir.path / "taken.txt") == "unrelated");
}

TEST_CASE("batch rename: cycles, chains and dangling symlinks use no replacement") {
    TempDir dir;
    dir.write("a", "A");
    dir.write("b", "B");
    dir.write("c", "C");
    auto p = plan(dir, {"a", "b", "c"}, {});
    REQUIRE(p.error.empty());
    p.changes[0].to = "b";
    p.changes[1].to = "c";
    p.changes[2].to = "a";
    auto result = ops::rename_batch(dir.path, p.changes);
    REQUIRE(result.error.empty());
    CHECK(read(dir.path / "a") == "C");
    CHECK(read(dir.path / "b") == "A");
    CHECK(read(dir.path / "c") == "B");
    for (auto& change : result.changed) {
        std::swap(change.from, change.to);
    }
    REQUIRE(ops::rename_batch(dir.path, result.changed).error.empty());
    CHECK(read(dir.path / "a") == "A");
    CHECK(read(dir.path / "b") == "B");
    CHECK(read(dir.path / "c") == "C");
    fs::create_symlink("missing", dir.path / "occupied");
    p.changes[0].to = "occupied";
    CHECK_FALSE(ops::rename_batch(dir.path, p.changes).error.empty());
    CHECK(fs::is_symlink(dir.path / "occupied"));
    p.changes[0].to = "b";
    p.changes[2].to = "d";
    REQUIRE(ops::rename_batch(dir.path, p.changes).error.empty());
    CHECK(read(dir.path / "b") == "A");
    CHECK(read(dir.path / "c") == "B");
    CHECK(read(dir.path / "d") == "C");
}

TEST_CASE("batch rename: sidecars and duplicates follow photos without consuming numbers") {
    TempDir dir;
    for (const auto* name : {"a.ORF", "a.ORF.xmp", "a_01.ORF.xmp", "b.ORF", "b.ORF.xmp"}) {
        dir.write(name, name);
    }
    const auto p = plan(dir, {"a.ORF", "a.ORF.xmp", "a_01.ORF.xmp", "b.ORF", "b.ORF.xmp"},
                        {.pattern = "photo_{n}{ext}"});
    REQUIRE(p.error.empty());
    REQUIRE(p.changes.size() == 5);
    CHECK(p.changes[0].to == "photo_01.ORF");
    CHECK(p.changes[1].to == "photo_01.ORF.xmp");
    CHECK(p.changes[2].to == "photo_01_01.ORF.xmp");
    CHECK(p.changes[3].to == "photo_02.ORF");
    REQUIRE(ops::rename_batch(dir.path, p.changes).error.empty());
    CHECK(read(dir.path / "photo_01_01.ORF.xmp") == "a_01.ORF.xmp");
    CHECK(read(dir.path / "photo_02.ORF.xmp") == "b.ORF.xmp");
}

TEST_CASE("batch rename: indexed sidecars match single-rename ownership and hidden conflicts") {
    const std::vector<std::string> names{
        "P1.ORF",         "P1.ORF.xmp", "P1_01.ORF.xmp", "P1_12.ORF.xmp", "P1_1.ORF.xmp",
        "P1_ab.ORF.xmp",  "P1.JPG.xmp", "P10.ORF.xmp",   "P1_2024.ORF",   "P1_2024.ORF.xmp",
        "P1.ORF.xmp.bak", ".ORF",       "_01.ORF.xmp",   "noext",         "noext.xmp"};
    std::vector<ops::RenameSource> directory;
    for (const auto& name : names) {
        directory.push_back({.name = name, .regular = true});
    }
    for (const auto* photo : {"P1.ORF", "P1_2024.ORF", ".ORF", "noext", "P1.ORF.xmp"}) {
        CAPTURE(photo);
        const std::vector<std::string> selection{photo};
        const auto p = ops::plan_rename(directory, selection, {.pattern = "{name}-renamed{ext}"});
        REQUIRE(p.error.empty());
        std::vector<std::string> actual;
        for (const auto& change : p.changes) {
            if (change.sidecar)
                actual.push_back(change.from);
        }
        CHECK(actual == ops::sidecars_of(names, photo));
    }
    directory.push_back({.name = ".hidden.ORF.xmp", .regular = true});
    const std::vector<std::string> selection{"P1.ORF"};
    const auto conflict = ops::plan_rename(directory, selection, {.pattern = ".hidden{ext}"});
    CHECK_FALSE(conflict.error.empty());
}

TEST_CASE("batch rename: changed sources and late conflicts refuse the whole operation") {
    TempDir dir;
    dir.write("a.txt", "a");
    dir.write("b.txt", "b");
    auto p = plan(dir, {"a.txt", "b.txt"}, {.pattern = "new_{n}{ext}"});
    REQUIRE(p.error.empty());
    fs::rename(dir.path / "b.txt", dir.path / "old.txt");
    dir.write("b.txt", "replacement");
    CHECK_FALSE(ops::rename_batch(dir.path, p.changes).error.empty());
    CHECK(read(dir.path / "a.txt") == "a");
    CHECK(read(dir.path / "b.txt") == "replacement");
    p = plan(dir, {"a.txt", "b.txt"}, {.pattern = "new_{n}{ext}"});
    dir.write("new_02.txt", "racing target");
    CHECK_FALSE(ops::rename_batch(dir.path, p.changes).error.empty());
    CHECK(read(dir.path / "new_02.txt") == "racing target");
    CHECK(fs::exists(dir.path / "a.txt"));
    CHECK_FALSE(fs::exists(dir.path / "new_01.txt"));
}

TEST_CASE(
    "batch rename: a target created during staging rolls back, retaining blocked recovery files") {
    for (const bool block_rollback : {false, true}) {
        CAPTURE(block_rollback);
        TempDir dir;
        std::vector<std::string> names;
        for (int i = 0; i < 2000; ++i) {
            const auto name = "source_" + std::to_string(i);
            dir.write(name, name);
            names.push_back(name);
        }
        const auto p = plan(dir, names, {.pattern = "target_{n}", .padding = 0});
        REQUIRE(p.error.empty());
        const int notify = inotify_init1(IN_CLOEXEC);
        REQUIRE(notify >= 0);
        REQUIRE(inotify_add_watch(notify, dir.path.c_str(), IN_MOVED_FROM) >= 0);
        std::atomic<bool> injected{false};
        // Observe an actual first-stage move before creating a conflicting final target.
        // Thousands of subsequent no-replace syscalls keep this independent of tiny timing gaps.
        std::thread contender{[&] {
            pollfd pending{.fd = notify, .events = POLLIN, .revents = 0};
            if (poll(&pending, 1, 5000) > 0) {
                alignas(inotify_event) std::array<char, 4096> buffer{};
                if (::read(notify, buffer.data(), buffer.size()) > 0) {
                    std::ofstream{dir.path / "target_2000"} << "racing destination";
                    if (block_rollback) {
                        std::ofstream{dir.path / "source_0"} << "racing original";
                    }
                    injected.store(true);
                }
            }
        }};
        const auto result = ops::rename_batch(dir.path, p.changes);
        contender.join();
        close(notify);
        REQUIRE(injected.load());
        REQUIRE_FALSE(result.error.empty());
        CHECK(result.changed.empty());
        CHECK(read(dir.path / "target_2000") == "racing destination");
        CHECK(read(dir.path / "source_1999") == "source_1999");
        CHECK_FALSE(fs::exists(dir.path / "target_1"));
        if (!block_rollback) {
            CHECK(result.recovery.empty());
            CHECK(read(dir.path / "source_0") == "source_0");
        } else {
            REQUIRE(result.recovery.size() == 1);
            CHECK(read(dir.path / "source_0") == "racing original");
            CHECK(result.recovery[0].starts_with("source_0 → "));
            const auto remaining = result.recovery[0].substr(std::string{"source_0 → "}.size());
            CHECK(read(remaining) == "source_0");
        }
    }
}

TEST_CASE("batch rename: dialog live preview, keyboard fields, conflicts and cancellation") {
    TempDir dir;
    dir.write("a.txt", "a");
    dir.write("b.txt", "b");
    bool applied = false;
    BatchRenameDialog dialog{[] {}};
    dialog.open(ops::rename_directory(dir.path).entries, {"b.txt", "a.txt"},
                [&](const auto&) { applied = true; });
    CHECK_FALSE(dialog.can_apply());
    CHECK(dialog.key(key(XKB_KEY_Tab)));
    CHECK(dialog.key(key(XKB_KEY_Tab)));  // template selected
    dialog.paste("{n}{ext}");
    REQUIRE(dialog.can_apply());
    CHECK(dialog.preview().changes[0].from == "b.txt");
    CHECK(dialog.preview().changes[0].to == "01.txt");
    CHECK(dialog.key(key(XKB_KEY_a, {}, {.ctrl = true})));
    dialog.paste("same.txt");
    CHECK_FALSE(dialog.can_apply());
    CHECK(dialog.key(key(XKB_KEY_Return)));
    CHECK_FALSE(applied);
    REQUIRE(dialog.is_open());
    CHECK(dialog.key(key(XKB_KEY_Escape)));
    CHECK_FALSE(dialog.is_open());
    CHECK_FALSE(applied);
}

TEST_CASE("batch rename: F2 multi-selection, sidecars, whole-batch undo and redo") {
    Fixture f;
    for (const auto* name : {"a.ORF", "a.ORF.xmp", "b.ORF"}) {
        f.dir.write(name, name);
    }
    f.browser.open(f.dir.path);
    f.browser.set_mode(Browser::Mode::details);
    REQUIRE(f.settle());
    f.browser.details().folder()->select_all();
    f.press(XKB_KEY_F2);
    REQUIRE(f.settle());
    REQUIRE(f.browser.batch_rename_dialog().is_open());
    f.press(XKB_KEY_Tab);
    f.press(XKB_KEY_Tab);
    f.press(XKB_KEY_NoSymbol, "photo_{n}{ext}");
    REQUIRE(f.browser.batch_rename_dialog().can_apply());
    f.press(XKB_KEY_Return);
    REQUIRE(f.settle());
    CHECK(read(f.dir.path / "photo_01.ORF") == "a.ORF");
    CHECK(read(f.dir.path / "photo_01.ORF.xmp") == "a.ORF.xmp");
    CHECK(read(f.dir.path / "photo_02.ORF") == "b.ORF");
    f.dir.write("photo_01_03.ORF.xmp", "created after rename");
    f.press(XKB_KEY_z, {}, {.ctrl = true});
    REQUIRE(f.settle());
    CHECK(read(f.dir.path / "photo_01_03.ORF.xmp") == "created after rename");
    CHECK_FALSE(fs::exists(f.dir.path / "a_03.ORF.xmp"));
    CHECK(read(f.dir.path / "a.ORF.xmp") == "a.ORF.xmp");
    CHECK(read(f.dir.path / "b.ORF") == "b.ORF");
    CHECK_FALSE(fs::exists(f.dir.path / "photo_01.ORF"));
    f.press(XKB_KEY_z, {}, {.ctrl = true, .shift = true});
    REQUIRE(f.settle());
    CHECK(read(f.dir.path / "photo_01.ORF.xmp") == "a.ORF.xmp");
    CHECK(read(f.dir.path / "photo_02.ORF") == "b.ORF");
}

TEST_CASE("batch rename: undo refuses a replaced member without renaming its peers") {
    Fixture f;
    f.dir.write("a.txt", "a");
    f.dir.write("b.txt", "b");
    f.browser.open(f.dir.path);
    f.browser.set_mode(Browser::Mode::details);
    REQUIRE(f.settle());
    f.browser.details().folder()->select_all();
    f.press(XKB_KEY_F2);
    REQUIRE(f.settle());
    f.press(XKB_KEY_Tab);
    f.press(XKB_KEY_Tab);
    f.press(XKB_KEY_NoSymbol, "renamed_{n}{ext}");
    f.press(XKB_KEY_Return);
    REQUIRE(f.settle());
    fs::rename(f.dir.path / "renamed_02.txt", f.dir.path / "original-b.txt");
    f.dir.write("renamed_02.txt", "replacement");
    f.press(XKB_KEY_z, {}, {.ctrl = true});
    REQUIRE(f.settle());
    CHECK(read(f.dir.path / "renamed_01.txt") == "a");
    CHECK(read(f.dir.path / "renamed_02.txt") == "replacement");
    CHECK_FALSE(fs::exists(f.dir.path / "a.txt"));
    CHECK_FALSE(fs::exists(f.dir.path / "b.txt"));
}

TEST_CASE("batch rename: menu and palette expose the same multi-selection operation") {
    Fixture f;
    f.dir.write("a.txt", "a");
    f.dir.write("b.txt", "b");
    f.browser.open(f.dir.path);
    f.browser.set_mode(Browser::Mode::details);
    REQUIRE(f.settle());
    f.browser.details().folder()->select_all();
    f.browser.open_menu(100, 100, true);
    const auto& entries = f.browser.context_menu().items();
    const auto menu = std::ranges::find(entries, "rename", &rummage::views::MenuItem::id);
    REQUIRE(menu != entries.end());
    CHECK(menu->label == "Batch Rename…");
    REQUIRE(menu->enabled);
    const auto run = menu->action;
    f.browser.context_menu().close();
    run();
    REQUIRE(f.settle());
    REQUIRE(f.browser.batch_rename_dialog().is_open());
    f.press(XKB_KEY_Escape);
    auto commands = f.browser.palette_items();
    const auto command =
        std::ranges::find(commands, "command:rename", &rummage::views::PaletteItem::id);
    REQUIRE(command != commands.end());
    CHECK(command->label == "Batch Rename…");
    command->action();
    REQUIRE(f.settle());
    CHECK(f.browser.batch_rename_dialog().is_open());
}

TEST_CASE("batch rename: delayed clipboard paste never enters a later inline editor") {
    Fixture f;
    f.dir.write("a.txt", "a");
    f.dir.write("b.txt", "b");
    f.browser.open(f.dir.path);
    f.browser.set_mode(Browser::Mode::details);
    REQUIRE(f.settle());
    f.browser.details().folder()->select_all();
    rummage::views::SystemClipboard::Done pending;
    rummage::views::SystemClipboard clipboard;
    clipboard.types = [] { return std::vector<std::string>{"text/plain"}; };
    clipboard.read = [&](std::string, auto done) { pending = std::move(done); };
    f.browser.set_system_clipboard(std::move(clipboard));
    f.press(XKB_KEY_F2);
    REQUIRE(f.settle());
    f.press(XKB_KEY_Tab);
    f.press(XKB_KEY_Tab);
    f.press(XKB_KEY_v, {}, {.ctrl = true});
    REQUIRE(pending);
    f.press(XKB_KEY_Escape);
    f.browser.details().folder()->select_name("a.txt");
    f.press(XKB_KEY_F2);
    REQUIRE(f.browser.name_editor().active());
    pending("wrong-dialog_{n}{ext}");
    CHECK(f.browser.name_editor().field().text() == "a.txt");
}

TEST_CASE("batch rename: repeated F2 and cancelled preparation cannot reopen a dialog") {
    Fixture f;
    f.dir.write("a.txt", "a");
    f.dir.write("b.txt", "b");
    f.browser.open(f.dir.path);
    f.browser.set_mode(Browser::Mode::details);
    REQUIRE(f.settle());
    f.browser.details().folder()->select_all();
    WorkerGate gate;
    gate.hold(f.pool);
    f.press(XKB_KEY_F2);
    REQUIRE(f.browser.batch_rename_dialog().busy());
    f.press(XKB_KEY_F2);
    f.browser.start_rename();
    f.press(XKB_KEY_Escape);
    CHECK_FALSE(f.browser.batch_rename_dialog().is_open());
    gate.release();
    REQUIRE(f.settle());
    CHECK_FALSE(f.browser.batch_rename_dialog().is_open());
    f.press(XKB_KEY_F2);
    REQUIRE(f.settle());
    REQUIRE(f.browser.batch_rename_dialog().is_open());
    CHECK_FALSE(f.browser.batch_rename_dialog().busy());
    f.press(XKB_KEY_Escape);
    REQUIRE(f.settle());
    CHECK_FALSE(f.browser.batch_rename_dialog().is_open());
}

TEST_CASE("batch rename: mutation and undo are serialized while a batch is running") {
    Fixture f;
    f.dir.write("a.txt", "a");
    f.dir.write("b.txt", "b");
    f.browser.open(f.dir.path);
    f.browser.set_mode(Browser::Mode::details);
    REQUIRE(f.settle());
    f.browser.new_file("previous.txt", {});
    REQUIRE(f.settle());
    f.press(XKB_KEY_Escape);
    REQUIRE(f.browser.last_operation());
    CHECK(f.browser.last_operation().value_or(rummage::views::FileOperation{}).kind ==
          rummage::views::FileOperation::Kind::create);
    f.browser.details().folder()->select_all();
    f.press(XKB_KEY_F2);
    REQUIRE(f.settle());
    f.press(XKB_KEY_Tab);
    f.press(XKB_KEY_Tab);
    f.press(XKB_KEY_NoSymbol, "renamed_{n}{ext}");
    WorkerGate gate;
    gate.hold(f.pool);
    f.press(XKB_KEY_Return);
    REQUIRE(f.browser.batch_rename_dialog().busy());
    f.browser.undo();
    f.browser.redo();
    f.browser.new_folder();
    f.browser.new_file("should-not-exist", {});
    f.press(XKB_KEY_Escape);
    CHECK(f.browser.batch_rename_dialog().busy());
    CHECK(f.browser.last_operation().value_or(rummage::views::FileOperation{}).kind ==
          rummage::views::FileOperation::Kind::create);
    gate.release();
    REQUIRE(f.settle());
    CHECK_FALSE(f.browser.batch_rename_dialog().is_open());
    REQUIRE(f.browser.last_operation());
    CHECK(f.browser.last_operation().value_or(rummage::views::FileOperation{}).kind ==
          rummage::views::FileOperation::Kind::batch_rename);
    CHECK_FALSE(fs::exists(f.dir.path / "New Folder"));
    CHECK_FALSE(fs::exists(f.dir.path / "should-not-exist"));
    CHECK(read(f.dir.path / "renamed_01.txt") == "a");
    CHECK(read(f.dir.path / "renamed_02.txt") == "b");
    CHECK(fs::exists(f.dir.path / "renamed_03.txt"));
    f.browser.undo();
    REQUIRE(f.settle());
    CHECK(fs::exists(f.dir.path / "previous.txt"));
    CHECK(read(f.dir.path / "a.txt") == "a");
}

TEST_CASE("batch rename: picker refuses multi-selection mutation") {
    Fixture f;
    f.dir.write("a", "a");
    f.dir.write("b", "b");
    f.browser.open(f.dir.path);
    f.browser.set_mode(Browser::Mode::details);
    REQUIRE(f.settle());
    f.browser.details().folder()->select_all();
    f.browser.set_picker_mode(true);
    f.browser.start_rename();
    REQUIRE(f.settle());
    CHECK_FALSE(f.browser.batch_rename_dialog().is_open());
    CHECK_FALSE(f.browser.name_editor().active());
}

TEST_CASE("batch rename: preview renders with scrolling and disabled conflicts") {
    rummage::ui::Offscreen gl{900, 650};
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    BatchRenameDialog dialog{[] {}};
    std::vector<ops::RenameSource> entries;
    std::vector<std::string> names;
    for (int i = 0; i < 30; ++i) {
        auto name = "Screenshot_20261116_" + std::to_string(i) + ".png";
        names.push_back(name);
        entries.push_back({.name = name, .regular = true});
    }
    dialog.open(entries, names, [](const auto&) {});
    CHECK(dialog.key(key(XKB_KEY_Tab)));
    CHECK(dialog.key(key(XKB_KEY_Tab)));
    dialog.paste("bug-report_{n}{ext}");
    for (const auto [width, height] :
         {std::pair{900, 650}, std::pair{900, 400}, std::pair{480, 400}}) {
        renderer.begin_frame(width, height, 1.0F, {});
        dialog.draw(renderer, {0, 0, static_cast<float>(width), static_cast<float>(height)},
                    rummage::theme::Palette{});
        renderer.end_frame();
        CHECK(glGetError() == GL_NO_ERROR);
        CHECK(dialog.key(key(XKB_KEY_Page_Down)));
        renderer.begin_frame(width, height, 1.0F, {});
        dialog.draw(renderer, {0, 0, static_cast<float>(width), static_cast<float>(height)},
                    rummage::theme::Palette{});
        renderer.end_frame();
        CHECK(glGetError() == GL_NO_ERROR);
        if (const char* const out = std::getenv("RUMMAGE_BATCH_RENAME_FRAMES")) {
            std::vector<unsigned char> pixels(static_cast<std::size_t>(width * height * 4));
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            const auto filename =
                width == 900 ? std::to_string(height) : std::format("{}x{}", width, height);
            std::ofstream image{fs::path{out} / (filename + ".ppm"), std::ios::binary};
            image << "P6\n" << width << ' ' << height << "\n255\n";
            for (int y = height - 1; y >= 0; --y) {
                for (int x = 0; x < width; ++x) {
                    image.write(reinterpret_cast<const char*>(
                                    pixels.data() + static_cast<std::size_t>((y * width + x) * 4)),
                                3);
                }
            }
        }
    }
}
