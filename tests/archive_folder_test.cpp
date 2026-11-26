// Archives seen as folders (ROADMAP 0.9.0): the tree made from an engine's listing, where a path
// goes into an archive, and DirectoryModel showing a folder inside one (the engine in this
// process, as tests/main.cpp sets it).
#include "rummage/fs/archive_folder.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "rummage/archive/archive.hpp"
#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/engines/engines.hpp"
#include "rummage/formats/image/load.hpp"
#include "rummage/fs/directory_model.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/extract.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/preview/preview.hpp"
#include "rummage/views/browser.hpp"
#include "rummage/views/status_bar.hpp"
#include "temp_dir.hpp"

using rummage::archive::Entry;
using rummage::archive::Format;
using rummage::archive::Listing;
using rummage::archive::Type;
using rummage::core::EventLoop;
using rummage::core::WorkerPool;
using rummage::fs::ArchiveTree;
using rummage::fs::DirectoryModel;
using rummage::test::TempDir;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

const fs::path kArchives = fs::path{RUMMAGE_TEST_DATA} / "archive";

Entry item(std::string path, Type type = Type::file, std::uint64_t size = 1) {
    Entry entry;
    entry.path = std::move(path);
    entry.type = type;
    entry.size = size;
    return entry;
}

std::vector<std::string> names_of(const std::vector<rummage::fs::Entry>* entries) {
    std::vector<std::string> names;
    if (entries != nullptr) {
        for (const rummage::fs::Entry& entry : *entries) {
            names.push_back(entry.name + (entry.is_directory() ? "/" : ""));
        }
    }
    std::ranges::sort(names);
    return names;
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

struct Fixture {
    TempDir dir;
    EventLoop loop;
    WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    DirectoryModel model{loop, pool, watcher, rummage::fs::MimeDatabase::load(), 10ms};
    bool loaded = false;

    Fixture() {
        rummage::fs::forget_archive_trees();
        model.set_change_handler([this](const rummage::fs::ModelChange& change) {
            loaded = loaded || change.loaded || (change.reset && model.error());
        });
        fs::copy_file(kArchives / "basic.zip", dir.path / "basic.zip");
        dir.write("notes.txt", "not an archive\n");
    }

    void open(const fs::path& path) {
        loaded = false;
        model.open(path);
        REQUIRE(run_until(loop, [this] { return loaded && !model.loading(); }));
    }

    [[nodiscard]] std::vector<std::string> names() const {
        std::vector<std::string> out;
        for (const rummage::fs::Entry& entry : model.listing().entries()) {
            out.push_back(entry.name + (entry.is_directory() ? "/" : ""));
        }
        std::ranges::sort(out);
        return out;
    }
};

}  // namespace

TEST_CASE("archive folders: inner paths are made safe, or refused") {
    using rummage::fs::clean_inner_path;
    CHECK(clean_inner_path("") == std::string{});
    CHECK(clean_inner_path("./a//b/") == std::string{"a/b"});
    CHECK_FALSE(clean_inner_path("/etc/passwd"));
    CHECK(clean_inner_path("a/./b") == std::string{"a/b"});
    CHECK_FALSE(clean_inner_path("../evil").has_value());
    CHECK_FALSE(clean_inner_path("a/../../evil").has_value());
    CHECK_FALSE(clean_inner_path("a/..").has_value());
    CHECK_FALSE(clean_inner_path(std::string_view{"a\0b", 3}).has_value());
    CHECK_FALSE(clean_inner_path(std::string(256, 'n')).has_value());
    CHECK(clean_inner_path(std::string(255, 'n')).has_value());
}

