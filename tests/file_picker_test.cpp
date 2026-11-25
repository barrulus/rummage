#include "rummage/core/file_picker.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sys/mman.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "rummage/ui/offscreen.hpp"
#include "rummage/views/file_picker.hpp"
#include "temp_dir.hpp"

namespace {
namespace core = rummage::core;
namespace fs = std::filesystem;
namespace views = rummage::views;
using rummage::test::TempDir;
using Mode = core::PickerRequest::Mode;
using namespace std::chrono_literals;

struct MemoryFile {
    int fd{memfd_create("picker-test", MFD_CLOEXEC)};

    ~MemoryFile() { close(fd); }
};

struct Fixture {
    TempDir temp;
    core::EventLoop loop;
    core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    std::shared_ptr<const rummage::fs::MimeDatabase> mime = rummage::fs::MimeDatabase::load();
    views::Browser browser{loop, pool, watcher, mime, [] {}, [](auto) {}};
    std::optional<core::PickerResult> result;

    Fixture() {
        fs::create_directories(temp.path / "folder");
        std::ofstream{temp.path / "one.txt"} << "one";
        std::ofstream{temp.path / "two.png"} << "two";
        browser.set_home_trash(temp.path / "Trash");
        browser.open(temp.path);
    }

    bool settle(std::function<bool()> ready = {}) {
        bool done = false;
        const auto poll = loop.add_timer(1ms, 1ms, [&] {
            if (!browser.loading() && (!ready || ready())) {
                done = true;
                loop.quit();
            }
        });
        const auto stop = loop.add_timer(5s, 0ms, [&] { loop.quit(1); });
        loop.run();
        loop.remove(poll);
        loop.remove(stop);
        return done;
    }

    auto picker(core::PickerRequest request) {
        return std::make_unique<views::FilePicker>(
            browser, std::move(request), mime, [] {},
            [this](core::PickerResult chosen) { result = std::move(chosen); });
    }
};
}  // namespace

TEST_CASE("picker: request and result round trip preserves arbitrary filename bytes") {
    MemoryFile file;
    REQUIRE(file.fd >= 0);
    core::PickerRequest request;
    request.mode = Mode::save_files;
    request.multiple = true;
    request.directory = true;
    request.title = "Save photos";
    request.accept_label = "Choose";
    request.folder = "/tmp/directory with spaces";
    request.name = "photo\nname";
    request.parent_window = "wayland:token";
    request.modal = false;
    request.files = {"a\nb", std::string{"raw-"} + static_cast<char>(0xff)};
    request.filters = {{"Text", {{0, "*.txt"}, {1, "text/plain"}}}, {"All", {{0, "*"}}}};
    request.filter = 1;
    request.choices = {{"encoding", "Encoding", {{"utf8", "UTF-8"}}, "utf8"},
                       {"readonly", "Read only", {}, "true"}};
    core::write_picker_request(file.fd, request);
    CHECK(core::read_picker_request(file.fd) == request);
    core::PickerResult result{0, {"/tmp/a\nb", "/tmp/quote'file"}, 1, {"utf8", "true"}};
    core::write_picker_result(file.fd, result);
    CHECK(core::read_picker_result(file.fd) == result);
    CHECK_THROWS(core::read_picker_request(file.fd));
    REQUIRE(ftruncate(file.fd, 3) == 0);
    CHECK_THROWS(core::read_picker_result(file.fd));
    request.name = std::string{"a\0b", 3};
    CHECK_THROWS(core::write_picker_request(file.fd, request));
}

TEST_CASE("picker: invalid protocol indexes and lengths are rejected") {
    MemoryFile file;
    core::PickerRequest request;
    request.filter = 99;
    core::write_picker_request(file.fd, request);
    CHECK_THROWS(core::read_picker_request(file.fd));
    CHECK(ftruncate(file.fd, 5 * 1024 * 1024) == 0);
    CHECK_THROWS(core::read_picker_request(file.fd));
    core::PickerResult result{8, {}, 0, {}};
    core::write_picker_result(file.fd, result);
    CHECK_THROWS(core::read_picker_result(file.fd));
}

TEST_CASE("picker: open files directories and multiple selection are validated") {
    TempDir temp;
    fs::create_directory(temp.path / "folder");
    std::ofstream{temp.path / "a\nb"} << "data";
    std::ofstream{temp.path / "second"} << "data";
    core::PickerRequest request;
    CHECK_FALSE(views::picker_selection(request, temp.path, {}, "").error.empty());
    CHECK_FALSE(views::picker_selection(request, temp.path, {"gone"}, "").error.empty());
    CHECK_FALSE(views::picker_selection(request, temp.path, {"folder"}, "").error.empty());
    CHECK(views::picker_selection(request, temp.path, {"a\nb"}, "").paths ==
          std::vector<std::string>{(temp.path / "a\nb").string()});
    CHECK_FALSE(views::picker_selection(request, temp.path, {"a\nb", "second"}, "").error.empty());
    request.multiple = true;
    CHECK(views::picker_selection(request, temp.path, {"a\nb", "second"}, "").paths.size() == 2);
    request.directory = true;
    CHECK(views::picker_selection(request, temp.path, {}, "").paths ==
          std::vector<std::string>{temp.path.string()});
    CHECK(views::picker_selection(request, temp.path, {"folder"}, "").error.empty());
    CHECK_FALSE(views::picker_selection(request, temp.path, {"second"}, "").error.empty());
}

