#include "rummage/views/file_finder.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/fs/walk.hpp"
#include "rummage/ui/seat.hpp"
#include "rummage/views/browser.hpp"
#include "rummage/views/command_palette.hpp"
#include "rummage/views/fuzzy.hpp"
#include "temp_dir.hpp"

using rummage::core::CancelToken;
using rummage::core::EventLoop;
using rummage::core::WorkerPool;
using rummage::fs::PathBatch;
using rummage::fs::walk;
using rummage::fs::WalkOptions;
using rummage::fs::WalkResult;
using rummage::test::TempDir;
using rummage::ui::CursorShape;
using rummage::ui::KeyEvent;
using rummage::ui::Modifiers;
using rummage::views::better_rank;
using rummage::views::Browser;
using rummage::views::FileFinder;
using rummage::views::FoundFile;
using rummage::views::FuzzyQuery;
using rummage::views::match_paths;
using rummage::views::PathRank;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

void touch(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream{path} << "x";
}

// a.txt, photos/{shot.orf, shot.orf.xmp, deep/deeper/far.orf}, notes/orf-list.txt, .hidden/
// secret.orf, .dotfile, link -> photos.
void make_tree(const fs::path& root) {
    touch(root / "a.txt");
    touch(root / "photos" / "shot.orf");
    touch(root / "photos" / "shot.orf.xmp");
    touch(root / "photos" / "deep" / "deeper" / "far.orf");
    touch(root / "notes" / "orf-list.txt");
    touch(root / ".hidden" / "secret.orf");
    touch(root / ".dotfile");
    fs::create_directory_symlink(root / "photos", root / "link");
}

struct Walked {
    std::vector<std::string> paths;
    std::vector<bool> directories;
    WalkResult result;
};

Walked walk_all(const fs::path& root, WalkOptions options, const CancelToken& token = {}) {
    Walked out;
    out.result = walk(root, options, token, [&out](const PathBatch& batch) {
        for (std::size_t i = 0; i < batch.size(); ++i) {
            out.paths.emplace_back(batch.path(i));
            out.directories.push_back(batch.directory(i));
        }
    });
    return out;
}

std::size_t index_of(const std::vector<std::string>& paths, std::string_view path) {
    return static_cast<std::size_t>(std::ranges::find(paths, path) - paths.begin());
}

bool run_until(EventLoop& loop, const std::function<bool()>& done) {
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

KeyEvent key(xkb_keysym_t keysym, Modifiers modifiers = {}, std::string text = {}) {
    KeyEvent event;
    event.keysym = keysym;
    event.modifiers = modifiers;
    event.text = std::move(text);
    return event;
}

constexpr Modifiers kCtrl{.ctrl = true};

}  // namespace

TEST_CASE("walk: breadth first, relative paths, no dotfiles, links listed not followed") {
    const TempDir dir;
    make_tree(dir.path);
    const Walked walked = walk_all(dir.path, {.batch = 2});
    CHECK(walked.result.entries == walked.paths.size());
    CHECK_FALSE(walked.result.truncated);
    std::vector<std::string> sorted = walked.paths;
    std::ranges::sort(sorted);
    CHECK(sorted == std::vector<std::string>{"a.txt", "link", "notes", "notes/orf-list.txt",
                                             "photos", "photos/deep", "photos/deep/deeper",
                                             "photos/deep/deeper/far.orf", "photos/shot.orf",
                                             "photos/shot.orf.xmp"});
    // The nearest first: every entry of a folder before those of its subfolders.
    CHECK(index_of(walked.paths, "a.txt") < index_of(walked.paths, "photos/shot.orf"));
    CHECK(index_of(walked.paths, "photos/shot.orf") < index_of(walked.paths, "photos/deep/deeper"));
    CHECK(walked.directories[index_of(walked.paths, "photos")]);
    CHECK_FALSE(walked.directories[index_of(walked.paths, "link")]);  // a link, not entered
    CHECK_FALSE(walked.directories[index_of(walked.paths, "a.txt")]);

    const Walked hidden = walk_all(dir.path, {.hidden = true});
    CHECK(std::ranges::find(hidden.paths, ".hidden/secret.orf") != hidden.paths.end());
    CHECK(std::ranges::find(hidden.paths, ".dotfile") != hidden.paths.end());
}

