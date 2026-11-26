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
    results->select_all();
    REQUIRE(results->chosen().size() == 2);
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
    f.browser.search_results()->select_all();
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
    results.select_all();
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
    REQUIRE(results.chosen().size() == 1);
    f.browser.search_action(rummage::views::SearchResults::Action::preview);
    REQUIRE(run_until(f.loop, [&] {
        f.browser.settle_search_preview();
        return f.browser.quick_look().is_open();
    }));
    CHECK(results.is_open());
    CHECK(f.browser.grid().folder()->path() == f.a);
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
    f.browser.open_search_results(f.a, "", false);
    REQUIRE(f.settle());
    auto& results = *f.browser.search_results();
    results.select_all();
    const auto paths = results.chosen();
    CHECK(paths.size() == 2);  // one.txt and photos; photos includes all of its descendants
    CHECK(std::ranges::find(paths, f.a / "photos") != paths.end());
    results.set_query("one");
    REQUIRE(f.settle());
    REQUIRE(results.chosen().size() == 1);
    results.select(0, kCtrl);
    CHECK(results.chosen().empty());
    results.select(0);
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

TEST_CASE(
    "search results: the command palette acts on results and drops cannot reach the folder "
    "behind") {
    Fixture f;
    f.browser.open_search_results(f.a, "ext:txt", false);
    REQUIRE(f.settle());
    f.browser.open_palette();
    const auto& items = f.browser.command_palette().items();
    CHECK(std::ranges::any_of(items, [](const auto& item) { return item.id == "search:copy"; }));
    CHECK(std::ranges::none_of(items, [](const auto& item) {
        return item.id == "command:new_folder" || item.id == "command:trash";
    }));
    f.browser.command_palette().close();
    bool rejected = false, read = false;
    rummage::views::DragAndDrop drag;
    drag.accept = [&](const auto& type, std::uint32_t actions, std::uint32_t) {
        rejected = !type && actions == 0;
    };
    drag.read = [&](const auto&, auto) { read = true; };
    f.browser.set_drag_and_drop(std::move(drag));
    f.browser.drop_event({.kind = rummage::ui::DropEvent::Kind::drop});
    CHECK(rejected);
    CHECK_FALSE(read);
}

TEST_CASE("search results: a removed preview target never falls back to another file") {
    Fixture f;
    f.browser.open_search_results(f.a, "one", false);
    REQUIRE(f.settle());
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
    results->select(1);
    const auto selected = results->chosen();
    const auto before = results->snapshot();
    f.browser.search_action(rummage::views::SearchResults::Action::reveal);
    REQUIRE(f.settle());
    CHECK_FALSE(f.browser.searching_results());
    f.browser.back();
    REQUIRE(f.settle());
    REQUIRE(f.browser.searching_results());
    CHECK(f.browser.search_results()->finder()->root() == f.a);
    CHECK(f.browser.search_results()->finder()->query() == "ext:orf");
    CHECK(f.browser.search_results()->finder()->hidden());
    CHECK(f.browser.search_results()->chosen() == selected);
    CHECK(f.browser.search_results()->first_row() == before.first);
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
    const auto remaining = f.browser.search_results()->chosen();
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
    CHECK(results->chosen() == std::vector<fs::path>{f.a / path});
    CHECK(results->first_row() == first);
}

TEST_CASE("annotation search: exact tags rating and comment predicates") {
    const TempDir dir;
    const auto file = dir.write("notes.txt", "data");
    const rummage::fs::Annotations values{
        .tags = "Travel, blue", .rating = "4", .comment = "A great trip", .error = {}};
    REQUIRE(rummage::fs::write_annotations(rummage::fs::permission_target(file), {}, values, 7)
                .empty());
    CHECK(search_tree(dir.path, "tag:Travel").files.size() == 1);
    CHECK(search_tree(dir.path, "tag:Tra").files.empty());
    CHECK(search_tree(dir.path, "tag:travel case:fold rating:4 comment:trip").files.size() == 1);
    CHECK(search_tree(dir.path, "rating:3").files.empty());
    CHECK_FALSE(rummage::views::parse_search_query("rating:6").error.empty());
}