TEST_CASE("picker: saving never writes and requires confirmation for existing files") {
    TempDir temp;
    core::PickerRequest request;
    request.mode = Mode::save;
    std::ofstream{temp.path / "existing"} << "original";
    fs::create_directory(temp.path / "folder");
    fs::create_symlink(temp.path / "missing", temp.path / "broken");
    const auto fresh = views::picker_selection(request, temp.path, {}, "new.txt");
    CHECK(fresh.error.empty());
    CHECK_FALSE(fresh.overwrite);
    CHECK_FALSE(fs::exists(temp.path / "new.txt"));
    const auto existing = views::picker_selection(request, temp.path, {}, "existing");
    CHECK(existing.error.empty());
    CHECK(existing.overwrite);
    CHECK(fs::file_size(temp.path / "existing") == 8);
    for (const auto* name : {"", ".", "..", "../escape", "folder", "broken"}) {
        CHECK_FALSE(views::picker_selection(request, temp.path, {}, name).error.empty());
    }
    request.mode = Mode::save_files;
    request.files = {"new.txt", "existing"};
    const auto batch = views::picker_selection(request, temp.path, {}, "");
    CHECK(batch.paths.size() == 2);
    CHECK(batch.overwrite);
    CHECK(views::picker_selection(request, temp.path, {"folder"}, "").paths.front() ==
          (temp.path / "folder/new.txt").string());
    request.files = {"../escape"};
    CHECK_FALSE(views::picker_selection(request, temp.path, {}, "").error.empty());
}

TEST_CASE("picker: filters preserve directories and return the chosen filter and choices") {
    Fixture f;
    core::PickerRequest request;
    request.filters = {{"Text", {{0, "*.txt"}}}, {"Images", {{1, "image/*"}}}};
    request.choices = {{"encoding", "Encoding", {{"utf8", "UTF-8"}}, ""}};
    auto picker = f.picker(request);
    REQUIRE(f.settle());
    auto folder = f.browser.picker_folder();
    REQUIRE(folder);
    CHECK(folder->entries().size() == 2);
    CHECK(folder->entries().back().name == "one.txt");
    picker->set_filter(1);
    CHECK(folder->entries().size() == 2);
    CHECK(folder->entries().back().name == "two.png");
    folder->select_name("two.png");
    picker->accept();
    REQUIRE(f.result);
    CHECK(f.result->response == 0);
    CHECK(f.result->filter == 1);
    CHECK(f.result->choices == std::vector<std::string>{"utf8"});
    CHECK(f.result->paths == std::vector<std::string>{(f.temp.path / "two.png").string()});
}

TEST_CASE("picker: filtering survives view changes and opening new folders") {
    Fixture f;
    std::ofstream{f.temp.path / "folder/inside.txt"} << "data";
    std::ofstream{f.temp.path / "folder/other.png"} << "data";
    core::PickerRequest request;
    request.filters = {{"Text", {{0, "*.txt"}}}};
    auto picker = f.picker(request);
    REQUIRE(f.settle());
    for (const auto mode : {views::Browser::Mode::grid, views::Browser::Mode::details,
                            views::Browser::Mode::columns}) {
        f.browser.set_mode(mode);
        f.browser.open(f.temp.path / "folder");
        REQUIRE(f.settle());
        const auto folder = f.browser.picker_folder();
        REQUIRE(folder);
        REQUIRE(folder->entries().size() == 1);
        CHECK(folder->entries()[0].name == "inside.txt");
    }
}

TEST_CASE(
    "picker: Enter selects a file, Escape cancels and directory mode chooses current folder") {
    SUBCASE("open") {
        Fixture f;
        auto picker = f.picker({});
        REQUIRE(f.settle());
        f.browser.picker_folder()->select_name("one.txt");
        picker->key({.keysym = XKB_KEY_Return, .text = {}});
        REQUIRE(f.result);
        CHECK(f.result->response == 0);
    }
    SUBCASE("cancel") {
        Fixture f;
        auto picker = f.picker({});
        REQUIRE(f.settle());
        picker->key({.keysym = XKB_KEY_Escape, .text = {}});
        REQUIRE(f.result);
        CHECK(f.result->response == 1);
        picker->accept();
        CHECK(f.result->response == 1);
    }
    SUBCASE("folder") {
        Fixture f;
        core::PickerRequest request;
        request.directory = true;
        auto picker = f.picker(request);
        REQUIRE(f.settle());
        CHECK(f.browser.picker_folder()->entries().size() == 1);
        picker->key({.keysym = XKB_KEY_Return, .text = {}, .modifiers = {.ctrl = true}});
        REQUIRE(f.result);
        CHECK(f.result->paths == std::vector<std::string>{f.temp.path.string()});
    }
}