TEST_CASE("archive folders: the tree makes up folders, leaves out what leads outside") {
    Listing listing;
    listing.format = Format::tar;
    listing.entries = {item("./top.txt"),
                       item("docs/a.txt"),
                       item("docs/deep/b.txt"),
                       item("../evil.txt"),
                       item("/abs/c.txt"),
                       item("docs/a.txt", Type::file, 99),  // a later one wins, as in tar
                       item("clash"),
                       item("clash/inside.txt"),  // a folder where a file is: the folder wins
                       item("empty/", Type::directory)};
    const ArchiveTree tree{listing, "", nullptr};
    CHECK(names_of(tree.folder("")) ==
          std::vector<std::string>{"clash/", "docs/", "empty/", "top.txt"});
    CHECK(names_of(tree.folder("docs")) == std::vector<std::string>{"a.txt", "deep/"});
    CHECK(names_of(tree.folder("docs/deep")) == std::vector<std::string>{"b.txt"});
    CHECK(tree.folder("abs") == nullptr);
    CHECK(names_of(tree.folder("clash")) == std::vector<std::string>{"inside.txt"});
    REQUIRE(tree.folder("empty") != nullptr);
    CHECK(tree.folder("empty")->empty());
    CHECK(tree.folder("nowhere") == nullptr);
    CHECK(tree.left_out() == 3);  // traversal, absolute path, and the file "clash"
    REQUIRE(tree.entry("docs/a.txt") != nullptr);
    CHECK(tree.entry("docs/a.txt")->size == 99);
    CHECK(tree.entry("docs") == nullptr);  // only named by what is in it
    CHECK(tree.entry("clash") == nullptr);

    Listing gzip;
    gzip.format = Format::gzip;
    gzip.entries = {item("")};
    const ArchiveTree single{gzip, "notes.txt", nullptr};
    CHECK(names_of(single.folder("")) == std::vector<std::string>{"notes.txt"});
}

TEST_CASE("archive folders: the tree counts what it shows") {
    Listing listing;
    listing.format = Format::zip;
    Entry locked = item("docs/secret.txt", Type::file, 10);
    locked.unreadable = rummage::archive::Unreadable::encrypted;
    Entry squeezed = item("docs/deep/x.bin", Type::file, 100);
    squeezed.unreadable = rummage::archive::Unreadable::method;
    Entry link = item("to-top", Type::symlink, 0);
    link.link = "top.txt";
    listing.entries = {item("top.txt", Type::file, 5),
                       item("same.txt", Type::hardlink, 0),
                       locked,
                       squeezed,
                       link,
                       item("fifo", Type::other, 0),
                       item("../evil.txt", Type::file, 1000),  // left out: not counted
                       item("empty/", Type::directory, 0)};
    const ArchiveTree tree{listing, "", nullptr};
    CHECK(tree.totals() == rummage::fs::ArchiveTotals{.files = 4,
                                                      .folders = 3,  // docs, docs/deep, empty
                                                      .links = 1,
                                                      .others = 1,
                                                      .bytes = 115,
                                                      .encrypted = 1,
                                                      .unsupported = 1});
}

TEST_CASE(
    "archive folders: a tree kept untyped is not given where types are asked; a typed one does "
    "untyped") {
    TempDir dir;
    fs::copy_file(kArchives / "basic.zip", dir.path / "basic.zip");
    rummage::fs::forget_archive_trees();
    const std::shared_ptr<const rummage::fs::MimeDatabase> mime = rummage::fs::MimeDatabase::load();
    const auto untyped = rummage::fs::archive_tree(dir.path / "basic.zip", nullptr);
    const auto typed = rummage::fs::archive_tree(dir.path / "basic.zip", mime.get());
    CHECK(untyped != typed);
    CHECK(rummage::fs::archive_tree(dir.path / "basic.zip", mime.get()) == typed);
    CHECK(rummage::fs::archive_tree(dir.path / "basic.zip", nullptr) == untyped);
    // Typed first: asked without types, that one does (not listed again).
    rummage::fs::forget_archive_trees();
    const auto first = rummage::fs::archive_tree(dir.path / "basic.zip", mime.get());
    CHECK(rummage::fs::archive_tree(dir.path / "basic.zip", nullptr) == first);
    const std::vector<rummage::fs::Entry>* top = typed->folder("");
    REQUIRE(top != nullptr);
    const auto text = std::ranges::find(*top, "a.txt", &rummage::fs::Entry::name);
    REQUIRE(text != top->end());
    CHECK(mime->name(text->mime) == "text/plain");
}