TEST_CASE("walk: stops at the limits, when cancelled, and on a missing folder") {
    const TempDir dir;
    make_tree(dir.path);
    const Walked few = walk_all(dir.path, {.max_entries = 3});
    CHECK(few.paths.size() == 3);
    CHECK(few.result.truncated);
    const Walked shallow = walk_all(dir.path, {.max_depth = 1});
    CHECK(std::ranges::find(shallow.paths, "photos/shot.orf") == shallow.paths.end());
    CHECK(std::ranges::find(shallow.paths, "photos") != shallow.paths.end());
    CancelToken token;
    token.cancel();
    const Walked cancelled = walk_all(dir.path, {}, token);
    CHECK(cancelled.result.cancelled);
    CHECK(cancelled.paths.empty());
    const Walked missing = walk_all(dir.path / "nothing", {});
    CHECK(missing.paths.empty());
    CHECK(missing.result.unreadable == 1);
}

TEST_CASE("fuzzy paths: a match in the name ranks above one across folders") {
    FuzzyQuery query{"orf"};
    const auto rank = [&query](std::string_view path) {
        const std::optional<PathRank> found = query.path_rank(path);
        REQUIRE(found);
        return found.value_or(PathRank{});
    };
    CHECK(better_rank(rank("photos/shot.orf"), rank("orf-notes/list.txt")));
    CHECK(better_rank(rank("x/y/z/far.orf"), rank("orfanage/a.txt")));
    CHECK(rank("orfanage/a.txt").tier == PathRank::Tier::folders);
    CHECK_FALSE(query.path_rank("photos/a.txt"));
    // Highlighted in the name when it matches there.
    CHECK(query.path_positions("orf/shot.orf") == std::vector<std::size_t>{9, 10, 11});
    CHECK(query.path_positions("orf/notes.txt") == std::vector<std::size_t>{0, 1, 2});
}

TEST_CASE("fuzzy paths: the name as typed, then shallower, then scattered (issue #25)") {
    const auto tier = [](std::string_view query, std::string_view path) {
        const std::optional<PathRank> found = FuzzyQuery{query}.path_rank(path);
        REQUIRE(found);
        return found.value_or(PathRank{}).tier;
    };
    CHECK(tier("swap", "usr/share/doc/SWAP") == PathRank::Tier::equal);  // any case
    CHECK(tier("swap", "swapfile") == PathRank::Tier::prefix);
    CHECK(tier("apfil", "swapfile") == PathRank::Tier::contains);
    CHECK(tier("apfil", "usr/include/app-filter.h") == PathRank::Tier::scattered);
    CHECK(tier("usr swap", "usr/bin/swapon") ==
          PathRank::Tier::folders);  // the weakest term decides

    // stradus64's searches from /: /swapfile is the shallowest of the names starting with
    // "swap", and a contiguous "apfil" beats letters at word starts, however many of those.
    auto batch = std::make_shared<PathBatch>();
    for (const char* path : {"usr/include/linux/swap.h", "usr/bin/swapon", "swapfile",
                             "usr/share/man/man8/swapon.8.gz", "usr/share/doc/SWAP"}) {
        batch->add(path, false);
    }
    for (int i = 0; i < 300; ++i) {
        batch->add(std::format("usr/x86_64-w64-mingw32/lib/api-ms-win-core-file-l{}-1-0.def", i),
                   false);
    }
    batch->add("usr/include/app-filter.h", false);
    const std::vector<std::shared_ptr<const PathBatch>> batches{batch};
    const std::vector<FoundFile> swap = match_paths(batches, "swap", 200, {});
    REQUIRE(swap.size() >= 4);
    CHECK(swap[0].path == "usr/share/doc/SWAP");  // the name is the query
    CHECK(swap[1].path == "swapfile");            // then the shallowest that starts with it
    CHECK(swap[2].path == "usr/bin/swapon");
    const std::vector<FoundFile> apfil = match_paths(batches, "apfil", 200, {});
    REQUIRE_FALSE(apfil.empty());
    CHECK(apfil[0].path == "swapfile");
}

TEST_CASE("fuzzy: terms split at spaces, all needed, in any order (issue #25)") {
    CHECK(FuzzyQuery{"monte rosa"}.score("Monte Rosa"));
    CHECK(FuzzyQuery{"monte rosa"}.score("MonteRosa"));
    CHECK(FuzzyQuery{"rosa monte"}.score("Monte Rosa"));          // any order
    CHECK(FuzzyQuery{"  ga   st "}.score("star Games"));          // spaces anywhere, any number
    CHECK_FALSE(FuzzyQuery{"monte bianco"}.score("Monte Rosa"));  // every term is needed
    CHECK(FuzzyQuery{"   "}.empty());
    // The positions: every term's, each once, ascending.
    CHECK(FuzzyQuery{"ga st"}.positions("star Games") == std::vector<std::size_t>{0, 1, 5, 6});
    CHECK(FuzzyQuery{"st sta"}.positions("star") == std::vector<std::size_t>{0, 1, 2});
    // In a path: each term in the name when it can be.
    CHECK(FuzzyQuery{"ga st"}.path_positions("Games/star.png") ==
          std::vector<std::size_t>{0, 1, 6, 7});
    const std::optional<PathRank> games = FuzzyQuery{"ga st"}.path_rank("Games/star.png");
    REQUIRE(games);
    CHECK(games.value_or(PathRank{}).tier == PathRank::Tier::folders);  // "ga" needs the folder
}