TEST_CASE("picker: overwrite confirmation can be cancelled without closing picker") {
    Fixture f;
    core::PickerRequest request;
    request.mode = Mode::save;
    request.name = "one.txt";
    auto picker = f.picker(request);
    REQUIRE(f.settle());
    picker->accept();
    CHECK_FALSE(f.result);
    picker->key({.keysym = XKB_KEY_Escape, .text = {}});
    CHECK_FALSE(f.result);
    picker->accept();
    CHECK_FALSE(f.result);
    picker->key({.keysym = XKB_KEY_Return, .text = {}});
    REQUIRE(f.result);
    CHECK(f.result->response == 0);
    CHECK(fs::file_size(f.temp.path / "one.txt") == 3);
}

TEST_CASE("keyboard browsing: picker name input stays independent of prefix and Vim") {
    Fixture f;
    f.browser.set_keyboard_config({.vim = true, .wrap = true});
    core::PickerRequest request;
    request.mode = Mode::save;
    request.name = "Untitled.txt";
    auto picker = f.picker(request);
    REQUIRE(f.settle());
    REQUIRE(picker->editing_name());
    picker->key({.keysym = XKB_KEY_j, .text = "j"});
    CHECK(f.browser.type_jump_prefix().empty());
    picker->accept();
    REQUIRE(f.result);
    CHECK(f.result->paths == std::vector<std::string>{(f.temp.path / "j.txt").string()});
}

TEST_CASE("picker: choosing a filename in save mode replaces the initial suggestion") {
    Fixture f;
    core::PickerRequest request;
    request.mode = Mode::save;
    request.name = "Untitled.txt";
    auto picker = f.picker(request);
    REQUIRE(f.settle());
    // Shift+Tab returns from the name field to the browser.
    picker->key({.keysym = XKB_KEY_Tab, .text = {}, .modifiers = {.shift = true}});
    f.browser.picker_folder()->select_name("one.txt");
    picker->key({.keysym = XKB_KEY_Control_L, .text = {}});
    picker->key({.keysym = XKB_KEY_Return, .text = {}, .modifiers = {.ctrl = true}});
    CHECK_FALSE(f.result);  // the existing filename needs confirmation
    picker->key({.keysym = XKB_KEY_Return, .text = {}});
    REQUIRE(f.result);
    CHECK(f.result->paths == std::vector<std::string>{(f.temp.path / "one.txt").string()});
    CHECK_FALSE(fs::exists(f.temp.path / "Untitled.txt"));
}

TEST_CASE("picker: missing typed paths have useful errors and folder labels") {
    Fixture f;
    core::PickerRequest request;
    request.directory = true;
    auto picker = f.picker(request);
    REQUIRE(f.settle());
    CHECK(std::string{picker->name_label()} == "Folder");
    picker->set_name("not-created");
    picker->accept();
    CHECK(picker->error().find("does not exist") != std::string::npos);
    CHECK(picker->error().find("New Folder") != std::string::npos);
    CHECK_FALSE(f.result);
    CHECK_FALSE(fs::exists(f.temp.path / "not-created"));
    picker->set_name("one.txt");
    CHECK(picker->error().empty());
    picker->accept();
    CHECK(picker->error().find("not a folder") != std::string::npos);
    const auto removed = views::picker_selection(request, f.temp.path, {"removed"}, "");
    CHECK(removed.error.find("no longer exists") != std::string::npos);
    fs::create_symlink(f.temp.path / "missing", f.temp.path / "broken");
    CHECK(views::picker_selection(request, f.temp.path, {}, "broken")
              .error.find("broken symbolic link") != std::string::npos);
    request.directory = false;
    CHECK(views::picker_selection(request, f.temp.path, {}, "absent.txt")
              .error.find("existing file") != std::string::npos);
    CHECK(views::picker_selection(request, f.temp.path, {}, "folder").error.find("is a folder") !=
          std::string::npos);
    request.mode = Mode::save;
    CHECK(views::picker_selection(request, f.temp.path, {}, "").error ==
          "Enter a filename to save.");
}

TEST_CASE("browser: live name filtering composes with picker filters across all views") {
    Fixture f;
    core::PickerRequest request;
    request.filters = {{"Text", {{0, "*.txt"}}}, {"All", {{0, "*"}}}};
    auto picker = f.picker(request);
    REQUIRE(f.settle());
    for (const auto mode : {views::Browser::Mode::columns, views::Browser::Mode::grid,
                            views::Browser::Mode::details}) {
        f.browser.set_mode(mode);
        f.browser.open(f.temp.path);
        REQUIRE(f.settle());
        auto folder = f.browser.picker_folder();
        folder->select_name("one.txt");
        f.browser.set_name_filter("TWO");
        CHECK(folder->entries().empty());
        CHECK(folder->selected_names().empty());
        CHECK_FALSE(folder->cursor_entry());
        picker->set_filter(1);
        REQUIRE(folder->entries().size() == 1);
        CHECK(folder->entries()[0].name == "two.png");
        // Files arriving through the watcher obey the same filter.
        std::ofstream{f.temp.path / "two-new.txt"} << "new";
        REQUIRE(f.settle([&] { return folder->entries().size() == 2; }));
        CHECK(folder->entries().size() == 2);
        f.browser.set_name_filter("");
        CHECK(folder->entries().size() == 4);
        fs::remove(f.temp.path / "two-new.txt");
        REQUIRE(f.settle([&] { return folder->entries().size() == 3; }));
        picker->set_filter(0);
    }
}