TEST_CASE("archive folders: what an archive holds, for Quick Look and the info pane") {
    TempDir dir;
    fs::copy_file(kArchives / "basic.zip", dir.path / "basic.zip");
    rummage::fs::forget_archive_trees();
    const std::shared_ptr<const rummage::fs::MimeDatabase> mime = rummage::fs::MimeDatabase::load();
    const rummage::preview::Archive held =
        rummage::preview::read_archive(dir.path / "basic.zip", mime, 2);
    CHECK(held.format == Format::zip);
    CHECK(held.failure == rummage::archive::Failure::none);
    CHECK(held.totals.files == 5);
    CHECK(held.totals.folders == 1);
    CHECK(held.totals.bytes == 13 + 12 + 8 + 12 + 5120);
    CHECK(held.top_count == 3);
    // Folders first, then by name; only as many as asked.
    REQUIRE(held.top.size() == 2);
    CHECK(held.top[0].name == "docs");
    CHECK(held.top[1].name == "a.txt");
    CHECK(mime->name(held.top[1].mime) == "text/plain");

    // The info pane's summary: the counts, no entries.
    const rummage::core::CancelToken token;
    const rummage::preview::Summary summary = rummage::preview::summarize(
        dir.path / "basic.zip", "application/zip", rummage::preview::Kind::archive, token, mime);
    REQUIRE(summary.archive.has_value());
    CHECK(summary.archive.value_or(rummage::preview::Archive{}).totals.files == 5);
    CHECK(summary.archive.value_or(rummage::preview::Archive{}).top.empty());

    // Encrypted entries are counted, never opened.
    fs::copy_file(kArchives / "aes.zip", dir.path / "aes.zip");
    CHECK(rummage::preview::read_archive(dir.path / "aes.zip", mime, 0).totals.encrypted == 1);
    // Not an archive: said.
    dir.write("fake.zip", "not a zip at all");
    const rummage::preview::Archive fake =
        rummage::preview::read_archive(dir.path / "fake.zip", mime, 12);
    CHECK(fake.failure == rummage::archive::Failure::not_archive);
    CHECK(fake.top.empty());
}

TEST_CASE("archive folders: where a path goes into an archive") {
    TempDir dir;
    fs::copy_file(kArchives / "basic.zip", dir.path / "basic.zip");
    fs::create_directories(dir.path / "folder");
    const auto inside = rummage::fs::find_archive(dir.path / "basic.zip" / "docs" / "deep");
    REQUIRE(inside.has_value());
    CHECK(inside.value_or(rummage::fs::ArchiveLocation{}).archive == dir.path / "basic.zip");
    CHECK(inside.value_or(rummage::fs::ArchiveLocation{}).inner == "docs/deep");
    const auto top = rummage::fs::find_archive(dir.path / "basic.zip");
    REQUIRE(top.has_value());
    CHECK(top.value_or(rummage::fs::ArchiveLocation{}).inner.empty());
    CHECK_FALSE(rummage::fs::find_archive(dir.path / "folder").has_value());
    CHECK_FALSE(rummage::fs::find_archive(dir.path / "missing" / "x").has_value());
    CHECK_FALSE(rummage::fs::find_archive(dir.path / "basic.zip" / ".." / "x").has_value());
}

TEST_CASE("archive folders: the model shows a folder inside an archive, read from its tree") {
    Fixture f;
    f.open(f.dir.path / "basic.zip");
    CHECK_FALSE(f.model.error());
    CHECK(f.model.is_archive());
    CHECK(f.model.archive_format() == Format::zip);
    CHECK(f.names() == std::vector<std::string>{"a.txt", "città.txt", "docs/"});
    const rummage::archive::Entry* a = f.model.archive_entry("a.txt");
    REQUIRE(a != nullptr);
    CHECK(a->size == 13);
    // Types from the names only: nothing inside the archive is on disk to be looked at.
    const std::shared_ptr<const rummage::fs::MimeDatabase> mime = rummage::fs::MimeDatabase::load();
    const auto& entries = f.model.listing().entries();
    const auto text = std::ranges::find(entries, "a.txt", &rummage::fs::Entry::name);
    REQUIRE(text != entries.end());
    CHECK(mime->name(text->mime) == "text/plain");

    f.open(f.dir.path / "basic.zip" / "docs");
    CHECK(f.model.is_archive());
    CHECK(f.model.archive_location().inner == "docs");
    CHECK(f.names() ==
          std::vector<std::string>{"b.txt", std::string(120, 'n') + ".txt", "repeat.txt"});
    REQUIRE(f.model.archive_entry("b.txt") != nullptr);

    // Back to a real folder: an archive no more.
    f.open(f.dir.path);
    CHECK_FALSE(f.model.is_archive());
}