TEST_CASE("match_paths: the best few, ordered, the shorter first among equals") {
    auto batch = std::make_shared<PathBatch>();
    for (const char* path : {"orf-notes/list.txt", "photos/deep/deeper/far.orf", "photos/shot.orf",
                             "a.txt", "shot.orf"}) {
        batch->add(path, false);
    }
    const std::vector<std::shared_ptr<const PathBatch>> batches{batch};
    const std::vector<FoundFile> found = match_paths(batches, "orf", 10, {});
    REQUIRE(found.size() == 4);
    CHECK(found.back().path == "orf-notes/list.txt");  // only across the folders
    const std::vector<FoundFile> two = match_paths(batches, "orf", 2, {});
    REQUIRE(two.size() == 2);
    CHECK(two[0].path == found[0].path);
    CHECK(two[1].path == found[1].path);
    CancelToken token;
    token.cancel();
    CHECK(match_paths(batches, "orf", 10, token).empty());
}

TEST_CASE("file finder: lists once, a query typed at once is answered, the nearest when empty") {
    const TempDir dir;
    make_tree(dir.path);
    EventLoop loop;
    WorkerPool pool{loop, 2};
    int changes = 0;
    FileFinder finder{pool, [&changes] { ++changes; }, {.batch = 2}};
    finder.start(dir.path, false);
    finder.set_query("far");  // before anything is listed
    REQUIRE(run_until(loop, [&] { return !finder.scanning() && !finder.matching(); }));
    CHECK(changes > 0);
    REQUIRE_FALSE(finder.results().empty());
    CHECK(finder.results().front().path == "photos/deep/deeper/far.orf");
    CHECK(finder.entries() == 10);
    CHECK(finder.status() == "10 items");

    finder.set_query("shot");
    REQUIRE(run_until(loop, [&] { return !finder.matching(); }));
    REQUIRE(finder.results().size() == 2);
    CHECK(finder.results().front().path == "photos/shot.orf");  // the shorter first

    finder.set_query(" ");  // nothing to match: the nearest
    CHECK(finder.results().size() == 10);
    CHECK(finder.results().front().path.find('/') == std::string::npos);
    CHECK(finder.bytes() > 0);
}

namespace {

// Two folders: a/{one.txt, photos/{shot.orf, deep/far.orf}}, in the grid at a.
struct Fixture {
    TempDir dir;
    fs::path a = dir.path / "a";
    EventLoop loop;
    WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop,    pool,
                    watcher, rummage::fs::MimeDatabase::load(),
                    [] {},   [](CursorShape /*shape*/) {}};

    Fixture() {
        touch(a / "one.txt");
        touch(a / "photos" / "shot.orf");
        touch(a / "photos" / "deep" / "far.orf");
        browser.set_home_trash(dir.path / "Trash");
        browser.open(a);
        REQUIRE(settle());
        browser.set_mode(Browser::Mode::grid);
        REQUIRE(settle());
        REQUIRE(browser.mode() == Browser::Mode::grid);
    }

    bool settle() {
        return run_until(loop, [this] { return !browser.loading(); });
    }

    bool press(const KeyEvent& event) { return browser.key(event, Browser::Clock::now()); }

    // Ctrl+F, query typed, the list filled.
    void find(const std::string& query) {
        REQUIRE(press(key(XKB_KEY_f, kCtrl)));
        REQUIRE(browser.command_palette().external());
        REQUIRE(browser.file_finder() != nullptr);
        browser.command_palette().set_query(query);
        REQUIRE(run_until(loop, [this] {
            const FileFinder* finder = browser.file_finder();
            return finder != nullptr && !finder->scanning() && !finder->matching();
        }));
    }

    // The settle_found() that draw() would run, until the file is shown.
    bool shown(const std::string& name) {
        return run_until(loop, [this, &name] {
            browser.settle_found(Browser::Clock::now());
            const std::shared_ptr<rummage::views::Folder> folder = browser.grid().folder();
            return folder && folder->cursor_entry() != nullptr &&
                   folder->cursor_entry()->name == name;
        });
    }
};

}  // namespace