TEST_CASE("picker: filter keyboard input and Escape do not accept or cancel the dialog") {
    Fixture f;
    auto picker = f.picker({});
    REQUIRE(f.settle());
    picker->key({.keysym = XKB_KEY_F, .text = {}, .modifiers = {.ctrl = true, .shift = true}});
    REQUIRE(f.browser.filtering());
    picker->key({.keysym = XKB_KEY_o, .text = "o"});
    picker->key({.keysym = XKB_KEY_n, .text = "n"});
    picker->key({.keysym = XKB_KEY_e, .text = "e"});
    CHECK(f.browser.name_filter() == "one");
    REQUIRE(f.browser.picker_folder()->entries().size() == 1);
    picker->key({.keysym = XKB_KEY_Return, .text = {}});
    CHECK_FALSE(f.browser.filtering());
    CHECK_FALSE(f.result);
    picker->key({.keysym = XKB_KEY_F, .text = {}, .modifiers = {.ctrl = true, .shift = true}});
    picker->key({.keysym = XKB_KEY_Escape, .text = {}});
    CHECK(f.browser.name_filter().empty());
    CHECK_FALSE(f.result);
    CHECK(f.browser.picker_folder()->entries().size() == 3);
}

TEST_CASE("picker: New Folder clears the name filter and starts renaming in all views") {
    for (const auto mode : {views::Browser::Mode::columns, views::Browser::Mode::grid,
                            views::Browser::Mode::details}) {
        Fixture f;
        core::PickerRequest request;
        request.directory = true;
        auto picker = f.picker(request);
        f.browser.set_mode(mode);
        REQUIRE(f.settle());
        picker->set_name("not-created");
        picker->accept();
        REQUIRE_FALSE(picker->error().empty());
        f.browser.set_name_filter("nothing-matches");
        f.browser.new_folder();
        REQUIRE(f.settle());
        CHECK(f.browser.name_filter().empty());
        CHECK(fs::is_directory(f.temp.path / "New Folder"));
        CHECK(f.browser.name_editor().active());
        picker->key({.keysym = XKB_KEY_Return, .text = {}});
        picker->accept();
        REQUIRE(f.result);
        CHECK(f.result->paths == std::vector<std::string>{(f.temp.path / "New Folder").string()});
    }
}

TEST_CASE("picker: save editing keeps the extension and dotfiles stay whole") {
    Fixture f;
    core::PickerRequest request;
    request.mode = Mode::save;
    request.name = "Untitled.txt";
    auto picker = f.picker(request);
    REQUIRE(f.settle());
    CHECK(picker->copy_name(false) == "Untitled");
    picker->key({.keysym = XKB_KEY_r, .text = "renamed"});
    picker->accept();
    REQUIRE(f.result);
    CHECK(f.result->paths == std::vector<std::string>{(f.temp.path / "renamed.txt").string()});
    CHECK_FALSE(fs::exists(f.temp.path / "renamed.txt"));
    picker->set_name(".bashrc");
    CHECK(picker->copy_name(false) == ".bashrc");
    picker->set_name("report.tar.gz");
    CHECK(picker->copy_name(false) == "report.tar");
}

TEST_CASE("picker: typing a folder navigates and preserves the proposed save name") {
    Fixture f;
    std::ofstream{f.temp.path / "folder/inside.txt"} << "data";
    core::PickerRequest request;
    request.mode = Mode::save;
    request.name = "Untitled.txt";
    auto picker = f.picker(request);
    REQUIRE(f.settle());
    picker->set_name("folder");
    picker->accept();
    CHECK_FALSE(f.result);
    REQUIRE(f.settle());
    CHECK(f.browser.picker_folder()->path() == f.temp.path / "folder");
    CHECK(picker->copy_name(false) == "Untitled");
    // A newly opened folder must not replace the proposal with its first entry.
    picker->key({.keysym = XKB_KEY_Tab, .text = {}, .modifiers = {.shift = true}});
    picker->key({.keysym = XKB_KEY_Control_L, .text = {}});
    picker->accept();
    REQUIRE(f.result);
    CHECK(f.result->paths ==
          std::vector<std::string>{(f.temp.path / "folder/Untitled.txt").string()});
}