TEST_CASE("archive folders: what is not an archive, or not in it, says so") {
    Fixture f;
    f.open(f.dir.path / "notes.txt");
    CHECK(f.model.error());
    CHECK_FALSE(f.model.is_archive());
    CHECK(f.model.problem() == "not an archive rummage reads");
    CHECK(f.model.listing().entries().empty());

    f.open(f.dir.path / "basic.zip" / "nowhere");
    CHECK(f.model.error());
    CHECK(f.model.problem() == "no such folder in the archive");
}

TEST_CASE("archive folders: Enter goes into an archive, where nothing is changed") {
    TempDir dir;
    fs::copy_file(kArchives / "basic.zip", dir.path / "basic.zip");
    rummage::fs::forget_archive_trees();
    EventLoop loop;
    WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    rummage::views::Browser browser{loop,    pool,
                                    watcher, rummage::fs::MimeDatabase::load(),
                                    [] {},   [](rummage::ui::CursorShape /*shape*/) {}};
    browser.set_home_trash(dir.path / "Trash");
    std::vector<std::string> opened;
    std::optional<fs::path> opened_in;
    browser.set_open_handler([&opened, &opened_in](const fs::path& directory,
                                                   const std::vector<rummage::fs::Entry>& files) {
        opened_in = directory;
        for (const rummage::fs::Entry& file : files) {
            opened.push_back(file.name);
        }
    });
    const auto settle = [&] { return run_until(loop, [&] { return !browser.loading(); }); };
    const auto press = [&](xkb_keysym_t keysym, rummage::ui::Modifiers modifiers = {}) {
        rummage::ui::KeyEvent event;
        event.keysym = keysym;
        event.modifiers = modifiers;
        browser.key(event, rummage::views::Browser::Clock::now());
        REQUIRE(settle());
    };
    const auto shown = [&] { return browser.columns().focused_folder()->path(); };

    browser.go_to(dir.path, "basic.zip");
    REQUIRE(settle());
    press(XKB_KEY_Return);
    CHECK(opened.empty());
    CHECK(shown() == dir.path / "basic.zip");
    REQUIRE(browser.columns().focused_folder()->is_archive());

    // Changing anything is refused, with a word why; the archive is untouched.
    const auto size_before = fs::file_size(dir.path / "basic.zip");
    const auto time_before = fs::last_write_time(dir.path / "basic.zip");
    press(XKB_KEY_F2);
    CHECK(browser.status_bar().message() ==
          "Archive contents are read-only; extract the archive first");
    press(XKB_KEY_Delete);
    CHECK(browser.status_bar().message() ==
          "Archive contents are read-only; extract the archive first");
    press(XKB_KEY_n, {.ctrl = true, .shift = true});
    CHECK(browser.status_bar().message() ==
          "Archive contents are read-only; extract the archive first");
    CHECK(fs::file_size(dir.path / "basic.zip") == size_before);
    CHECK(fs::last_write_time(dir.path / "basic.zip") == time_before);

    const auto cursor = [&] {
        const rummage::fs::Entry* entry = browser.columns().focused_folder()->cursor_entry();
        return entry != nullptr ? entry->name : std::string{};
    };
    for (int i = 0; i < 5 && cursor() != "a.txt"; ++i) {
        press(XKB_KEY_Down);
    }
    REQUIRE(cursor() == "a.txt");
    // Ctrl+C takes it to be extracted; Ctrl+X still may not.
    press(XKB_KEY_c, {.ctrl = true});
    CHECK(browser.status_bar().message() == "Ctrl+V extracts the selected archive contents");
    press(XKB_KEY_x, {.ctrl = true});
    CHECK(browser.status_bar().message() ==
          "Archive contents are read-only; extract the archive first");

    // Enter: extracted into a private folder first, then handed to another program from there.
    press(XKB_KEY_Return);
    REQUIRE(run_until(loop, [&] { return !opened.empty(); }));
    CHECK(opened == std::vector<std::string>{"a.txt"});
    REQUIRE(opened_in.has_value());
    const fs::path extracted = opened_in.value_or(fs::path{}) / "a.txt";
    CHECK(extracted.parent_path().parent_path() == rummage::fs::opened_files_folder());
    std::ifstream copy{extracted};
    std::string line;
    std::getline(copy, line);
    CHECK(line == "hello from a");
    CHECK((fs::status(extracted.parent_path()).permissions() & fs::perms::others_read) ==
          fs::perms::none);

    // Space shows it, read by the engine into memory: its text, as on disk.
    press(XKB_KEY_space);
    REQUIRE(browser.quick_look().is_open());
    REQUIRE(run_until(loop, [&] { return !browser.quick_look().loading(); }));
    const rummage::preview::Content* content = browser.quick_look().content();
    REQUIRE(content != nullptr);
    const auto* text = std::get_if<rummage::preview::Text>(content);
    REQUIRE(text != nullptr);
    REQUIRE(text->lines.size() == 1);
    CHECK(text->lines[0] == "hello from a");
}