TEST_CASE("find files: Ctrl+F lists the folder, Enter shows the file in the view") {
    Fixture f;
    f.find("far");
    const auto& palette = f.browser.command_palette();
    REQUIRE_FALSE(palette.shown().empty());
    CHECK(palette.items()[palette.shown().front()].label == "photos/deep/far.orf");
    CHECK(palette.items().front().path);
    CHECK(palette.status() == "5 items");
    f.press(key(XKB_KEY_Return));
    CHECK_FALSE(palette.is_open());
    CHECK(f.browser.file_finder() == nullptr);  // let go once closed
    REQUIRE(f.shown("far.orf"));
    CHECK(f.browser.grid().folder()->path() == f.a / "photos" / "deep");
    CHECK(f.browser.mode() == Browser::Mode::grid);
}

TEST_CASE("find files: Space is a space in the field, Quick Look in the list (issue #25)") {
    Fixture f;
    f.find("shot");
    const auto& palette = f.browser.command_palette();
    // Typing: a space in the query.
    f.press(key(XKB_KEY_space, {}, " "));
    REQUIRE(palette.is_open());
    CHECK(palette.query() == "shot ");
    CHECK_FALSE(palette.in_list());
    // Down moves into the list; Shift or Ctrl alone leave it there.
    f.press(key(XKB_KEY_Down));
    CHECK(palette.in_list());
    f.press(key(XKB_KEY_Shift_L, {.shift = true}));
    CHECK(palette.in_list());
    // Shift+Space is always a space, and back to the field.
    f.press(key(XKB_KEY_space, {.shift = true}, " "));
    CHECK(palette.query() == "shot  ");
    CHECK_FALSE(palette.in_list());
    // So is typing.
    f.press(key(XKB_KEY_Up));
    CHECK(palette.in_list());
    f.press(key(XKB_KEY_BackSpace));
    CHECK(palette.query() == "shot ");
    CHECK_FALSE(palette.in_list());
    // In the list, Space shows the highlighted file in Quick Look (once the query's results are
    // in).
    REQUIRE(run_until(f.loop, [&f] {
        const FileFinder* finder = f.browser.file_finder();
        return finder != nullptr && !finder->matching() &&
               !f.browser.command_palette().shown().empty();
    }));
    f.press(key(XKB_KEY_Down));
    f.press(key(XKB_KEY_Up));
    f.press(key(XKB_KEY_space, {}, " "));
    CHECK_FALSE(palette.is_open());
    REQUIRE(f.shown("shot.orf"));
    REQUIRE(run_until(f.loop, [&f] {
        f.browser.settle_found(Browser::Clock::now());
        return f.browser.quick_look().is_open();
    }));
    f.browser.quick_look().close();

    f.find("deep");
    REQUIRE_FALSE(palette.shown().empty());
    CHECK(palette.items()[palette.shown().front()].keys == "Folder");
}

TEST_CASE("find files: Ctrl+F again closes, Ctrl+K switches, the palette offers it") {
    Fixture f;
    REQUIRE(f.press(key(XKB_KEY_f, kCtrl)));
    REQUIRE(f.browser.command_palette().external());
    f.press(key(XKB_KEY_k, kCtrl));
    CHECK(f.browser.command_palette().is_open());
    CHECK_FALSE(f.browser.command_palette().external());
    CHECK(f.browser.file_finder() == nullptr);
    f.press(key(XKB_KEY_f, kCtrl));
    CHECK(f.browser.command_palette().external());
    f.press(key(XKB_KEY_f, kCtrl));
    CHECK_FALSE(f.browser.command_palette().is_open());
    // From the command palette, "Find Files…".
    f.browser.open_palette();
    f.browser.command_palette().set_query("find files");
    f.press(key(XKB_KEY_Return));
    CHECK(f.browser.command_palette().external());
    f.press(key(XKB_KEY_Escape));
    CHECK_FALSE(f.browser.command_palette().is_open());
}

TEST_CASE("find files: in the columns, the way down from the first column stays") {
    Fixture f;
    f.browser.set_mode(Browser::Mode::columns);
    REQUIRE(f.settle());
    f.find("far");
    f.press(key(XKB_KEY_Return));
    REQUIRE(run_until(f.loop, [&f] {
        const std::shared_ptr<rummage::views::Folder> folder = f.browser.columns().focused_folder();
        return folder && folder->path() == f.a / "photos" / "deep" &&
               folder->cursor_entry() != nullptr && folder->cursor_entry()->name == "far.orf";
    }));
    CHECK(f.browser.columns().column(0).folder().path() == f.a);
    CHECK(f.browser.columns().column_count() == 3);  // a, photos, deep
}