TEST_CASE("picker: direct application choices remain keyboard accessible in both directions") {
    Fixture f;
    core::PickerRequest request;
    request.choices = {
        {"readonly", "Read only", {}, "false"},
        {"encoding", "Encoding", {{"utf8", "UTF-8"}, {"latin1", "Latin-1"}}, "utf8"}};
    auto picker = f.picker(request);
    REQUIRE(f.settle());
    f.browser.picker_folder()->select_name("one.txt");
    // Browser -> filename -> checkbox -> choice menu, skipping the absent filter.
    picker->key({.keysym = XKB_KEY_Tab, .text = {}});
    picker->key({.keysym = XKB_KEY_Tab, .text = {}});
    picker->key({.keysym = XKB_KEY_space, .text = " "});
    picker->key({.keysym = XKB_KEY_Tab, .text = {}});
    picker->key({.keysym = XKB_KEY_Return, .text = {}});
    picker->key({.keysym = XKB_KEY_Down, .text = {}});
    picker->key({.keysym = XKB_KEY_Return, .text = {}});
    picker->key({.keysym = XKB_KEY_ISO_Left_Tab, .text = {}});
    picker->key({.keysym = XKB_KEY_space, .text = " "});
    picker->key({.keysym = XKB_KEY_Return, .text = {}, .modifiers = {.ctrl = true}});
    REQUIRE(f.result);
    CHECK(f.result->choices == std::vector<std::string>{"false", "latin1"});
}

TEST_CASE("picker: globs ignore case and MIME wildcards include inherited media types") {
    TempDir temp;
    std::ofstream{temp.path / "globs2"} << "50:application/x-picker-photo:*.raw\n";
    std::ofstream{temp.path / "subclasses"} << "application/x-picker-photo image/x-picker-photo\n"
                                               "application/x-loop application/x-loop\n";
    const auto mime = rummage::fs::MimeDatabase::load({temp.path});
    rummage::fs::Entry entry;
    entry.name = "PHOTO.JPG";
    CHECK(views::picker_matches({"JPEG", {{0, "*.jpg"}}}, entry, *mime));
    entry.name = "unknown";
    CHECK(views::picker_matches({"All", {{1, "*/*"}}}, entry, *mime));
    CHECK(views::picker_matches({"All", {{1, "*"}}}, entry, *mime));
    entry.name = "photo.raw";
    CHECK(views::picker_matches({"Images", {{1, "image/*"}}}, entry, *mime));
    CHECK_FALSE(views::picker_matches({"Text", {{1, "text/*"}}}, entry, *mime));
    entry.mime = mime->find("application/x-loop");
    CHECK_FALSE(views::picker_matches({"Images", {{1, "image/*"}}}, entry, *mime));
}

TEST_CASE("picker: overflowing options scroll into view and receive pointer input") {
    rummage::ui::Offscreen gl{360, 320};
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    Fixture f;
    core::PickerRequest request;
    request.mode = Mode::save;
    request.name = "Untitled.txt";
    for (int i = 0; i < 8; ++i) {
        request.choices.push_back(
            {std::to_string(i), "An application checkbox " + std::to_string(i), {}, "false"});
    }
    auto picker = f.picker(request);
    REQUIRE(f.settle());
    const auto draw = [&] {
        renderer.begin_frame(360, 320, 1, {});
        picker->draw(renderer, {0, 0, 360, 320}, rummage::theme::Palette{}, icons);
        renderer.end_frame();
    };
    draw();
    for (int i = 0; i < 8; ++i) {
        picker->key({.keysym = XKB_KEY_Tab, .text = {}});
        draw();
    }
    // The last checkbox is revealed in the visible option row above the actions.
    CHECK(picker->cursor_at(30, 225) == rummage::ui::CursorShape::pointer);
    picker->pointer({.kind = rummage::ui::PointerEvent::Kind::press,
                     .x = 30,
                     .y = 225,
                     .button = rummage::ui::PointerButton::left});
    picker->pointer(
        {.kind = rummage::ui::PointerEvent::Kind::scroll, .x = 30, .y = 225, .scroll_y = -1000});
    draw();
    picker->pointer({.kind = rummage::ui::PointerEvent::Kind::press,
                     .x = 30,
                     .y = 225,
                     .button = rummage::ui::PointerButton::left});
    // The save action stays reachable even with many options in a small window.
    picker->pointer({.kind = rummage::ui::PointerEvent::Kind::press,
                     .x = 300,
                     .y = 265,
                     .button = rummage::ui::PointerButton::left});
    REQUIRE(f.result);
    CHECK(f.result->response == 0);
    CHECK(f.result->choices.front() == "true");
    CHECK(f.result->choices.back() == "true");
    CHECK(std::count(f.result->choices.begin(), f.result->choices.end(), "true") == 2);
}

