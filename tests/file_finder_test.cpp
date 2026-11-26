#include "rummage/views/file_finder.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/annotations.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/fs/recent.hpp"
#include "rummage/fs/walk.hpp"
#include "rummage/ui/icons.hpp"
#include "rummage/ui/offscreen.hpp"
#include "rummage/ui/seat.hpp"
#include "rummage/views/browser.hpp"
#include "rummage/views/command_palette.hpp"
#include "rummage/views/fuzzy.hpp"
#include "temp_dir.hpp"
#include "xattr_support.hpp"

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

    void select_result(std::size_t row,
                       rummage::views::SelectMode mode = rummage::views::SelectMode::replace) {
        const auto folder = browser.picker_folder();
        REQUIRE(folder);
        REQUIRE(folder->is_collection());
        folder->set_cursor(row, mode);
    }

    std::vector<fs::path> chosen_results() const {
        const auto folder = browser.picker_folder();
        std::vector<fs::path> paths;
        if (folder)
            for (const auto& name : folder->selected_names())
                paths.push_back(folder->model().source_path(name));
        std::ranges::sort(paths);
        return paths;
    }

    // The optional quick finder, query typed, the list filled.
    void find(const std::string& query) {
        browser.open_finder();
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

TEST_CASE("find files: quick finder lists the folder, Enter shows the file in the view") {
    Fixture f;
    f.find("far");
    const auto& palette = f.browser.command_palette();
    REQUIRE_FALSE(palette.shown().empty());
    CHECK(palette.items()[palette.shown().front()].label == "photos/deep/far.orf");
    CHECK(palette.items().front().path);
    CHECK(palette.status().starts_with("5 items"));
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

TEST_CASE("find files: Ctrl+F opens native recursive results and refocuses the toolbar query") {
    Fixture f;
    touch(f.a / "one.zip");
    touch(f.a / "photos" / "another.zip");
    touch(f.a / "photos" / "deep" / "third.zip");
    f.browser.set_name_filter("one.txt");
    REQUIRE(f.press(key(XKB_KEY_F, {.ctrl = true, .shift = true})));
    REQUIRE(f.browser.filtering());
    REQUIRE(f.press(key(XKB_KEY_f, kCtrl)));
    REQUIRE(f.browser.searching_results());
    CHECK(f.browser.filtering());
    CHECK_FALSE(f.browser.has_overlay());
    CHECK_FALSE(f.browser.command_palette().is_open());
    auto* results = f.browser.search_results();
    f.browser.set_name_filter("*.zip");
    REQUIRE(f.settle());
    CHECK(results->finder()->root() == f.a);
    REQUIRE(results->rows().size() == 3);
    f.browser.picker_folder()->select_all();
    CHECK(f.chosen_results().size() == 3);
    REQUIRE(f.press(key(XKB_KEY_f, kCtrl)));
    CHECK(f.browser.filtering());
    CHECK(results->finder()->query() == "*.zip");
    CHECK(results->rows().size() == 3);
    // The usual command palette remains available while the toolbar query owns focus.
    REQUIRE(f.press(key(XKB_KEY_k, kCtrl)));
    CHECK(f.browser.command_palette().is_open());
    CHECK_FALSE(f.browser.command_palette().external());
    REQUIRE(f.press(key(XKB_KEY_Escape)));
    CHECK_FALSE(f.browser.command_palette().is_open());
    CHECK(f.browser.searching_results());
    CHECK(f.browser.name_filter() == "*.zip");
    CHECK(f.browser.picker_folder()->entries().size() == 3);
    f.browser.search_action(rummage::views::SearchResults::Action::close);
    f.browser.open_palette();
    f.browser.command_palette().set_query("find files");
    f.press(key(XKB_KEY_Return));
    CHECK(f.browser.searching_results());
    f.browser.search_action(rummage::views::SearchResults::Action::close);
    f.browser.open_finder();
    REQUIRE(f.browser.command_palette().external());
    f.browser.command_palette().set_query("*.zip");
    REQUIRE(f.settle());
    // Ctrl+F promotes an optional quick search without discarding its scope/query.
    f.press(key(XKB_KEY_f, kCtrl));
    REQUIRE(f.browser.searching_results());
    REQUIRE(f.settle());
    CHECK(f.browser.search_results()->finder()->query() == "*.zip");
    CHECK(f.browser.search_results()->rows().size() == 3);
}

TEST_CASE("native search: duplicate names use normal selection copy and navigation history") {
    Fixture f;
    const auto first = f.a / "left" / "same.zip";
    const auto second = f.a / "right" / "same.zip";
    touch(first);
    touch(second);
    f.browser.set_name_filter("one.txt");
    REQUIRE(f.settle());
    REQUIRE(f.browser.picker_folder()->entries().size() == 1);
    f.browser.open_search_results();
    REQUIRE(f.settle());
    REQUIRE(f.browser.picker_folder()->is_search());
    CHECK_FALSE(f.browser.has_overlay());
    CHECK(f.browser.mode() == Browser::Mode::grid);
    CHECK(f.browser.picker_folder()->entries().empty());
    f.browser.set_name_filter("*.zip");
    REQUIRE(f.settle());
    const auto folder = f.browser.picker_folder();
    REQUIRE(folder->entries().size() == 2);
    CHECK(folder == f.browser.grid().folder());
    CHECK(folder->entries()[0].label() == "same.zip");
    CHECK(folder->entries()[1].label() == "same.zip");
    CHECK(folder->entries()[0].name != folder->entries()[1].name);
    f.browser.blur_filter();
    f.select_result(0);
    REQUIRE(f.press(key(XKB_KEY_a, kCtrl)));
    CHECK(f.chosen_results() == std::vector<fs::path>{first, second});
    std::vector<rummage::views::SystemClipboard::Format> offered;
    rummage::views::SystemClipboard clipboard;
    clipboard.offer = [&](auto formats) { offered = std::move(formats); };
    f.browser.set_system_clipboard(std::move(clipboard));
    REQUIRE(f.press(key(XKB_KEY_c, kCtrl)));
    const auto uris = std::ranges::find_if(
        offered, [](const auto& format) { return format.first == "text/uri-list"; });
    REQUIRE(uris != offered.end());
    CHECK(uris->second.find("file://" + first.string()) != std::string::npos);
    CHECK(uris->second.find("file://" + second.string()) != std::string::npos);
    CHECK(uris->second.find("rummage:search") == std::string::npos);
    f.browser.back();
    REQUIRE(f.settle());
    CHECK_FALSE(f.browser.searching_results());
    CHECK(f.browser.picker_folder()->path() == f.a);
    CHECK(f.browser.name_filter() == "one.txt");
    REQUIRE(f.browser.picker_folder()->entries().size() == 1);
    CHECK(f.browser.picker_folder()->entries().front().label() == "one.txt");
    f.browser.forward();
    REQUIRE(f.settle());
    CHECK(f.browser.searching_results());
    CHECK(f.browser.name_filter() == "*.zip");
    CHECK(f.chosen_results() == std::vector<fs::path>{first, second});
    CHECK_FALSE(f.browser.has_overlay());
}

TEST_CASE("native search: hidden toggle reruns the same query and scope") {
    Fixture f;
    const auto visible = f.a / "visible.zip";
    const auto hidden = f.a / ".hidden" / "secret.zip";
    touch(visible);
    touch(hidden);
    f.browser.open_search_results(f.a, "*.zip", false);
    REQUIRE(f.settle());
    REQUIRE(f.browser.picker_folder()->entries().size() == 1);
    CHECK(f.browser.picker_folder()->path_of(f.browser.picker_folder()->entries()[0]) == visible);
    f.browser.blur_filter();
    REQUIRE(f.press(key(XKB_KEY_h, kCtrl)));
    REQUIRE(f.settle());
    CHECK(f.browser.searching_results());
    CHECK(f.browser.search_results()->finder()->root() == f.a);
    CHECK(f.browser.search_results()->finder()->query() == "*.zip");
    CHECK(f.browser.name_filter() == "*.zip");
    CHECK(f.browser.search_results()->finder()->hidden());
    REQUIRE(f.browser.picker_folder()->entries().size() == 2);
    REQUIRE(f.press(key(XKB_KEY_h, kCtrl)));
    REQUIRE(f.settle());
    REQUIRE(f.browser.picker_folder()->entries().size() == 1);
    CHECK_FALSE(f.browser.search_results()->finder()->hidden());
}

TEST_CASE("native search: columns enter result folders and return to the result column") {
    Fixture f;
    f.browser.set_mode(Browser::Mode::columns);
    REQUIRE(f.settle());
    f.browser.open_search_results(f.a, "type:folder name:photo[s]", false);
    REQUIRE(f.settle());
    const auto folder = f.browser.picker_folder();
    REQUIRE(folder->is_search());
    REQUIRE(folder->entries().size() == 1);
    CHECK(folder->path_of(folder->entries()[0]) == f.a / "photos");
    f.select_result(0);
    f.browser.blur_filter();
    REQUIRE(f.press(key(XKB_KEY_Return)));
    REQUIRE(f.settle());
    CHECK(f.browser.searching_results());
    CHECK(f.browser.mode() == Browser::Mode::columns);
    CHECK(f.browser.picker_folder()->path() == f.a / "photos");
    REQUIRE(f.press(key(XKB_KEY_Left)));
    REQUIRE(f.settle());
    CHECK(f.browser.searching_results());
    CHECK(f.browser.picker_folder()->is_search());
    CHECK_FALSE(f.browser.has_overlay());
    CHECK(f.browser.mode() == Browser::Mode::columns);
    CHECK(f.browser.name_filter() == "type:folder name:photo[s]");
    CHECK(f.chosen_results() == std::vector<fs::path>{f.a / "photos"});
    f.browser.back();
    REQUIRE(f.settle());
    CHECK_FALSE(f.browser.searching_results());
    CHECK(f.browser.picker_folder()->path() == f.a);
    f.browser.forward();
    REQUIRE(f.settle());
    CHECK(f.browser.searching_results());
    CHECK(f.browser.picker_folder()->is_search());
    CHECK(f.browser.name_filter() == "type:folder name:photo[s]");
    CHECK(f.chosen_results() == std::vector<fs::path>{f.a / "photos"});
}

TEST_CASE("native search: a split pane keeps independent normal navigation") {
    Fixture f;
    f.browser.toggle_split();
    REQUIRE(f.settle());
    REQUIRE(f.browser.split_visible());
    f.browser.open_search_results(f.a, "*.txt", false);
    REQUIRE(f.settle());
    REQUIRE(f.browser.searching_results());
    const auto search_pane = f.browser.active_pane();
    f.browser.focus_pane(1 - search_pane);
    REQUIRE(f.settle());
    CHECK_FALSE(f.browser.searching_results());
    f.browser.open(f.a / "photos");
    REQUIRE(f.settle());
    CHECK(f.browser.picker_folder()->path() == f.a / "photos");
    CHECK_FALSE(f.browser.picker_folder()->is_collection());
    f.browser.focus_pane(search_pane);
    REQUIRE(f.settle());
    CHECK(f.browser.searching_results());
    CHECK(f.browser.name_filter() == "*.txt");
    REQUIRE(f.browser.picker_folder()->entries().size() == 1);
    CHECK(f.browser.picker_folder()->entries()[0].label() == "one.txt");
    CHECK_FALSE(f.browser.has_overlay());
    CHECK(f.browser.sidebar_visible());
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

TEST_CASE("search query: explicit modes, filters, quoted text and UTC dates") {
    using rummage::views::parse_search_query;
    const auto query = parse_search_query(
        "content:\"two words\" type:file ext:.TXT after:2024-02-29 before:2024-03-01 hidden:yes "
        "case:fold exclude:.git exclude:build/cache");
    CHECK(query.error.empty());
    CHECK(query.content);
    CHECK(query.text == "two words");
    CHECK(query.type == "file");
    CHECK(query.extension == "txt");
    CHECK(query.hidden);
    CHECK(query.fold_case);
    CHECK(parse_search_query("exclude:build/cache/").excluded ==
          std::vector<std::string>{"build/cache"});
    CHECK(query.excluded == std::vector<std::string>{".git", "build/cache"});
    REQUIRE(query.after);
    REQUIRE(query.before);
    CHECK(*query.before - *query.after == 86400);
    CHECK(parse_search_query("\"content:notes\"").text == "content:notes");
    CHECK_FALSE(parse_search_query("\"content:notes\"").content);
    for (const auto* invalid :
         {"after:2023-02-29", "before:tomorrow", "after:2024-01-02 before:2024-01-01",
          "type:unknown", "hidden:maybe", "case:maybe", "exclude:../outside", "exclude:/tmp",
          "content:\"unfinished"}) {
        INFO(invalid);
        CHECK_FALSE(parse_search_query(invalid).error.empty());
    }
}

namespace {
rummage::views::SearchMatches search_tree(const fs::path& root, const std::string& text,
                                          rummage::views::SearchBudget* supplied = nullptr) {
    const auto query = rummage::views::parse_search_query(text);
    std::vector<std::shared_ptr<const PathBatch>> batches;
    walk(root, {.hidden = query.hidden, .excluded = query.excluded}, {}, [&](PathBatch batch) {
        batches.push_back(std::make_shared<const PathBatch>(std::move(batch)));
    });
    rummage::views::SearchBudget budget;
    return rummage::views::match_files(root, batches, query, 200, {},
                                       supplied ? *supplied : budget);
}
}  // namespace

TEST_CASE("content search: literal text, context, case, binary, large files and links") {
    const TempDir dir;
    fs::create_directories(dir.path / "nested");
    std::ofstream{dir.path / "nested" / "notes.txt"}
        << "first line\nA Needle in UTF-8 café\nlast line\n";
    std::ofstream{dir.path / "other.md"} << "Needle too";
    std::ofstream binary{dir.path / "binary.bin"};
    binary.write("Needle\0bytes", 12);
    binary.close();
    std::ofstream{dir.path / "large.txt"} << "Needle";
    fs::resize_file(dir.path / "large.txt", rummage::views::SearchBudget::kFileBytes + 1);
    std::ofstream invalid{dir.path / "invalid.txt"};
    invalid << "Needle" << char(0xff);
    invalid.close();
    fs::create_symlink(dir.path / "nested" / "notes.txt", dir.path / "linked.txt");
    const auto exact = search_tree(dir.path, "content:Needle");
    REQUIRE(exact.files.size() == 2);
    CHECK(exact.stats.matches == 2);
    CHECK(exact.stats.binary == 2);
    CHECK(exact.stats.oversized == 1);
    const auto text = std::ranges::find(exact.files, "nested/notes.txt", &FoundFile::path);
    REQUIRE(text != exact.files.end());
    CHECK(text->line == 2);
    CHECK(text->context.find("café") != std::string::npos);
    CHECK(text->context.find('\n') == std::string::npos);
    CHECK(search_tree(dir.path, "content:needle").files.empty());
    CHECK(search_tree(dir.path, "content:needle case:fold").files.size() == 2);
    CHECK(search_tree(dir.path, "content:Needle ext:md").files.size() == 1);
    CHECK(search_tree(dir.path, "content:\"Needle.*\"").files.empty());
    CHECK(search_tree(dir.path, "content:").files.empty());
    CHECK(search_tree(dir.path, "type:link").files.size() == 1);
    CHECK(search_tree(dir.path, "type:folder").files.size() == 1);
    rummage::views::SearchBudget exhausted;
    exhausted.bytes = rummage::views::SearchBudget::kTotalBytes;
    const auto limited = search_tree(dir.path, "content:Needle", &exhausted);
    CHECK(limited.files.empty());
    CHECK(limited.stats.byte_limit);
}

TEST_CASE("content search: bounded reads cross chunks, refuse ancestor links and FIFOs") {
    const TempDir dir;
    fs::create_directories(dir.path / "inside");
    std::ofstream{dir.path / "inside" / "large.txt"} << std::string(65534, 'x') << "needle";
    auto batch = std::make_shared<PathBatch>();
    batch->add("inside/large.txt", false);
    const auto query = rummage::views::parse_search_query("content:needle");
    rummage::views::SearchBudget budget;
    CHECK(rummage::views::match_files(dir.path, {batch}, query, 10, {}, budget).files.size() == 1);
    fs::rename(dir.path / "inside", dir.path / "elsewhere");
    fs::create_directory_symlink(dir.path / "elsewhere", dir.path / "inside");
    const auto replaced = rummage::views::match_files(dir.path, {batch}, query, 10, {}, budget);
    CHECK(replaced.files.empty());
    CHECK(replaced.stats.unreadable == 1);
    REQUIRE(::mkfifo((dir.path / "pipe").c_str(), 0600) == 0);
    batch = std::make_shared<PathBatch>();
    batch->add("pipe", false);
    CHECK(rummage::views::match_files(dir.path, {batch}, query, 10, {}, budget).files.empty());
    CancelToken cancelled;
    cancelled.cancel();
    CHECK(
        rummage::views::match_files(dir.path, {batch}, query, 10, cancelled, budget).files.empty());
}

TEST_CASE("search filters: hidden entries, exclusions and modified date boundaries") {
    const TempDir dir;
    make_tree(dir.path);
    const auto excluded = walk_all(dir.path, {.hidden = true, .excluded = {"photos", ".hidden"}});
    CHECK(std::ranges::none_of(excluded.paths, [](const auto& path) {
        return path.starts_with("photos") || path.starts_with(".hidden");
    }));
    struct timespec times[2]{{.tv_sec = 1709164800, .tv_nsec = 0},
                             {.tv_sec = 1709164800, .tv_nsec = 0}};  // 2024-02-29 UTC
    REQUIRE(::utimensat(AT_FDCWD, (dir.path / "a.txt").c_str(), times, 0) == 0);
    const auto dates =
        search_tree(dir.path, "type:file ext:txt after:2024-02-29 before:2024-03-01");
    REQUIRE(dates.files.size() == 1);
    CHECK(dates.files.front().path == "a.txt");
    CHECK(search_tree(dir.path, "type:file ext:txt before:2024-02-29").files.empty());
    CHECK(search_tree(dir.path, "secret hidden:yes").files.size() == 1);
    CHECK(search_tree(dir.path, "secret hidden:no").files.empty());
}

TEST_CASE("file finder: changing modes and scan scope cannot deliver stale results") {
    const TempDir dir;
    make_tree(dir.path);
    EventLoop loop;
    WorkerPool pool{loop, 2};
    FileFinder finder{pool, [] {}, {.batch = 1}};
    finder.start(dir.path, false);
    finder.set_query("content:x");
    finder.set_query("secret hidden:yes");
    REQUIRE(run_until(loop, [&] { return !finder.scanning() && !finder.matching(); }));
    REQUIRE(finder.results().size() == 1);
    CHECK(finder.results().front().path == ".hidden/secret.orf");
    finder.set_query("type:invalid");
    CHECK_FALSE(finder.specification().error.empty());
    CHECK(finder.results().empty());
    finder.set_query("content:x exclude:photos hidden:no");
    REQUIRE(run_until(loop, [&] { return !finder.scanning() && !finder.matching(); }));
    CHECK(finder.results().size() == 2);
    finder.stop();
    CHECK(finder.stopped());
    CHECK_FALSE(finder.scanning());
    CHECK_FALSE(finder.matching());
    finder.refresh();
    REQUIRE(run_until(loop, [&] { return !finder.scanning() && !finder.matching(); }));
    CHECK_FALSE(finder.stopped());
    CHECK(finder.results().size() == 2);
}

TEST_CASE("file finder: result truncation is explicit and includes total matches") {
    const TempDir dir;
    for (int i = 0; i < 205; ++i)
        touch(dir.path / std::format("match-{}.txt", i));
    EventLoop loop;
    WorkerPool pool{loop, 2};
    FileFinder finder{pool, [] {}, {.batch = 10}};
    finder.start(dir.path, false);
    finder.set_query("match");
    REQUIRE(run_until(loop, [&] { return !finder.scanning() && !finder.matching(); }));
    CHECK(finder.results().size() == FileFinder::kMaxResults);
    CHECK(finder.stats().matches == 205);
    CHECK(finder.status().find("Showing 200 of 205 matches") != std::string::npos);
}

TEST_CASE("search results: promote a live query, select across parents, copy and undo") {
    Fixture f;
    touch(f.a / "photos" / "two.txt");
    fs::create_directory(f.dir.path / "destination");
    f.find("ext:txt");
    CHECK(f.press(key(XKB_KEY_Return, kCtrl)));
    auto* results = f.browser.search_results();
    REQUIRE(results != nullptr);
    REQUIRE(results->is_open());
    REQUIRE(f.settle());
    CHECK(results->finder()->query() == "ext:txt");
    CHECK(results->finder()->root() == f.a);
    REQUIRE(results->rows().size() == 2);
    f.browser.picker_folder()->select_all();
    REQUIRE(f.chosen_results().size() == 2);
    f.browser.search_action(rummage::views::SearchResults::Action::copy);
    f.browser.search_action(rummage::views::SearchResults::Action::close);
    f.browser.open(f.dir.path / "destination");
    REQUIRE(f.settle());
    REQUIRE(f.press(key(XKB_KEY_v, kCtrl)));
    REQUIRE(f.settle());
    CHECK(fs::exists(f.dir.path / "destination" / "one.txt"));
    CHECK(fs::exists(f.dir.path / "destination" / "two.txt"));
    REQUIRE(f.press(key(XKB_KEY_z, kCtrl)));
    REQUIRE(f.settle());
    CHECK_FALSE(fs::exists(f.dir.path / "destination" / "one.txt"));
    CHECK_FALSE(fs::exists(f.dir.path / "destination" / "two.txt"));
    CHECK(fs::exists(f.a / "one.txt"));
    CHECK(fs::exists(f.a / "photos" / "two.txt"));
}

TEST_CASE("search results: cut from different parents, conflict choices and a single undo") {
    Fixture f;
    touch(f.a / "photos" / "two.txt");
    const auto destination = f.dir.path / "destination";
    touch(destination / "one.txt");
    f.browser.open_search_results(f.a, "ext:txt", false);
    REQUIRE(f.settle());
    f.browser.picker_folder()->select_all();
    f.browser.search_action(rummage::views::SearchResults::Action::cut);
    f.browser.search_action(rummage::views::SearchResults::Action::close);
    f.browser.open(destination);
    REQUIRE(f.settle());
    REQUIRE(f.press(key(XKB_KEY_v, kCtrl)));
    REQUIRE(run_until(f.loop, [&] { return f.browser.conflict_dialog().is_open(); }));
    CHECK(f.press(key(XKB_KEY_Return)));  // keep both
    REQUIRE(f.settle());
    CHECK(fs::exists(destination / "one 2.txt"));
    CHECK(fs::exists(destination / "two.txt"));
    CHECK_FALSE(fs::exists(f.a / "one.txt"));
    CHECK_FALSE(fs::exists(f.a / "photos" / "two.txt"));
    CHECK(f.press(key(XKB_KEY_z, kCtrl)));
    REQUIRE(f.settle());
    CHECK(fs::exists(f.a / "one.txt"));
    CHECK(fs::exists(f.a / "photos" / "two.txt"));
    CHECK(fs::exists(destination / "one.txt"));
}

TEST_CASE("search results: trash multiple parents and undo while results stay open") {
    Fixture f;
    touch(f.a / "photos" / "two.txt");
    f.browser.open_search_results(f.a, "ext:txt", false);
    REQUIRE(f.settle());
    auto& results = *f.browser.search_results();
    f.browser.picker_folder()->select_all();
    f.browser.search_action(rummage::views::SearchResults::Action::trash);
    REQUIRE(f.settle());
    CHECK(results.is_open());
    CHECK(results.rows().empty());
    CHECK_FALSE(fs::exists(f.a / "one.txt"));
    CHECK_FALSE(fs::exists(f.a / "photos" / "two.txt"));
    f.browser.search_action(rummage::views::SearchResults::Action::undo);
    REQUIRE(f.settle());
    CHECK(results.rows().size() == 2);
    CHECK(fs::exists(f.a / "one.txt"));
    CHECK(fs::exists(f.a / "photos" / "two.txt"));
}

TEST_CASE("search results: preview keeps the search open and rename refreshes with undo") {
    Fixture f;
    f.browser.open_search_results(f.a, "ext:txt", false);
    REQUIRE(f.settle());
    auto& results = *f.browser.search_results();
    f.select_result(0);
    REQUIRE(f.chosen_results().size() == 1);
    f.browser.search_action(rummage::views::SearchResults::Action::preview);
    REQUIRE(run_until(f.loop, [&] {
        f.browser.settle_search_preview();
        return f.browser.quick_look().is_open();
    }));
    CHECK(results.is_open());
    CHECK(f.browser.grid().folder()->is_search());
    f.press(key(XKB_KEY_Escape));
    CHECK_FALSE(f.browser.quick_look().is_open());
    CHECK(results.is_open());
    f.browser.search_action(rummage::views::SearchResults::Action::rename);
    REQUIRE(f.browser.confirm_dialog().is_open());
    f.browser.confirm_dialog().paste("renamed.txt");
    CHECK(f.press(key(XKB_KEY_Return)));
    REQUIRE(f.settle());
    CHECK(fs::exists(f.a / "renamed.txt"));
    REQUIRE(results.rows().size() == 1);
    CHECK(results.rows().front().path == "renamed.txt");
    f.browser.search_action(rummage::views::SearchResults::Action::undo);
    REQUIRE(f.settle());
    CHECK(fs::exists(f.a / "one.txt"));
}

TEST_CASE(
    "search results: overlapping selections act on the parent once and Ctrl-click deselects") {
    Fixture f;
    f.browser.open_search_results(f.a, "*", false);
    REQUIRE(f.settle());
    auto& results = *f.browser.search_results();
    f.browser.picker_folder()->select_all();
    const auto paths = f.chosen_results();
    CHECK(paths.size() == 5);  // Native selection includes descendants; actions prune overlaps.
    CHECK(std::ranges::find(paths, f.a / "photos") != paths.end());
    f.browser.set_name_filter("one");
    REQUIRE(f.settle());
    f.select_result(0);
    REQUIRE(f.chosen_results().size() == 1);
    f.select_result(0, rummage::views::SelectMode::toggle);
    CHECK(f.chosen_results().empty());
    f.select_result(0);
    fs::remove(f.a / "one.txt");
    f.browser.search_action(rummage::views::SearchResults::Action::trash);
    REQUIRE(f.settle());
    CHECK(results.rows().empty());
    CHECK(f.browser.status_bar().message().find("one.txt") != std::string::npos);
}

TEST_CASE("saved searches: persist definitions, rerun current files, and remove from navigation") {
    Fixture f;
    f.browser.open_search_results(f.a, "ext:txt hidden:no", true);
    REQUIRE(f.settle());
    f.browser.search_action(rummage::views::SearchResults::Action::save);
    REQUIRE(f.browser.confirm_dialog().is_open());
    f.browser.confirm_dialog().paste("Text files");
    CHECK(f.press(key(XKB_KEY_Return)));
    const auto state = f.browser.state();
    REQUIRE(state.saved_searches.size() == 1);
    CHECK(state.saved_searches.front().query == "ext:txt hidden:no");
    CHECK(state.saved_searches.front().root == f.a);
    CHECK(state.saved_searches.front().hidden);
    const auto restored = rummage::views::parse_state(rummage::views::format_state(state), "state");
    CHECK(restored.problems.empty());
    CHECK(restored.state.saved_searches == state.saved_searches);
    f.browser.search_action(rummage::views::SearchResults::Action::close);
    touch(f.a / "new.txt");
    f.browser.apply(restored.state);
    f.browser.open_saved_searches();
    auto& palette = f.browser.command_palette();
    palette.set_query("Text files");
    palette.choose_highlighted();
    REQUIRE(f.settle());
    REQUIRE(f.browser.search_results()->is_open());
    CHECK(f.browser.search_results()->rows().size() == 2);
    f.browser.open_saved_searches();
    palette.set_query("Remove saved search");
    palette.choose_highlighted();
    palette.set_query("Text files");
    palette.choose_highlighted();
    CHECK(f.browser.state().saved_searches.empty());
}

TEST_CASE("search results: normal file commands act on results and collection drops are rejected") {
    Fixture f;
    f.browser.open_search_results(f.a, "ext:txt", false);
    REQUIRE(f.settle());
    f.select_result(0);
    f.browser.blur_filter();
    f.browser.open_palette();
    const auto& items = f.browser.command_palette().items();
    CHECK(std::ranges::any_of(items, [](const auto& item) { return item.id == "command:copy"; }));
    CHECK(std::ranges::none_of(items,
                               [](const auto& item) { return item.id == "command:new_folder"; }));
    f.browser.command_palette().close();
    rummage::ui::Offscreen surface{800, 600};
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    renderer.begin_frame(800, 600, 1, {});
    f.browser.draw(renderer, {0, 0, 800, 600}, {}, icons, Browser::Clock::now());
    renderer.end_frame();
    const auto bounds = f.browser.pane_bounds(0);
    REQUIRE_FALSE(bounds.empty());
    bool rejected = false, read = false;
    rummage::views::DragAndDrop drag;
    drag.accept = [&](const auto& type, std::uint32_t actions, std::uint32_t) {
        rejected = !type && actions == 0;
    };
    drag.types = [] { return std::vector<std::string>{"text/uri-list"}; };
    drag.read = [&](const auto&, auto) { read = true; };
    f.browser.set_drag_and_drop(std::move(drag));
    f.browser.drop_event({.kind = rummage::ui::DropEvent::Kind::enter,
                          .x = bounds.right() - 20,
                          .y = bounds.bottom() - 20});
    f.browser.drop_event({.kind = rummage::ui::DropEvent::Kind::drop,
                          .x = bounds.right() - 20,
                          .y = bounds.bottom() - 20});
    CHECK(rejected);
    CHECK_FALSE(read);
}

TEST_CASE("search results: a removed preview target never falls back to another file") {
    Fixture f;
    f.browser.open_search_results(f.a, "one", false);
    REQUIRE(f.settle());
    f.select_result(0);
    fs::remove(f.a / "one.txt");
    touch(f.a / "different.txt");
    f.browser.search_action(rummage::views::SearchResults::Action::preview);
    REQUIRE(f.settle());
    f.browser.settle_search_preview();
    CHECK_FALSE(f.browser.quick_look().is_open());
    CHECK(f.browser.search_results()->is_open());
    CHECK(f.browser.status_bar().message().find("no longer available") != std::string::npos);
    f.browser.open_search_results(f.a / "missing", "", false);
    REQUIRE(f.settle());
    CHECK(f.browser.search_results()->rows().empty());
    CHECK_FALSE(f.browser.search_results()->finder()->status().empty());
}

TEST_CASE("search results: Back restores query and selection with refreshed paths per tab") {
    Fixture f;
    f.browser.open_search_results(f.a, "ext:orf", true);
    REQUIRE(f.settle());
    auto* results = f.browser.search_results();
    REQUIRE(results);
    REQUIRE(results->rows().size() == 2);
    f.select_result(1);
    const auto selected = f.chosen_results();
    f.browser.search_action(rummage::views::SearchResults::Action::reveal);
    REQUIRE(f.settle());
    CHECK_FALSE(f.browser.searching_results());
    f.browser.back();
    REQUIRE(f.settle());
    REQUIRE(f.browser.searching_results());
    CHECK(f.browser.search_results()->finder()->root() == f.a);
    CHECK(f.browser.search_results()->finder()->query() == "ext:orf");
    CHECK(f.browser.search_results()->finder()->hidden());
    CHECK(f.chosen_results() == selected);
    CHECK(f.browser.picker_folder()->selected_count() == selected.size());
    f.browser.open_tab(f.a, true);
    REQUIRE(f.settle());
    CHECK_FALSE(f.browser.searching_results());
    f.browser.open_search_results(f.a, "ext:txt", false);
    REQUIRE(f.settle());
    f.browser.select_tab(0);
    REQUIRE(f.settle());
    CHECK(f.browser.search_results()->finder()->query() == "ext:orf");
    f.browser.forward();
    REQUIRE(f.settle());
    CHECK_FALSE(f.browser.searching_results());
    fs::remove(selected.front());
    f.browser.back();
    REQUIRE(f.settle());
    REQUIRE(f.browser.searching_results());
    const auto remaining = f.chosen_results();
    CHECK(std::ranges::find(remaining, selected.front()) == remaining.end());
}

TEST_CASE("search results: restoring a snapshot cancels an earlier search") {
    Fixture f;
    for (int i = 0; i < 30; ++i)
        touch(f.a / ("extra-" + std::to_string(i) + ".txt"));
    f.browser.open_search_results(f.a, "ext:txt", false);
    REQUIRE(f.settle());
    auto* results = f.browser.search_results();
    results->select(20);
    const auto saved = results->snapshot();
    CHECK(saved.first > 0);
    results->set_query("not-present");
    results->restore(saved);
    REQUIRE(f.settle());
    CHECK(results->snapshot().cursor == saved.cursor);
    CHECK(results->first_row() == saved.first);
}

TEST_CASE("search results: unavailable Forward leaves the current search open") {
    Fixture f;
    f.browser.open_search_results(f.a, "ext:txt", false);
    REQUIRE(f.settle());
    f.browser.forward();
    REQUIRE(f.browser.searching_results());
    CHECK(f.browser.search_results()->finder()->query() == "ext:txt");
}

TEST_CASE("find files: Back restores a chosen palette result as a search session") {
    Fixture f;
    for (int i = 0; i < 30; ++i)
        touch(f.a / ("extra-" + std::to_string(i) + ".txt"));
    f.find("ext:txt");
    for (int i = 0; i < 20; ++i)
        REQUIRE(f.press(key(XKB_KEY_Down)));
    const auto& palette = f.browser.command_palette();
    REQUIRE(palette.highlighted());
    const auto path = palette.items()[palette.shown()[*palette.highlighted()]].id.substr(5);
    const auto first = palette.first_row();
    REQUIRE(first > 0);
    REQUIRE(f.press(key(XKB_KEY_Return)));
    REQUIRE(f.settle());
    CHECK_FALSE(f.browser.command_palette().is_open());
    f.browser.back();
    REQUIRE(f.settle());
    REQUIRE(f.browser.searching_results());
    const auto* results = f.browser.search_results();
    CHECK(results->finder()->root() == f.a);
    CHECK(results->finder()->query() == "ext:txt");
    CHECK(f.chosen_results() == std::vector<fs::path>{f.a / path});
    REQUIRE(f.browser.picker_folder()->cursor_entry());
    CHECK(f.browser.picker_folder()->path_of(*f.browser.picker_folder()->cursor_entry()) ==
          f.a / path);
}

TEST_CASE("annotation search: exact tags rating and comment predicates") {
    const TempDir dir;
    const auto file = dir.write("notes.txt", "data");
    const rummage::fs::Annotations values{
        .tags = "Travel, blue", .rating = "4", .comment = "A great trip", .error = {}};
    const bool xattrs = rummage::test::user_xattrs_available(file);
    const auto error =
        rummage::fs::write_annotations(rummage::fs::permission_target(file), {}, values, 7);
    REQUIRE(error.empty() == xattrs);
    CHECK(search_tree(dir.path, "notes.txt").files.size() == 1);
    CHECK(search_tree(dir.path, "tag:Travel").files.size() == (xattrs ? 1 : 0));
    CHECK(search_tree(dir.path, "tag:Tra").files.empty());
    CHECK(search_tree(dir.path, "tag:travel case:fold rating:4 comment:trip").files.size() ==
          (xattrs ? 1 : 0));
    CHECK(search_tree(dir.path, "rating:3").files.empty());
    CHECK_FALSE(rummage::views::parse_search_query("rating:6").error.empty());
}

TEST_CASE("recent files: collection filters restores and acts on actual source parents") {
    Fixture f;
    rummage::fs::RecentFiles history;
    REQUIRE(history.set_enabled(true).empty());
    REQUIRE(history.clear().empty());
    const auto one = f.a / "one.txt";
    const auto other = f.a / "photos" / "shot.orf";
    REQUIRE(history.record({one, other}).empty());
    f.browser.open_search_results("rummage:recent");
    REQUIRE(f.settle());
    auto* results = f.browser.search_results();
    REQUIRE(results);
    REQUIRE(results->rows().size() == 2);
    CHECK(results->finder()->recent());
    CHECK(results->rows()[0].context.find("UTC") != std::string::npos);
    f.browser.picker_folder()->select_all();
    CHECK(f.chosen_results() == std::vector<fs::path>{one, other});
    f.browser.set_name_filter("one.txt");
    REQUIRE(f.settle());
    REQUIRE(results->rows().size() == 1);
    f.select_result(0);
    const auto saved = results->snapshot();
    CHECK(saved.root == "rummage:recent");
    f.browser.search_action(rummage::views::SearchResults::Action::reveal);
    REQUIRE(f.settle());
    CHECK_FALSE(f.browser.searching_results());
    f.browser.back();
    REQUIRE(f.settle());
    REQUIRE(f.browser.searching_results());
    results = f.browser.search_results();
    CHECK(results->finder()->recent());
    CHECK(f.chosen_results() == std::vector<fs::path>{one});
    if (const char* output = std::getenv("RUMMAGE_RECENT_FRAME")) {
        rummage::ui::Offscreen gl{700, 600};
        rummage::ui::FontSystem fonts;
        rummage::ui::Renderer renderer{fonts};
        renderer.begin_frame(700, 600, 1, {});
        rummage::ui::IconSet icons{fonts};
        f.browser.draw(renderer, {0, 0, 700, 600}, rummage::theme::Palette{}, icons,
                       Browser::Clock::now());
        renderer.end_frame();
        std::vector<unsigned char> pixels(700 * 600 * 4);
        glReadPixels(0, 0, 700, 600, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        std::ofstream image{output, std::ios::binary};
        image << "P6\n700 600\n255\n";
        for (int y = 599; y >= 0; --y)
            for (int x = 0; x < 700; ++x)
                image.write(reinterpret_cast<const char*>(pixels.data() + (y * 700 + x) * 4), 3);
        REQUIRE(image.good());
    }
    fs::remove(one);
    results->refresh();
    REQUIRE(f.settle());
    CHECK(results->rows().empty());
    REQUIRE(history.set_enabled(false).empty());
    results->refresh();
    REQUIRE(f.settle());
    CHECK(results->finder()->status() == "Recent history is disabled");
    REQUIRE(history.set_enabled(true).empty());
}

TEST_CASE("recursive filename globs: basename patterns compose with metadata and path scope") {
    const TempDir dir;
    for (const auto* path : {"first.zip", "nested/second.ZIP", "nested/deep/third.zip",
                             "nested/notes.txt", "not-a-zip.zip.txt", ".hidden/fourth.zip"})
        touch(dir.path / path);
    fs::create_directory(dir.path / "bundles.zip");
    const auto paths = [&](const std::string& query) {
        std::vector<std::string> found;
        for (const auto& file : search_tree(dir.path, query).files)
            found.push_back(file.path);
        std::ranges::sort(found);
        return found;
    };
    const std::vector<std::string> zip_files{"first.zip", "nested/deep/third.zip",
                                             "nested/second.ZIP"};
    CHECK(paths("*.zip type:file") == zip_files);
    CHECK(paths("name:*.zip ext:ZIP type:file") == zip_files);
    CHECK(paths("*.zip type:folder") == std::vector<std::string>{"bundles.zip"});
    CHECK(paths("*.zip").size() == 4);
    CHECK(paths("*.zip hidden:yes type:file").size() == 4);
    CHECK(paths("*.zip exclude:nested type:file") == std::vector<std::string>{"first.zip"});
    CHECK(paths("nested/*.zip") == std::vector<std::string>{"nested/second.ZIP"});
    CHECK(paths("nested/**/*.zip") ==
          std::vector<std::string>{"nested/deep/third.zip", "nested/second.ZIP"});
    CHECK(paths("./nested/**/*.zip") == paths("nested/**/*.zip"));
    CHECK(paths("**/*.zip type:file") == zip_files);
    CHECK(paths("nested/*/*.zip") == std::vector<std::string>{"nested/deep/third.zip"});
    CHECK(paths("*/third.zip").empty());
    CHECK(paths("fstzp") == std::vector<std::string>{"first.zip"});  // plain fuzzy unchanged
    for (const auto* invalid : {"/*.zip", "nested//*.zip", "nested/*.zip/"})
        CHECK_FALSE(rummage::views::parse_search_query(invalid).error.empty());
    CHECK_FALSE(rummage::views::parse_search_query("name:broken[").error.empty());
    CHECK_FALSE(rummage::views::parse_search_query(std::string(1024, '*')).error.empty());
    CHECK(rummage::views::parse_search_query("content:broken[").error.empty());

    std::ofstream{dir.path / "nested/notes.txt"} << "The literal *.zip and [broken characters.\n";
    CHECK(paths("content:\"*.zip\" ext:txt") == std::vector<std::string>{"nested/notes.txt"});
    CHECK(paths("content:[broken") == std::vector<std::string>{"nested/notes.txt"});
    CHECK(paths("content:*.tar").empty());
}

TEST_CASE("filename glob matcher: escapes ranges unusual names ranking and cancellation") {
    auto batch = std::make_shared<PathBatch>();
    for (const auto* path : {"first.zip", "nested/second.zip", "nested/deep/third.zip",
                             "literal*.zip", "line\nbreak.zip", "zip-folder/notes.txt"})
        batch->add(path, false);
    auto matches = match_paths({batch}, "*.zip", 200, {});
    REQUIRE(matches.size() == 5);
    CHECK(matches.front().path == "first.zip");
    CHECK(match_paths({batch}, "[fl]*.zip", 200, {}).size() == 3);
    matches = match_paths({batch}, R"(literal\*.zip)", 200, {});
    REQUIRE(matches.size() == 1);
    CHECK(matches.front().path == "literal*.zip");
    CHECK(match_paths({batch}, "?????.zip", 200, {}).size() == 2);
    CHECK(match_paths({batch}, "*.zip", 2, {}).size() == 2);
    CHECK(match_paths({batch}, "broken[", 200, {}).empty());
    CancelToken cancelled;
    cancelled.cancel();
    CHECK(match_paths({batch}, "**/*.zip", 200, cancelled).empty());
}

TEST_CASE("file finder: asynchronous glob query changes retain all matching nested archives") {
    const TempDir dir;
    for (const auto* path :
         {"first.zip", "nested/second.zip", "nested/deep/third.zip", "notes.txt"})
        touch(dir.path / path);
    EventLoop loop;
    WorkerPool pool{loop, 2};
    FileFinder finder{pool, [] {}, {.batch = 1}};
    finder.set_query("*.zip");
    finder.start(dir.path, false);
    finder.set_query("name:*.txt type:file");
    REQUIRE(run_until(loop, [&] { return !finder.scanning() && !finder.matching(); }));
    REQUIRE(finder.results().size() == 1);
    CHECK(finder.results().front().path == "notes.txt");
    finder.set_query("name:*.zip ext:zip type:file");
    REQUIRE(run_until(loop, [&] { return !finder.scanning() && !finder.matching(); }));
    CHECK(finder.results().size() == 3);
    CHECK(finder.stats().matches == 3);
    finder.set_query("*.zip[");
    CHECK_FALSE(finder.specification().error.empty());
    CHECK(finder.results().empty());
    finder.set_query("**/*.zip");
    REQUIRE(run_until(loop, [&] { return !finder.scanning() && !finder.matching(); }));
    CHECK(finder.specification().error.empty());
    CHECK(finder.results().size() == 3);
    finder.set_query("nested/*.zip");
    finder.set_query("*.zip");
    REQUIRE(run_until(loop, [&] { return !finder.scanning() && !finder.matching(); }));
    CHECK(finder.results().size() == 3);
}

TEST_CASE("find files: wildcard archive queries populate quick finder and persistent results") {
    Fixture f;
    for (const auto* path : {"first.zip", "nested/second.zip", "nested/deep/third.zip"})
        touch(f.a / path);
    // Call the explicit quick finder: Ctrl+F may independently choose the persistent UI.
    f.browser.open_finder();
    auto& palette = f.browser.command_palette();
    palette.set_query("*.zip");
    REQUIRE(run_until(f.loop, [&] {
        const auto* finder = f.browser.file_finder();
        return finder && !finder->scanning() && !finder->matching();
    }));
    REQUIRE(f.browser.file_finder()->results().size() == 3);
    REQUIRE(palette.shown().size() == 3);
    REQUIRE(f.press(key(XKB_KEY_Return, kCtrl)));
    auto* results = f.browser.search_results();
    REQUIRE(results);
    REQUIRE(f.settle());
    REQUIRE(results->rows().size() == 3);
    f.browser.picker_folder()->select_all();
    CHECK(f.chosen_results().size() == 3);
    f.browser.set_name_filter("name:nested/*.zip type:file");
    REQUIRE(f.settle());
    REQUIRE(results->rows().size() == 1);
    CHECK(results->rows().front().path == "nested/second.zip");
    f.browser.set_name_filter("name:*.zip ext:zip");
    REQUIRE(f.settle());
    REQUIRE(results->rows().size() == 3);
    CHECK(results->finder()->specification().error.empty());
}