TEST_CASE("archive folders: Quick Look shows what an archive holds, from outside") {
    TempDir dir;
    fs::copy_file(kArchives / "basic.zip", dir.path / "basic.zip");
    rummage::fs::forget_archive_trees();
    EventLoop loop;
    WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    rummage::views::Browser browser{loop,    pool,
                                    watcher, rummage::fs::MimeDatabase::load(),
                                    [] {},   [](rummage::ui::CursorShape /*shape*/) {}};
    browser.set_home_trash(dir.path / "Trash");
    browser.go_to(dir.path, "basic.zip");
    REQUIRE(run_until(loop, [&] { return !browser.loading(); }));
    rummage::ui::KeyEvent space;
    space.keysym = XKB_KEY_space;
    browser.key(space, rummage::views::Browser::Clock::now());
    REQUIRE(browser.quick_look().is_open());
    REQUIRE(run_until(loop, [&] { return !browser.quick_look().loading(); }));
    const rummage::preview::Content* content = browser.quick_look().content();
    REQUIRE(content != nullptr);
    const auto* held = std::get_if<rummage::preview::Archive>(content);
    REQUIRE(held != nullptr);
    CHECK(held->totals.files == 5);
    CHECK(held->top_count == 3);
    REQUIRE(held->top.size() == 3);
    CHECK(held->top[0].name == "docs");

    // Inside it, the status bar retains the archive context for every selection state.
    browser.go_to(dir.path / "basic.zip", "a.txt");
    REQUIRE(run_until(loop, [&] { return !browser.loading(); }));
    const auto folder = browser.columns().focused_folder();
    REQUIRE(folder);
    REQUIRE(folder->is_archive());
    REQUIRE(folder->single_selection());
    CHECK(folder->single_selection()->name == "a.txt");
    CHECK(rummage::views::status_of(*folder) == "zip archive  ·  read-only  ·  a.txt");

    folder->set_cursor(std::nullopt);
    CHECK(rummage::views::status_of(*folder) == "zip archive  ·  read-only  ·  3 items");

    folder->select_all();
    CHECK(rummage::views::status_of(*folder) == "zip archive  ·  read-only  ·  3 of 3 selected");
}