TEST_CASE("picker: save filename supports double click and drag selection") {
    namespace ui = rummage::ui;
    using Kind = ui::PointerEvent::Kind;
    ui::Offscreen gl{800, 600};
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    ui::IconSet icons{fonts};
    Fixture f;
    core::PickerRequest request;
    request.mode = Mode::save;
    request.name = "sample.txt";
    auto picker = f.picker(request);
    REQUIRE(f.settle());
    renderer.begin_frame(800, 600, 1, {});
    picker->draw(renderer, {0, 0, 800, 600}, rummage::theme::Palette{}, icons);
    renderer.end_frame();
    const float start = 64 + ui::TextField::kPadding;
    const float end = start + fonts.layout("sample", 14, 1).width;
    REQUIRE(picker->cursor_at(start, 500) == ui::CursorShape::text);
    std::string expected;
    SUBCASE("double click selects the complete filename for replacement") {
        picker->pointer({.kind = Kind::press,
                         .x = end,
                         .y = 500,
                         .button = ui::PointerButton::left,
                         .click_count = 2});
        picker->pointer(
            {.kind = Kind::release, .x = end, .y = 500, .button = ui::PointerButton::left});
        CHECK(picker->copy_name(false) == "sample.txt");
        picker->paste("renamed.md");
        expected = "renamed.md";
    }
    SUBCASE("drag outside the footer selects the stem without changing the browser selection") {
        picker->pointer({.kind = Kind::press,
                         .x = end,
                         .y = 500,
                         .button = ui::PointerButton::left,
                         .click_count = 1});
        picker->pointer({.kind = Kind::motion, .x = start, .y = 100});
        CHECK(picker->copy_name(false) == "sample");
        picker->pointer(
            {.kind = Kind::release, .x = start, .y = 100, .button = ui::PointerButton::left});
        picker->pointer({.kind = Kind::motion, .x = end, .y = 500});
        CHECK(picker->copy_name(false) == "sample");
        picker->paste("renamed");
        expected = "renamed.txt";
    }
    REQUIRE(picker->editing_name());
    CHECK_FALSE(f.result);
    picker->accept();
    REQUIRE(f.result);
    CHECK(f.result->response == 0);
    CHECK(f.result->paths == std::vector<std::string>{(f.temp.path / expected).string()});
}

TEST_CASE(
    "picker: terminal action is keyboard accessible and returning preserves GUI selection rules") {
    Fixture f;
    core::PickerRequest request;
    request.filters = {{"Text", {{0, "*.txt"}}}};
    request.choices = {{"readonly", "Read only", {}, "true"}};
    auto picker = f.picker(request);
    fs::path launched;
    int cancelled = 0;
    picker->set_terminal_navigation([&](const auto& directory) { launched = directory; },
                                    [&] { ++cancelled; });
    REQUIRE(f.settle());
    // Browser -> name -> filter -> application choice -> terminal.
    for (int i = 0; i < 4; ++i)
        picker->key({.keysym = XKB_KEY_Tab, .text = {}});
    picker->key({.keysym = XKB_KEY_Return, .text = {}});
    CHECK(launched == f.temp.path);
    CHECK_FALSE(f.result);
    const auto target = f.temp.path / "folder";
    std::ofstream{target / "returned.txt"} << "text";
    std::ofstream{target / "hidden.png"} << "png";
    picker->return_from_terminal(target);
    REQUIRE(f.settle());
    CHECK_FALSE(f.result);
    CHECK(f.browser.picker_folder()->path() == target);
    CHECK(f.browser.picker_folder()->entries().size() == 1);
    picker->set_name("returned.txt");
    picker->accept();
    REQUIRE(f.result);
    CHECK(f.result->paths == std::vector<std::string>{(target / "returned.txt").string()});
    CHECK(f.result->filter == 0);
    CHECK(f.result->choices == std::vector<std::string>{"true"});
    CHECK(cancelled == 1);
    picker->return_from_terminal(f.temp.path);  // late callback after acceptance is ignored
    CHECK(f.browser.picker_folder()->path() == target);
}

TEST_CASE(
    "picker: terminal return preserves save name and directory mode and cancellation invalidates "
    "it") {
    Fixture f;
    core::PickerRequest request;
    SUBCASE("save") {
        request.mode = Mode::save;
        request.name = "saved.txt";
    }
    SUBCASE("directory") { request.directory = true; }
    auto picker = f.picker(request);
    int cancelled = 0;
    picker->set_terminal_navigation([](const auto&) {}, [&] { ++cancelled; });
    REQUIRE(f.settle());
    const auto target = f.temp.path / "folder";
    picker->return_from_terminal(target);
    REQUIRE(f.settle());
    CHECK_FALSE(f.result);
    picker->accept();
    REQUIRE(f.result);
    CHECK(f.result->paths ==
          std::vector<std::string>{(request.directory ? target : target / "saved.txt").string()});
    CHECK(cancelled == 1);
}

TEST_CASE("picker: closing or cancelling invalidates terminal navigation") {
    Fixture f;
    auto picker = f.picker({});
    int cancelled = 0;
    picker->set_terminal_navigation([](const auto&) {}, [&] { ++cancelled; });
    REQUIRE(f.settle());
    SUBCASE("cancel") {
        picker->cancel();
        CHECK(cancelled == 1);
        picker->return_from_terminal(f.temp.path / "folder");
        CHECK(f.browser.picker_folder()->path() == f.temp.path);
    }
    SUBCASE("destroy") {
        picker.reset();
        CHECK(cancelled == 1);
    }
}

TEST_CASE("picker: terminal button remains clickable at wide and narrow window sizes") {
    namespace ui = rummage::ui;
    Fixture f;
    core::PickerRequest request;
    request.choices = {{"option", "Option", {}, "false"}};
    auto picker = f.picker(request);
    int launched = 0;
    picker->set_terminal_navigation([&](const auto&) { ++launched; }, [] {});
    REQUIRE(f.settle());
    ui::Offscreen gl{800, 600};
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    ui::IconSet icons{fonts};
    for (float width : {800.0F, 360.0F}) {
        renderer.begin_frame(800, 600, 1, {});
        picker->draw(renderer, {0, 0, width, 600}, rummage::theme::Palette{}, icons);
        renderer.end_frame();
        // Queue keyboard focus on a choice, then a pointer activation before the next draw.
        for (int i = 0; i < 2; ++i)
            picker->key({.keysym = XKB_KEY_Tab, .text = {}});
        const float y = width == 800 ? 545.0F : 505.0F;
        CHECK(picker->cursor_at(30, y) == ui::CursorShape::pointer);
        picker->pointer({.kind = ui::PointerEvent::Kind::press,
                         .x = 30,
                         .y = y,
                         .button = ui::PointerButton::left});
        renderer.begin_frame(800, 600, 1, {});
        picker->draw(renderer, {0, 0, width, 600}, rummage::theme::Palette{}, icons);
        renderer.end_frame();
    }
    CHECK(launched == 2);
    CHECK_FALSE(f.result);
}

TEST_CASE("browser: filename filters support wildcards and hidden extensions in every view") {
    for (const auto mode : {views::Browser::Mode::columns, views::Browser::Mode::grid,
                            views::Browser::Mode::details}) {
        Fixture f;
        for (const auto name : {"icon.svg", "Logo.SVG", "icon.svg.bak", "svg-notes.txt",
                                "art[draft].svg", "star*.txt"})
            std::ofstream{f.temp.path / name} << "content";
        f.browser.set_mode(mode);
        f.browser.open(f.temp.path);
        REQUIRE(f.settle());
        const auto folder = f.browser.picker_folder();
        const auto names = [&] {
            std::vector<std::string> result;
            for (const auto& entry : folder->entries())
                result.push_back(entry.name);
            std::ranges::sort(result);
            return result;
        };
        for (const bool extensions : {true, false}) {
            CAPTURE(mode);
            CAPTURE(extensions);
            f.browser.set_show_extensions(extensions);
            f.browser.set_name_filter("*.svg");
            CHECK(names() == std::vector<std::string>{"Logo.SVG", "art[draft].svg", "icon.svg"});
            f.browser.set_name_filter("SVG");
            CHECK(names() == std::vector<std::string>{"Logo.SVG", "art[draft].svg", "icon.svg",
                                                      "icon.svg.bak", "svg-notes.txt"});
            f.browser.set_name_filter("icon.?vg");
            CHECK(names() == std::vector<std::string>{"icon.svg"});
            f.browser.set_name_filter("[il]*.svg");
            CHECK(names() == std::vector<std::string>{"Logo.SVG", "icon.svg"});
            f.browser.set_name_filter("*.svg*");
            CHECK(names() == std::vector<std::string>{"Logo.SVG", "art[draft].svg", "icon.svg",
                                                      "icon.svg.bak"});
            f.browser.set_name_filter(R"(*\[draft\]*)");
            CHECK(names() == std::vector<std::string>{"art[draft].svg"});
            f.browser.set_name_filter(R"(star\*.txt)");
            CHECK(names() == std::vector<std::string>{"star*.txt"});
            f.browser.set_name_filter(".txt");
            CHECK(names() == std::vector<std::string>{"one.txt", "star*.txt", "svg-notes.txt"});
            f.browser.set_name_filter("*");
            CHECK(names().size() == 9);
            f.browser.set_name_filter("");
            CHECK(names().size() == 9);
        }
    }
}

TEST_CASE("browser: wildcard filters compose with picker types and watch new files") {
    Fixture f;
    core::PickerRequest request;
    request.filters = {{"Text", {{0, "*.txt"}}}, {"All", {{0, "*"}}}};
    auto picker = f.picker(request);
    REQUIRE(f.settle());
    f.browser.set_show_extensions(false);
    f.browser.set_name_filter("*.svg");
    const auto folder = f.browser.picker_folder();
    CHECK(folder->entries().empty());
    std::ofstream{f.temp.path / "new.svg"} << "<svg/>";
    picker->set_filter(1);
    REQUIRE(f.settle([&] { return folder->entries().size() == 1; }));
    CHECK(folder->entries().front().name == "new.svg");
    std::ofstream{f.temp.path / "another.SVG"} << "<svg/>";
    REQUIRE(f.settle([&] { return folder->entries().size() == 2; }));
    picker->set_filter(0);
    CHECK(folder->entries().empty());
}