TEST_CASE("archive folders: Quick Look shows a picture from inside an archive") {
    TempDir dir;
    fs::copy_file(kArchives / "pictures.zip", dir.path / "pictures.zip");
    rummage::fs::forget_archive_trees();
    EventLoop loop;
    WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    rummage::views::Browser browser{loop,    pool,
                                    watcher, rummage::fs::MimeDatabase::load(),
                                    [] {},   [](rummage::ui::CursorShape /*shape*/) {}};
    browser.set_home_trash(dir.path / "Trash");
    const auto settle = [&] { return run_until(loop, [&] { return !browser.loading(); }); };
    browser.go_to(dir.path / "pictures.zip", "blocks.png");
    REQUIRE(settle());
    rummage::ui::KeyEvent space;
    space.keysym = XKB_KEY_space;
    browser.key(space, rummage::views::Browser::Clock::now());
    REQUIRE(browser.quick_look().is_open());
    REQUIRE(run_until(loop, [&] { return !browser.quick_look().loading(); }));
    const rummage::preview::Content* content = browser.quick_look().content();
    REQUIRE(content != nullptr);
    const auto* picture = std::get_if<rummage::picture::Loaded>(content);
    REQUIRE(picture != nullptr);
    CHECK(picture->failure == rummage::picture::Failure::none);
    CHECK(picture->full_width == 48);
    CHECK(picture->full_height == 32);
}

namespace {

// A ustar header and data, enough for the model's tests.
std::string tar_member(std::string_view name, std::string_view data, bool broken = false) {
    std::string block(512, '\0');
    std::ranges::copy(name, block.begin());
    const auto octal = [&block](std::size_t at, std::size_t width, std::uint64_t value) {
        std::string text(width - 1, '0');
        for (std::size_t i = width - 1; i-- > 0 && value > 0; value >>= 3U) {
            text[i] = static_cast<char>('0' + (value & 7U));
        }
        std::ranges::copy(text, block.begin() + static_cast<std::ptrdiff_t>(at));
    };
    octal(100, 8, 0644);
    octal(124, 12, data.size());
    octal(136, 12, 1794744000);
    block[156] = '0';
    std::ranges::copy(std::string_view{"ustar\0"
                                       "00",
                                       8},
                      block.begin() + 257);
    std::ranges::fill(block.begin() + 148, block.begin() + 156, ' ');
    unsigned sum = broken ? 1U : 0U;
    for (const char c : block) {
        sum += static_cast<unsigned char>(c);
    }
    octal(148, 7, sum);
    std::string out = block + std::string{data};
    out.append((512 - (data.size() % 512)) % 512, '\0');
    return out;
}

}  // namespace

TEST_CASE("archive folders: media retains its extracted still preview without playback") {
    namespace picture = rummage::picture;
    namespace engines = rummage::engines;

    struct Restore {
        std::shared_ptr<const engines::Registry> registry{engines::installed()};

        ~Restore() {
            engines::set_installed(registry);
            picture::set_loader([](const picture::LoadRequest& request) {
                return rummage::formats::image::load(request);
            });
        }
    } restore;

    auto registry = std::make_shared<engines::Registry>(*restore.registry);
    REQUIRE(registry->add({.manifest = {.name = "media",
                                        .program = "rummage-media",
                                        .families = {{.name = "video",
                                                      .shows = engines::Shows::picture,
                                                      .mime = {"video/mp4"}}}},
                           .program = "rummage-media",
                           .manifest_path = {}}));
    engines::set_installed(registry);
    std::ifstream fixture{fs::path{RUMMAGE_TEST_DATA} / "media" / "metadata.mp4", std::ios::binary};
    const std::string movie{std::istreambuf_iterator<char>{fixture}, {}};
    REQUIRE_FALSE(movie.empty());
    // Exercise actual archive extraction and provider dispatch without requiring FFmpeg in
    // the UI test binary. Only a descriptor containing the original movie yields a frame.
    picture::set_loader([movie](const picture::LoadRequest& request) {
        std::ifstream input{request.path, std::ios::binary};
        const std::string extracted{std::istreambuf_iterator<char>{input}, {}};
        picture::Loaded result;
        if (request.mime != "video/mp4" || !request.path.string().starts_with("/proc/self/fd/") ||
            extracted != movie) {
            result.failure = picture::Failure::unreadable;
            return result;
        }
        result.image = {
            .width = 64, .height = 32, .rgba = std::vector<std::uint8_t>(64 * 32 * 4, 255)};
        result.full_width = 64;
        result.full_height = 32;
        return result;
    });
    TempDir dir;
    dir.write("movies.tar", tar_member("clip.mp4", movie) + std::string(1024, '\0'));
    rummage::fs::forget_archive_trees();
    EventLoop loop;
    WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    const auto mime = rummage::fs::MimeDatabase::load();
    auto folder = std::make_shared<rummage::views::Folder>(loop, pool, watcher, mime,
                                                           dir.path / "movies.tar", [] {});
    REQUIRE(run_until(loop, [&] { return folder->entries().size() == 1; }));
    REQUIRE(folder->is_archive());
    folder->select_name("clip.mp4");
    REQUIRE(mime->name(folder->mime_of(*folder->cursor_entry())) == "video/mp4");
    rummage::views::QuickLook look{pool, mime, {}, [] {}};
    look.open(folder, rummage::views::QuickLook::Clock::now());
    CHECK(look.playback() == nullptr);
    REQUIRE(run_until(loop, [&] { return !look.loading(); }));
    REQUIRE(look.content());
    const auto* still = std::get_if<picture::Loaded>(look.content());
    REQUIRE(still);
    CHECK(still->failure == picture::Failure::none);
    CHECK(still->image.width == 64);
    CHECK(still->image.height == 32);
    look.follow(folder);
    CHECK(look.playback() == nullptr);
}