TEST_CASE("picker: a browser dialog owns clipboard keys while the save name has focus") {
    Fixture f;
    core::PickerRequest request;
    request.mode = Mode::save;
    request.name = "saved.txt";
    auto picker = f.picker(request);
    REQUIRE(f.settle());
    REQUIRE(picker->editing_name());
    f.browser.location_dialog().open("/tmp", [](const auto&, bool) { return std::string{}; });
    CHECK_FALSE(picker->editing_name());
    picker->key({.keysym = XKB_KEY_x, .text = {}, .modifiers = {.ctrl = true}});
    CHECK(f.browser.location_dialog().text().empty());
    CHECK(picker->copy_name(false) == "saved");
    f.browser.location_dialog().close();
    CHECK(picker->editing_name());
}

TEST_CASE("picker: pattern selection works for Open and is unavailable for both Save modes") {
    Fixture f;
    f.browser.set_picker_mode(true);
    f.browser.set_panes_enabled(false);
    core::PickerRequest request;
    request.mode = Mode::open;
    request.multiple = true;
    auto open = f.picker(request);
    REQUIRE(f.settle());
    f.browser.picker_folder()->select_name("one.txt");
    const auto commands = f.browser.palette_items();
    CHECK(std::ranges::none_of(commands,
                               [](const auto& item) { return item.id == "command:create_link"; }));
    CHECK(std::ranges::any_of(commands,
                              [](const auto& item) { return item.id == "command:compress_zip"; }));
    f.browser.create_links();
    REQUIRE(f.settle());
    CHECK_FALSE(fs::exists(f.temp.path / "Link to one.txt"));
    REQUIRE(open->key({.keysym = XKB_KEY_s, .text = {}, .modifiers = {.ctrl = true}}));
    REQUIRE(f.browser.command_palette().is_open());
    f.browser.command_palette().set_query("*.txt");
    open->key({.keysym = XKB_KEY_Return, .text = {}});
    CHECK(f.browser.picker_folder()->selected_names() == std::vector<std::string>{"one.txt"});
    open.reset();
    for (const auto mode : {Mode::save, Mode::save_files}) {
        request.mode = mode;
        request.name = "new.txt";
        request.files = {"new.txt"};
        auto save = f.picker(request);
        const auto selection = f.browser.picker_folder()->selected_names();
        save->key({.keysym = XKB_KEY_s, .text = {}, .modifiers = {.ctrl = true}});
        CHECK_FALSE(f.browser.command_palette().is_open());
        CHECK(f.browser.picker_folder()->selected_names() == selection);
        const auto items = f.browser.palette_items();
        CHECK(std::ranges::none_of(
            items, [](const auto& item) { return item.id == "command:select_pattern"; }));
        CHECK_FALSE(f.result);
    }
}

TEST_CASE("picker: keyboard path completion and cancellation do not accept or close the picker") {
    for (const auto mode : {Mode::open, Mode::save}) {
        Fixture f;
        f.browser.set_picker_mode(true);
        f.browser.set_panes_enabled(false);
        core::PickerRequest request;
        request.mode = mode;
        request.directory = mode == Mode::open;
        request.name = "saved.txt";
        auto picker = f.picker(request);
        REQUIRE(f.settle());
        CHECK(picker->key({.keysym = XKB_KEY_d, .text = {}, .modifiers = {.alt = true}}));
        REQUIRE(f.browser.breadcrumb().editing());
        CHECK_FALSE(picker->editing_name());
        f.browser.breadcrumb().paste((f.temp.path / "fold").string());
        CHECK(picker->key({.keysym = XKB_KEY_Tab, .text = {}}));
        REQUIRE(f.settle([&] { return !f.browser.breadcrumb().completing(); }));
        CHECK(f.browser.breadcrumb().text() == (f.temp.path / "folder").string() + "/");
        CHECK(picker->key({.keysym = XKB_KEY_Escape, .text = {}}));
        CHECK_FALSE(f.result);
        CHECK_FALSE(f.browser.breadcrumb().editing());
        CHECK(picker->key({.keysym = XKB_KEY_d, .text = {}, .modifiers = {.alt = true}}));
        f.browser.breadcrumb().paste((f.temp.path / "folder").string());
        CHECK(picker->key({.keysym = XKB_KEY_Return, .text = {}, .modifiers = {.ctrl = true}}));
        REQUIRE(f.settle());
        CHECK_FALSE(f.result);
        CHECK(f.browser.picker_folder()->path() == f.temp.path / "folder");
        CHECK(picker->key({.keysym = XKB_KEY_Return, .text = {}, .modifiers = {.ctrl = true}}));
        REQUIRE(f.result);
        CHECK(f.result->response == 0);
        CHECK(f.browser.tab_count() == 1);
    }
}