TEST_CASE("archive folders: an archive shown in part says why") {
    Fixture f;
    f.dir.write("evil.tar", tar_member("good.txt", "fine") + tar_member("../evil.txt", "no") +
                                tar_member("../../worse.txt", "no") + std::string(1024, '\0'));
    f.open(f.dir.path / "evil.tar");
    CHECK_FALSE(f.model.error());
    CHECK(f.names() == std::vector<std::string>{"good.txt"});
    CHECK(f.model.problem() == "2 entries with unsafe names left out");

    f.dir.write("broken.tar",
                tar_member("first.txt", "one") + tar_member("second.txt", "two", true));
    f.open(f.dir.path / "broken.tar");
    CHECK_FALSE(f.model.error());
    CHECK(f.names() == std::vector<std::string>{"first.txt"});
    CHECK(f.model.problem() == "damaged: a header is broken");
}

TEST_CASE("archive folders: paths and synthesized tree growth stay bounded") {
    using rummage::fs::clean_inner_path;
    std::string deep;
    for (int i = 0; i < 65; ++i)
        deep += "a/";
    CHECK_FALSE(clean_inner_path(deep));
    CHECK_FALSE(clean_inner_path(std::string(1025, 'a')));
    Listing listing;
    listing.format = Format::zip;
    for (int i = 0; i < 25001; ++i)
        listing.entries.push_back(item(std::to_string(i) + "/a/b/c/file.txt"));
    ArchiveTree tree{std::move(listing), "", nullptr};
    CHECK(tree.failure() == rummage::archive::Failure::too_large);
    REQUIRE(tree.folder("") != nullptr);
    CHECK(tree.folder("")->empty());
}

TEST_CASE("archive folders: cached identity detects replacement and in-place edits") {
    TempDir dir;
    const auto path = dir.path / "basic.zip";
    fs::copy_file(kArchives / "basic.zip", path);
    rummage::fs::forget_archive_trees();
    const auto tree = rummage::fs::archive_tree(path, nullptr);
    REQUIRE(tree->unchanged(path));
    const auto original_time = fs::last_write_time(path);
    fs::rename(path, dir.path / "old.zip");
    fs::copy_file(dir.path / "old.zip", path);
    fs::last_write_time(path, original_time);
    CHECK_FALSE(tree->unchanged(path));
    const auto replaced = rummage::fs::archive_tree(path, nullptr);
    CHECK(replaced != tree);
    REQUIRE(replaced->unchanged(path));
    {
        std::ofstream out{path, std::ios::binary | std::ios::app};
        out << 'x';
    }
    CHECK_FALSE(replaced->unchanged(path));
}
