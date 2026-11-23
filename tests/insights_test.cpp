#include "rummage/fs/insights.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <limits>
#include <numeric>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/theme/desktop.hpp"
#include "rummage/ui/offscreen.hpp"
#include "rummage/views/browser.hpp"
#include "rummage/views/insights_view.hpp"
#include "rummage/views/shortcuts.hpp"
#include "temp_dir.hpp"

namespace {
namespace fs = std::filesystem;
using namespace std::chrono_literals;
using rummage::core::CancelToken;
using rummage::fs::scan_insights;
using rummage::test::TempDir;
using rummage::views::InsightsView;

bool until(rummage::core::EventLoop& loop, const std::function<bool()>& done) {
    bool ok = false;
    const auto poll = loop.add_timer(1ms, 1ms, [&] {
        if (done()) {
            ok = true;
            loop.quit(0);
        }
    });
    const auto timeout = loop.add_timer(5s, 0ms, [&] { loop.quit(1); });
    loop.run();
    loop.remove(poll);
    loop.remove(timeout);
    return ok;
}

std::size_t row(const InsightsView& dialog, std::string_view name) {
    const auto found = std::ranges::find(dialog.rows(), name, &InsightsView::Row::label);
    REQUIRE(found != dialog.rows().end());
    return static_cast<std::size_t>(found - dialog.rows().begin());
}

void capture(const std::string& name, int width, int height) {
    const char* directory = std::getenv("RUMMAGE_INSIGHTS_FRAMES");
    if (!directory)
        return;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width * height * 4));
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    std::ofstream out{fs::path{directory} / (name + ".ppm"), std::ios::binary};
    out << "P6\n" << width << " " << height << "\n255\n";
    for (int y = height - 1; y >= 0; --y)
        for (int x = 0; x < width; ++x)
            out.write(reinterpret_cast<const char*>(pixels.data() + (y * width + x) * 4), 3);
    REQUIRE(out.good());
}
}  // namespace

TEST_CASE("insights: extensions are exact, case folded, and content independent") {
    using rummage::fs::insight_extension;
    CHECK(insight_extension("report.PDF") == ".pdf");
    CHECK(insight_extension("report.pdf.txt") == ".txt");
    CHECK(insight_extension("data.TAR.GZ") == ".tar.gz");
    CHECK(insight_extension("README").empty());
    CHECK(insight_extension(".bashrc").empty());
    CHECK(insight_extension(".hidden.MD") == ".md");
    CHECK(insight_extension("trailing.").empty());
    CHECK(insight_extension("letter.doc") != insight_extension("letter.docx"));
}

TEST_CASE("insights: bounded tree stores additive counts, paths, hidden scope and links") {
    const TempDir dir;
    fs::create_directories(dir.path / "sub/deep");
    fs::create_directories(dir.path / ".private");
    dir.write("a.PDF", "1234");
    dir.write("sub/b.md", "123");
    dir.write("sub/deep/c.doc", "12");
    dir.write(".private/secret.pdf", "123456789");
    dir.write(".hidden", "x");
    fs::create_hard_link(dir.path / "a.PDF", dir.path / "sub/linked.pdf");
    fs::create_directory_symlink(dir.path, dir.path / "sub/cycle");
    fs::create_symlink("missing", dir.path / "broken");
    REQUIRE(mkfifo((dir.path / "pipe").c_str(), 0600) == 0);
    const auto snapshot = scan_insights(dir.path, {}, CancelToken{});
    REQUIRE(snapshot.complete());
    CHECK(snapshot.nodes[0].files == 4);
    CHECK(snapshot.nodes[0].bytes == 13);  // Per name: hard-linked files are both represented.
    CHECK(snapshot.links == 2);
    CHECK(snapshot.nodes[0].end == snapshot.nodes.size());
    std::uint64_t sum = 0;
    for (std::size_t i = 1; i < snapshot.nodes.size(); ++i) {
        const auto& node = snapshot.nodes[i];
        CHECK(node.end > i);
        CHECK(node.end <= snapshot.nodes.size());
        CHECK(node.parent < i);
        if (node.parent == 0)
            sum += node.bytes;
        if (node.name == "c.doc")
            CHECK(snapshot.relative_path(i) == "sub/deep/c.doc");
    }
    CHECK(sum == snapshot.nodes[0].bytes);
    const auto direct = scan_insights(dir.path, {.recursive = false}, CancelToken{});
    CHECK(direct.complete());
    CHECK(direct.nodes[0].files == 1);
    CHECK(direct.nodes[0].bytes == 4);
    const auto hidden = scan_insights(dir.path, {.hidden = true}, CancelToken{});
    CHECK(hidden.nodes[0].files == 6);
    CHECK(hidden.nodes[0].bytes == 23);
}

TEST_CASE("insights: bounded scans discover sibling folders before a deep tree") {
    const TempDir dir;
    fs::create_directories(dir.path / "dev/deep");
    fs::create_directories(dir.path / "Documents");
    fs::create_directories(dir.path / "Pictures");
    dir.write("Documents/report.pdf", "1234");
    dir.write("Pictures/photo.jpg", "123456");
    for (int i = 0; i < 30; ++i)
        dir.write("dev/deep/" + std::to_string(i) + ".md", "x");
    const auto data = scan_insights(dir.path, {.max_entries = 12}, CancelToken{});
    CHECK(data.limited);
    std::size_t folders = 0;
    for (std::size_t i = 1; i < data.nodes.size(); ++i) {
        const auto& node = data.nodes[i];
        if (node.parent == 0) {
            ++folders;
            if (node.name == "dev")
                CHECK(node.partial);
            else {
                CHECK_FALSE(node.partial);
                CHECK(node.files == 1);
                CHECK(node.bytes == (node.name == "Documents" ? 4 : 6));
            }
        }
        // Reordering preserves exactly the descendants, not the next sibling.
        for (std::size_t j = i + 1; j < data.nodes.size(); ++j) {
            auto ancestor = data.nodes[j].parent;
            while (ancestor > i)
                ancestor = data.nodes[ancestor].parent;
            CHECK((ancestor == i) == (j < node.end));
        }
    }
    CHECK(folders == 3);
    CHECK(data.nodes[0].files == 8);
    CHECK(data.nodes[0].bytes == 16);
    const auto pending = scan_insights(dir.path, {.max_entries = 3}, CancelToken{});
    CHECK(pending.limited);
    REQUIRE(pending.nodes.size() == 4);
    for (std::size_t i = 1; i < pending.nodes.size(); ++i) {
        CHECK(pending.nodes[i].parent == 0);
        CHECK(pending.nodes[i].partial);
        CHECK(pending.nodes[i].files == 0);
    }
    // Drilling into a previously unmeasured folder gives it a fresh budget.
    const auto nested = scan_insights(dir.path / "Documents", {}, CancelToken{});
    CHECK(nested.complete());
    CHECK(nested.nodes[0].bytes == 4);
}

TEST_CASE("insights: incomplete scans never present an exact empty result") {
    const TempDir dir;
    fs::create_directories(dir.path / "sub/deep");
    dir.write("sub/deep/a.pdf", "123");
    rummage::fs::InsightOptions options;
    SUBCASE("entry budget") { options.max_entries = 1; }
    SUBCASE("name budget") { options.max_name_bytes = 1; }
    SUBCASE("deadline") { options.time_limit = 0ms; }
    SUBCASE("depth") { options.max_depth = 1; }
    const auto snapshot = scan_insights(dir.path, options, CancelToken{});
    CHECK_FALSE(snapshot.complete());
    CHECK(snapshot.nodes[0].partial);
    CHECK((snapshot.limited || snapshot.skipped_folders > 0));
    const CancelToken cancelled;
    cancelled.cancel();
    const auto stopped = scan_insights(dir.path, {}, cancelled);
    CHECK(stopped.cancelled);
    CHECK(stopped.nodes[0].partial);
    const auto missing = scan_insights(dir.path / "missing", {}, CancelToken{});
    CHECK(missing.unreadable == 1);
    CHECK_FALSE(missing.complete());
    fs::create_directory_symlink(dir.path / "sub", dir.path / "link");
    CHECK_FALSE(scan_insights(dir.path / "link", {}, CancelToken{}).complete());
    CHECK_FALSE(scan_insights(dir.path / "link/", {}, CancelToken{}).complete());
}

TEST_CASE("insights: unreadable folders propagate partial state to their parents") {
    if (geteuid() == 0)
        return;
    const TempDir dir;
    fs::create_directories(dir.path / "closed");
    dir.write("closed/private.pdf", "123");
    fs::permissions(dir.path / "closed", fs::perms::none);
    const auto data = scan_insights(dir.path, {}, CancelToken{});
    fs::permissions(dir.path / "closed", fs::perms::owner_all);
    CHECK(data.unreadable == 1);
    CHECK(data.nodes[0].partial);
    CHECK(data.nodes[1].partial);
}

TEST_CASE("insights: treemap preserves areas and ignores zero or invalid weights") {
    const std::vector<double> weights{60, 30, 10, 0, -1, std::numeric_limits<double>::infinity()};
    const rummage::ui::Rect bounds{10, 20, 600, 400};
    const auto rects = rummage::views::insight_treemap(weights, bounds);
    REQUIRE(rects.size() == weights.size());
    for (std::size_t i = 0; i < 3; ++i) {
        const auto r = rects[i];
        CHECK(r.width * r.height == doctest::Approx(240000 * weights[i] / 100));
        CHECK(r.x >= bounds.x);
        CHECK(r.y >= bounds.y);
        CHECK(r.right() <= bounds.right());
        CHECK(r.bottom() <= bounds.bottom());
        for (std::size_t j = i + 1; j < 3; ++j)
            CHECK(r.intersect(rects[j]).empty());
    }
    for (std::size_t i = 3; i < rects.size(); ++i)
        CHECK(rects[i].empty());
    CHECK(rummage::views::insight_treemap({0, 0}, bounds)[0].empty());
}

TEST_CASE("insights: exact type matches, cached measures, drill-down and refresh history") {
    const TempDir dir;
    fs::create_directories(dir.path / "sub");
    dir.write("a.PDF", "12");
    dir.write("misleading.pdf.txt", "1234567890");
    dir.write("sub/b.pdf", "123");
    dir.write(".hidden.pdf", "x");
    rummage::core::EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    InsightsView dialog{loop, pool, [] {}};
    fs::path revealed;
    dialog.open(dir.path, false, [&](const fs::path& path, bool directory) {
        CHECK_FALSE(directory);
        revealed = path;
    });
    REQUIRE(until(loop, [&] { return !dialog.loading(); }));
    const auto* original = dialog.snapshot();
    REQUIRE(original != nullptr);
    CHECK(dialog.rows()[row(dialog, ".pdf")].files == 2);
    // Size is the initial measure; switching to count and back uses the same snapshot.
    CHECK(dialog.rows().front().label == ".txt");
    dialog.set_size_measure(false);
    CHECK(dialog.rows().front().label == ".pdf");
    dialog.set_size_measure(true);
    CHECK(dialog.snapshot() == original);
    CHECK_FALSE(dialog.loading());
    CHECK(dialog.rows().front().label == ".txt");
    dialog.activate_row(row(dialog, ".pdf"));
    CHECK(dialog.view() == InsightsView::View::matches);
    REQUIRE(dialog.rows().size() == 2);
    CHECK(std::ranges::none_of(dialog.rows(),
                               [](const auto& r) { return r.label == "misleading.pdf.txt"; }));
    dialog.back();
    dialog.set_view(InsightsView::View::space);
    dialog.activate_row(row(dialog, "sub"));
    CHECK(dialog.scope() != 0);
    CHECK(dialog.snapshot() == original);
    REQUIRE(dialog.rows().size() == 1);
    dialog.refresh();
    REQUIRE(until(loop, [&] { return !dialog.loading(); }));
    CHECK(dialog.snapshot()->root == dir.path / "sub");
    dialog.up();
    REQUIRE(until(loop, [&] { return !dialog.loading(); }));
    CHECK(dialog.snapshot()->root == dir.path);
    dialog.set_hidden(true);
    REQUIRE(until(loop, [&] { return !dialog.loading(); }));
    dialog.set_recursive(false);
    REQUIRE(until(loop, [&] { return !dialog.loading(); }));
    dialog.set_view(InsightsView::View::types);
    CHECK(dialog.rows()[row(dialog, ".pdf")].files == 2);
    dialog.activate_row(row(dialog, ".pdf"));
    dialog.activate_row(row(dialog, "a.PDF"));
    CHECK(revealed == dir.path / "a.PDF");
    CHECK_FALSE(dialog.is_open());
    dialog.open(dir.path, false, [&](const fs::path& path, bool directory) {
        CHECK_FALSE(directory);
        revealed = path;
    });
    dialog.set_recursive(true);
    dialog.set_view(InsightsView::View::types);
    REQUIRE(until(loop, [&] { return !dialog.loading(); }));
    dialog.activate_row(row(dialog, ".pdf"));
    dialog.activate_row(row(dialog, "b.pdf"));
    CHECK(revealed == dir.path / "sub/b.pdf");
}

TEST_CASE("insights: partial map keeps unknown siblings available for drilling") {
    rummage::ui::Offscreen offscreen{1100, 820};
    const TempDir dir;
    fs::create_directories(dir.path / "Documents");
    fs::create_directories(dir.path / "Projects");
    dir.write("Documents/report.pdf", "1234");
    dir.write("Projects/notes.md", "123");
    dir.write("large.pdf", std::string(4000, 'x'));
    rummage::core::EventLoop loop;
    rummage::core::WorkerPool pool{loop, 1};
    InsightsView dialog{loop, pool, [] {}, {.max_entries = 3}};
    dialog.open(dir.path, false, {});
    REQUIRE(until(loop, [&] { return !dialog.loading(); }));
    REQUIRE(dialog.snapshot()->limited);
    dialog.set_view(InsightsView::View::space);
    REQUIRE(dialog.rows().size() == 3);
    const auto documents = row(dialog, "Documents");
    CHECK(dialog.rows()[documents].partial);
    CHECK(dialog.rows()[documents].files == 0);
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    renderer.begin_frame(1100, 820, 1, {});
    rummage::ui::IconSet icons{fonts};
    const auto mime = rummage::fs::MimeDatabase::load();
    dialog.draw(renderer, {0, 0, 1100, 820}, {}, icons, *mime);
    renderer.end_frame();
    capture("partial", 1100, 820);
    CHECK_FALSE(dialog.control_bounds("row:" + std::to_string(documents)).empty());
    // Unknown folders have rows but no fictitious chart area.
    CHECK(dialog.control_bounds("tile:" + std::to_string(documents)).empty());
    dialog.activate_row(documents);
    REQUIRE(until(loop, [&] { return !dialog.loading(); }));
    CHECK(dialog.snapshot()->complete());
    CHECK(dialog.snapshot()->root == dir.path / "Documents");
    CHECK(dialog.snapshot()->nodes[0].bytes == 4);
    dialog.up();
    REQUIRE(until(loop, [&] { return !dialog.loading(); }));
    CHECK(dialog.snapshot()->root == dir.path);
    CHECK(dialog.snapshot()->limited);
}

TEST_CASE("insights: reopening restores location, view and options with fresh data") {
    const TempDir dir;
    const auto library = dir.path / "Library";
    fs::create_directories(library / "Research/deep/Many");
    fs::create_directories(library / "Research/deep/Large");
    fs::create_directories(dir.path / "Other");
    dir.write("Library/Research/deep/Many/a.md", "a");
    dir.write("Library/Research/deep/Many/b.md", "b");
    dir.write("Library/Research/deep/Large/book.pdf", std::string(1000, 'x'));
    dir.write("Library/Research/deep/.private.pdf", "hidden");
    dir.write("Other/.private.pdf", "hidden");
    rummage::core::EventLoop loop;
    rummage::core::WorkerPool pool{loop, 1};
    InsightsView dialog{loop, pool, [] {}};
    dialog.open(library, false, {});
    REQUIRE(until(loop, [&] { return !dialog.loading(); }));
    dialog.set_view(InsightsView::View::space);
    dialog.activate_row(row(dialog, "Research"));
    dialog.activate_row(row(dialog, "deep"));
    dialog.set_hidden(true);
    REQUIRE(until(loop, [&] { return !dialog.loading(); }));
    CHECK(dialog.snapshot()->root == library / "Research/deep");
    dialog.set_size_measure(false);
    CHECK(dialog.rows().front().label == "Many");
    dialog.close();
    CHECK(dialog.snapshot() == nullptr);  // Closing still releases the potentially large tree.
    dir.write("Library/Research/deep/new.md", "new");
    dialog.open(library, false, {});
    REQUIRE(until(loop, [&] { return !dialog.loading(); }));
    CHECK(dialog.current_path() == library / "Research/deep");
    CHECK(dialog.view() == InsightsView::View::space);
    CHECK(dialog.rows().front().label == "Many");
    CHECK(dialog.rows()[row(dialog, ".private.pdf")].files == 1);
    CHECK(dialog.rows()[row(dialog, "new.md")].files == 1);
    dialog.back();
    REQUIRE(until(loop, [&] { return !dialog.loading(); }));
    CHECK(dialog.current_path() == library / "Research");
    dialog.up();
    REQUIRE(until(loop, [&] { return !dialog.loading(); }));
    CHECK(dialog.current_path() == library);
    dialog.back();
    REQUIRE(until(loop, [&] { return !dialog.loading(); }));
    CHECK(dialog.current_path() == library / "Research");
    dialog.close();
    dialog.open(dir.path / "Other", false, {});
    REQUIRE(until(loop, [&] { return !dialog.loading(); }));
    CHECK(dialog.current_path() == dir.path / "Other");
    CHECK(dialog.view() == InsightsView::View::space);
    CHECK(dialog.rows().empty());
    CHECK_FALSE(dialog.can_back());
    CHECK(dialog.can_up());
    // At the initial scan root, Up can still visit the actual filesystem parent.
    dialog.up();
    REQUIRE(until(loop, [&] { return !dialog.loading(); }));
    CHECK(dialog.current_path() == dir.path);
}

TEST_CASE("insights: Back restores type matches while Up navigates folders") {
    const TempDir dir;
    fs::create_directories(dir.path / "sub");
    dir.write("sub/a.pdf", "x");
    rummage::core::EventLoop loop;
    rummage::core::WorkerPool pool{loop, 1};
    InsightsView dialog{loop, pool, [] {}};
    dialog.open(dir.path / "sub", false, {});
    REQUIRE(until(loop, [&] { return !dialog.loading(); }));
    dialog.activate_row(row(dialog, ".pdf"));
    dialog.close();
    dialog.open(dir.path / "sub", false, {});
    REQUIRE(until(loop, [&] { return !dialog.loading(); }));
    CHECK(dialog.view() == InsightsView::View::matches);
    CHECK(dialog.rows().front().label == "a.pdf");
    dialog.key({.keysym = XKB_KEY_Up, .text = {}, .modifiers = {.alt = true}});
    REQUIRE(until(loop, [&] { return !dialog.loading(); }));
    CHECK(dialog.current_path() == dir.path);
    CHECK(dialog.view() == InsightsView::View::types);
    dialog.key({.keysym = XKB_KEY_Left, .text = {}, .modifiers = {.alt = true}});
    REQUIRE(until(loop, [&] { return !dialog.loading(); }));
    CHECK(dialog.current_path() == dir.path / "sub");
    CHECK(dialog.view() == InsightsView::View::matches);
    dialog.key({.keysym = XKB_KEY_BackSpace, .text = {}});
    CHECK(dialog.view() == InsightsView::View::types);
}

TEST_CASE("insights: reopening preserves the result page and light theme remains usable") {
    rummage::ui::Offscreen offscreen{1100, 820};
    const TempDir dir;
    for (int i = 0; i < 30; ++i)
        dir.write("report-" + std::to_string(i) + ".pdf", "x");
    rummage::core::EventLoop loop;
    rummage::core::WorkerPool pool{loop, 1};
    InsightsView dialog{loop, pool, [] {}};
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    const auto mime = rummage::fs::MimeDatabase::load();
    const auto draw = [&] {
        renderer.begin_frame(1100, 820, 1, {});
        dialog.draw(renderer, {0, 0, 1100, 820},
                    rummage::theme::desktop_palette({.dark = false, .accent = {}}), icons, *mime);
        renderer.end_frame();
    };
    dialog.open(dir.path, false, {});
    REQUIRE(until(loop, [&] { return !dialog.loading(); }));
    dialog.activate_row(row(dialog, ".pdf"));
    draw();
    std::size_t page_size = 0;
    while (!dialog.control_bounds("row:" + std::to_string(page_size)).empty())
        ++page_size;
    REQUIRE(page_size > 0);
    REQUIRE(page_size < 30);
    dialog.key({.keysym = XKB_KEY_Page_Down, .text = {}});
    draw();
    REQUIRE_FALSE(dialog.control_bounds("row:" + std::to_string(page_size)).empty());
    dialog.close();
    dialog.open(dir.path, false, {});
    REQUIRE(until(loop, [&] { return !dialog.loading(); }));
    draw();
    CHECK(dialog.view() == InsightsView::View::matches);
    CHECK(dialog.control_bounds("row:0").empty());
    CHECK_FALSE(dialog.control_bounds("row:" + std::to_string(page_size)).empty());
    capture("light", 1100, 820);
    dialog.set_view(InsightsView::View::space);
    draw();
    capture("light-files", 1100, 820);
}

TEST_CASE("insights: replacement, stop and close cancel obsolete deliveries") {
    const TempDir first, second;
    first.write("a.pdf", "123");
    second.write("b.md", "1234");
    rummage::core::EventLoop loop;
    rummage::core::WorkerPool pool{loop, 1};
    InsightsView dialog{loop, pool, [] {}};
    dialog.open(first.path, false, {});
    dialog.open(second.path, false, {});
    REQUIRE(until(loop, [&] { return !dialog.loading(); }));
    CHECK(dialog.snapshot()->root == second.path);
    REQUIRE(dialog.rows().size() == 1);
    CHECK(dialog.rows()[0].label == ".md");
    dialog.refresh();
    dialog.stop();
    REQUIRE(until(loop, [&] { return !dialog.loading(); }));
    CHECK(dialog.snapshot()->cancelled);
    CHECK_FALSE(dialog.snapshot()->complete());
    dialog.refresh();
    dialog.close();
    REQUIRE(until(loop, [&] { return !dialog.loading(); }));
    CHECK_FALSE(dialog.is_open());
    CHECK(dialog.snapshot() == nullptr);
}

TEST_CASE("insights: native browser entry points, rendering and pane navigation") {
    rummage::ui::Offscreen offscreen{1100, 820};
    const TempDir dir;
    fs::create_directories(dir.path / "Research");
    fs::create_directories(dir.path / "Projects");
    dir.write("Research/paper.PDF", std::string(12500, 'x'));
    dir.write("Research/notes.md", "notes");
    dir.write("Projects/report.docx", std::string(7200, 'x'));
    dir.write("draft.doc", std::string(2300, 'x'));
    dir.write("README", "example");
    dir.write(".secret.pdf", "hidden");
    rummage::core::EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    rummage::views::Browser browser{loop,    pool,
                                    watcher, rummage::fs::MimeDatabase::load(),
                                    [] {},   [](rummage::ui::CursorShape) {}};
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    browser.open(dir.path);
    REQUIRE(until(loop, [&] { return !browser.loading(); }));
    const auto commands = browser.palette_items();
    CHECK(std::ranges::find(commands, "command:folder_insights",
                            &rummage::views::PaletteItem::id) != commands.end());
    const rummage::ui::KeyEvent shortcut{
        .keysym = XKB_KEY_I, .text = {}, .modifiers = {.ctrl = true, .alt = true}};
    CHECK(rummage::views::command_for(shortcut) == rummage::views::Command::folder_insights);
    browser.key(shortcut, rummage::views::Browser::Clock::now());
    auto& dialog = browser.insights_view();
    REQUIRE(dialog.is_open());
    REQUIRE(until(loop, [&] { return !dialog.loading(); }));
    const auto draw = [&](int width, int height, const std::string& name) {
        renderer.begin_frame(width, height, 1, {});
        browser.draw(renderer, {0, 0, static_cast<float>(width), static_cast<float>(height)},
                     rummage::theme::Palette{}, icons, rummage::views::Browser::Clock::now());
        renderer.end_frame();
        capture(name, width, height);
    };
    draw(1100, 820, "types");
    const auto space = dialog.control_bounds("space");
    REQUIRE_FALSE(space.empty());
    browser.pointer({.kind = rummage::ui::PointerEvent::Kind::press,
                     .x = space.x + 4,
                     .y = space.y + 4,
                     .button = rummage::ui::PointerButton::left},
                    rummage::views::Browser::Clock::now());
    CHECK(dialog.view() == InsightsView::View::space);
    draw(1100, 820, "space");
    CHECK_FALSE(dialog.control_bounds("tile:0").empty());
    dialog.activate_row(row(dialog, "Research"));
    draw(1100, 820, "files");
    CHECK_FALSE(browser.has_overlay());
    CHECK_FALSE(dialog.control_bounds("up").empty());
    const auto up_button = dialog.control_bounds("up");
    dialog.pointer({.kind = rummage::ui::PointerEvent::Kind::press,
                    .x = up_button.x + 5,
                    .y = up_button.y + 5,
                    .button = rummage::ui::PointerButton::left});
    CHECK(dialog.current_path() == dir.path);
    dialog.back();
    CHECK(dialog.current_path() == dir.path / "Research");
    draw(1100, 820, "files");
    browser.key({.keysym = XKB_KEY_Up, .text = {}, .modifiers = {.alt = true}},
                rummage::views::Browser::Clock::now());
    CHECK(dialog.current_path() == dir.path);
    draw(640, 600, "compact");
    for (const auto id : {"close", "recursive", "hidden", "refresh"}) {
        const auto r = dialog.control_bounds(id);
        CHECK_FALSE(r.empty());
        CHECK(r.x >= 0);
        CHECK(r.y >= 0);
        CHECK(r.right() <= 640);
        CHECK(r.bottom() <= 600);
    }
    dialog.set_view(InsightsView::View::types);
    dialog.activate_row(row(dialog, ".pdf"));
    draw(1100, 820, "matches");
    browser.key({.keysym = XKB_KEY_Escape, .text = {}}, rummage::views::Browser::Clock::now());
    CHECK(dialog.is_open());
    CHECK(dialog.view() == InsightsView::View::types);
    browser.key({.keysym = XKB_KEY_Escape, .text = {}}, rummage::views::Browser::Clock::now());
    CHECK_FALSE(dialog.is_open());
    browser.set_name_filter({});
    browser.show_insights(dir.path);
    dialog.set_hidden(true);
    REQUIRE(until(loop, [&] { return !dialog.loading(); }));
    dialog.activate_row(row(dialog, ".pdf"));
    dialog.select_row(row(dialog, ".secret.pdf"));
    REQUIRE(until(loop, [&] { return !browser.loading(); }));
    CHECK(dialog.is_open());
    const auto selected = browser.picker_folder()->single_selection();
    REQUIRE(selected != nullptr);
    CHECK(selected->name == ".secret.pdf");
    dialog.close();
    browser.set_picker_mode(true);
    browser.show_insights(dir.path);
    CHECK_FALSE(dialog.is_open());
}

TEST_CASE("insights: modification age boundaries include future and extreme dates") {
    using rummage::fs::insight_age;
    using rummage::fs::InsightAge;
    constexpr std::int64_t now = 1'800'000'000;
    CHECK(insight_age(now + 1, now) == InsightAge::future);
    CHECK(insight_age(now, now) == InsightAge::day);
    CHECK(insight_age(now - 86399, now) == InsightAge::day);
    CHECK(insight_age(now - 86400, now) == InsightAge::week);
    CHECK(insight_age(now - 7 * 86400, now) == InsightAge::month);
    CHECK(insight_age(now - 30 * 86400, now) == InsightAge::year);
    CHECK(insight_age(now - 365 * 86400, now) == InsightAge::older);
    CHECK(insight_age(std::numeric_limits<std::int64_t>::min(),
                      std::numeric_limits<std::int64_t>::max()) == InsightAge::older);
}

TEST_CASE("insights: largest files and age graphs drill down and preserve scope") {
    rummage::ui::Offscreen offscreen{1100, 820};
    const TempDir dir;
    fs::create_directory(dir.path / "Research");
    dir.write("Research/large.pdf", std::string(12000, 'x'));
    dir.write("recent.md", std::string(3000, 'x'));
    dir.write("old.txt", std::string(1000, 'x'));
    dir.write("future.txt", "x");
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    for (const auto& [name, age] : {std::pair{"Research/large.pdf", 10 * 86400},
                                    {"old.txt", 400 * 86400},
                                    {"future.txt", -86400}}) {
        const timespec times[2]{{now - age, 0}, {now - age, 0}};
        REQUIRE(utimensat(AT_FDCWD, (dir.path / name).c_str(), times, 0) == 0);
    }
    rummage::core::EventLoop loop;
    rummage::core::WorkerPool pool{loop, 1};
    InsightsView dialog{loop, pool, [] {}};
    fs::path revealed;
    dialog.open(dir.path, false, [&](const auto& target, bool directory) {
        CHECK_FALSE(directory);
        revealed = target;
    });
    REQUIRE(until(loop, [&] { return !dialog.loading(); }));
    dialog.set_size_measure(false);
    dialog.set_view(InsightsView::View::largest);
    REQUIRE(dialog.rows().size() == 4);
    CHECK(dialog.rows()[0].label == "large.pdf");
    CHECK(dialog.rows()[0].bytes == 12000);
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    const auto mime = rummage::fs::MimeDatabase::load();
    const auto draw = [&](const std::string& name, int width = 1100, int height = 820) {
        renderer.begin_frame(width, height, 1, {});
        dialog.draw(renderer, {0, 0, static_cast<float>(width), static_cast<float>(height)},
                    name.starts_with("light-")
                        ? rummage::theme::desktop_palette({.dark = false, .accent = {}})
                        : rummage::theme::Palette{},
                    icons, *mime);
        renderer.end_frame();
        capture(name, width, height);
    };
    draw("largest");
    draw("light-largest");
    dialog.set_view(InsightsView::View::age);
    CHECK(dialog.rows().size() == 4);
    CHECK(dialog.rows()[row(dialog, "7–30 days")].bytes == 12000);
    CHECK(dialog.rows()[row(dialog, "Future dates")].files == 1);
    draw("age");
    draw("light-age");
    dialog.activate_row(row(dialog, "7–30 days"));
    REQUIRE(dialog.rows().size() == 1);
    CHECK(dialog.rows()[0].label == "large.pdf");
    dialog.back();
    CHECK(dialog.view() == InsightsView::View::age);
    dialog.set_size_measure(true);
    draw("age-size");
    draw("age-compact", 640, 640);
    dialog.set_recursive(false);
    REQUIRE(until(loop, [&] { return !dialog.loading(); }));
    CHECK(dialog.rows().size() == 3);
    dialog.set_view(InsightsView::View::largest);
    REQUIRE(dialog.rows().size() == 3);
    CHECK(dialog.rows()[0].label == "recent.md");
    dialog.activate_row(0);
    CHECK(revealed == dir.path / "recent.md");
    CHECK_FALSE(dialog.is_open());
}

#ifdef RUMMAGE_TEST_TOKEI
TEST_CASE("insights: real Tokei language graph, embedded files, options and resume") {
    rummage::ui::Offscreen offscreen{1100, 820};
    const TempDir dir;
    fs::create_directory(dir.path / "src");
    fs::create_directory(dir.path / ".git");
    dir.write("README.md", "# Example\n\n```python\n# Example\nprint('hello')\n```\n");
    dir.write("src/main.rs", "// A comment\nfn main() {}\n\n");
    dir.write(".hidden.py", "print('hidden')\n");
    dir.write("ignored.py", "print('ignored')\n");
    dir.write(".gitignore", "ignored.py\n");
    rummage::core::EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    InsightsView dialog{loop, pool, [] {}};
    fs::path revealed;
    const auto reveal = [&](const auto& target, bool directory) {
        CHECK_FALSE(directory);
        revealed = target;
    };
    dialog.open(dir.path, false, reveal, InsightsView::View::languages);
    REQUIRE(until(loop, [&] { return !dialog.loading(); }));
    REQUIRE(dialog.code_report() != nullptr);
    REQUIRE(dialog.code_report()->error == rummage::platform::CodeError::none);
    CHECK(dialog.code_report()->files == 2);
    CHECK(dialog.code_report()->total.lines() == 9);
    REQUIRE(dialog.rows().size() == 3);
    CHECK(dialog.rows()[row(dialog, "Python")].depth == 1);
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    const auto mime = rummage::fs::MimeDatabase::load();
    const auto draw = [&](const std::string& name, int width = 1100, int height = 820) {
        renderer.begin_frame(width, height, 1, {});
        dialog.draw(renderer, {0, 0, static_cast<float>(width), static_cast<float>(height)},
                    name.starts_with("light-")
                        ? rummage::theme::desktop_palette({.dark = false, .accent = {}})
                        : rummage::theme::Palette{},
                    icons, *mime);
        renderer.end_frame();
        capture(name, width, height);
    };
    draw("languages");
    draw("light-languages");
    draw("languages-compact", 640, 640);
    dialog.activate_row(row(dialog, "Python"));
    CHECK(dialog.view() == InsightsView::View::code_matches);
    REQUIRE(dialog.rows().size() == 1);
    CHECK(dialog.rows()[0].label == "README.md");
    CHECK(dialog.rows()[0].code.lines() == 2);
    draw("language-files");
    dialog.close();
    dialog.open(dir.path, false, reveal);
    REQUIRE(until(loop, [&] { return !dialog.loading(); }));
    CHECK(dialog.view() == InsightsView::View::code_matches);
    REQUIRE(dialog.rows().size() == 1);
    dialog.back();
    CHECK(dialog.view() == InsightsView::View::languages);
    dialog.set_recursive(false);
    REQUIRE(until(loop, [&] { return !dialog.loading(); }));
    REQUIRE(dialog.code_report()->error == rummage::platform::CodeError::none);
    CHECK(dialog.code_report()->files == 1);
    dialog.set_hidden(true);
    REQUIRE(until(loop, [&] { return !dialog.loading(); }));
    CHECK(dialog.code_report()->files == 2);
    dialog.set_view(InsightsView::View::space);
    REQUIRE(until(loop, [&] { return !dialog.loading(); }));
    CHECK(dialog.snapshot() != nullptr);
    dialog.back();
    REQUIRE(until(loop, [&] { return !dialog.loading(); }));
    CHECK(dialog.view() == InsightsView::View::languages);
    dialog.activate_row(row(dialog, "Markdown"));
    dialog.activate_row(0);
    CHECK(revealed == dir.path / "README.md");
    CHECK_FALSE(dialog.is_open());
}
#endif

TEST_CASE(
    "insights: native results select, open, preview, copy and rename without leaving their pane") {
    using rummage::ui::PointerButton;
    using rummage::ui::PointerEvent;
    using rummage::views::Browser;
    rummage::ui::Offscreen offscreen{1400, 900};
    const TempDir dir;
    fs::create_directories(dir.path / "one");
    fs::create_directories(dir.path / "two");
    dir.write("one/report.md", "# First\n");
    dir.write("one/other.md", "# Another\n");
    dir.write("one/unrelated.txt", "Not a result\n");
    dir.write("two/report.md", "# Second file\n");
    rummage::core::EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop, pool, watcher, rummage::fs::MimeDatabase::load(), [] {}, [](auto) {}};
    fs::path opened;
    browser.set_open_handler([&](const fs::path& parent, auto files) {
        REQUIRE(files.size() == 1);
        opened = parent / files[0].name;
    });
    std::vector<rummage::views::SystemClipboard::Format> offered;
    rummage::views::SystemClipboard clipboard;
    clipboard.offer = [&](auto formats) { offered = std::move(formats); };
    browser.set_system_clipboard(std::move(clipboard));
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    const auto draw = [&](const std::string& name) {
        renderer.begin_frame(1400, 900, 1, {});
        browser.draw(renderer, {0, 0, 1400, 900}, rummage::theme::Palette{}, icons,
                     Browser::Clock::now());
        renderer.end_frame();
        capture(name, 1400, 900);
    };
    browser.open(dir.path);
    REQUIRE(until(loop, [&] { return !browser.loading(); }));
    browser.show_insights(dir.path);
    auto& view = browser.insights_view();
    REQUIRE(until(loop, [&] { return !view.loading(); }));
    view.activate_row(row(view, ".md"));
    REQUIRE(view.rows().size() == 3);
    CHECK_FALSE(browser.has_overlay());
    draw("pane-matches");
    const auto click = [&](std::size_t index, PointerButton button = PointerButton::left,
                           int clicks = 1) {
        const auto rect = view.control_bounds(std::format("row:{}", index));
        REQUIRE_FALSE(rect.empty());
        browser.pointer({.kind = PointerEvent::Kind::press,
                         .x = rect.x + 50,
                         .y = rect.y + 15,
                         .button = button,
                         .click_count = clicks},
                        Browser::Clock::now());
        browser.pointer({.kind = PointerEvent::Kind::release,
                         .x = rect.x + 50,
                         .y = rect.y + 15,
                         .button = button},
                        Browser::Clock::now());
    };
    // Same basenames from different folders retain their actual file-operation target.
    const auto target = std::ranges::find_if(view.rows(), [&](const auto& entry) {
        return view.snapshot()->relative_path(entry.node) == fs::path{"two/report.md"};
    });
    REQUIRE(target != view.rows().end());
    const auto index = static_cast<std::size_t>(target - view.rows().begin());
    click(index);
    REQUIRE(until(loop, [&] { return !view.loading(); }));
    CHECK(opened.empty());
    REQUIRE(browser.picker_folder()->single_selection());
    CHECK(browser.picker_folder()->path() == dir.path / "two");
    CHECK(browser.picker_folder()->single_selection()->name == "report.md");
    draw("pane-selected");
    CHECK_FALSE(browser.info_pane().copy_bounds().empty());
    browser.key({.keysym = XKB_KEY_c, .text = {}, .modifiers = {.ctrl = true}},
                Browser::Clock::now());
    REQUIRE_FALSE(offered.empty());
    CHECK(std::ranges::any_of(offered, [&](const auto& format) {
        return format.second.find((dir.path / "two/report.md").string()) != std::string::npos;
    }));
    click(index, PointerButton::left, 2);
    CHECK(opened == dir.path / "two/report.md");
    CHECK(view.is_open());
    browser.key({.keysym = XKB_KEY_space, .text = {}}, Browser::Clock::now());
    CHECK(browser.quick_look().is_open());
    browser.key({.keysym = XKB_KEY_Escape, .text = {}}, Browser::Clock::now());
    CHECK_FALSE(browser.quick_look().is_open());
    CHECK(view.is_open());
    click(index, PointerButton::right);
    CHECK(browser.context_menu().is_open());
    browser.context_menu().close();
    browser.key({.keysym = XKB_KEY_F2, .text = {}}, Browser::Clock::now());
    REQUIRE(browser.name_editor().active());
    draw("pane-rename");
    REQUIRE(browser.name_editor().slot().rect.has_value());
    browser.name_editor().paste("renamed");
    browser.key({.keysym = XKB_KEY_Return, .text = {}}, Browser::Clock::now());
    REQUIRE(until(loop, [&] {
        return !browser.name_editor().active() && fs::exists(dir.path / "two/renamed.md");
    }));
    REQUIRE(until(loop, [&] { return view.rows()[index].label == "renamed.md"; }));
    CHECK(view.is_open());
    CHECK_FALSE(fs::exists(dir.path / "two/report.md"));
    CHECK(fs::exists(dir.path / "one/report.md"));
    browser.set_name_filter("other");
    REQUIRE(view.rows().size() == 1);
    CHECK(view.rows()[0].label == "other.md");
    browser.set_name_filter({});
    view.select_row(row(view, "other.md"));
    REQUIRE(until(loop, [&] { return !view.loading(); }));
    browser.key({.keysym = XKB_KEY_a, .text = {}, .modifiers = {.ctrl = true}},
                Browser::Clock::now());
    CHECK(browser.picker_folder()->selected_count() == 2);
    CHECK_FALSE(browser.picker_folder()->is_selected("unrelated.txt"));
    browser.new_tab();
    CHECK_FALSE(browser.insights_view().is_open());
    browser.select_tab(0);
    CHECK(browser.insights_view().is_open());
    CHECK(browser.insights_view().view() == InsightsView::View::matches);
    draw("pane-restored");
    browser.key({.keysym = XKB_KEY_i, .text = {}, .modifiers = {.ctrl = true, .shift = true}},
                Browser::Clock::now());
    CHECK(browser.picker_folder()->selected_count() == 0);
    browser.toggle_split();
    CHECK(browser.active_pane() == 1);
    CHECK_FALSE(browser.insights_view().is_open());
    browser.open(dir.path / "two");
    REQUIRE(until(loop, [&] { return !browser.loading(); }));
    browser.focus_pane(0);
    CHECK(browser.insights_view().is_open());
    draw("pane-split");
    CHECK_FALSE(browser.has_overlay());
    browser.focus_pane(1);
    CHECK(browser.picker_folder()->path() == dir.path / "two");
    browser.focus_pane(0);
    browser.toggle_split();
    browser.open(dir.path / "two");
    CHECK_FALSE(browser.insights_view().is_open());
    REQUIRE(until(loop, [&] { return !browser.loading(); }));
    CHECK(browser.picker_folder()->path() == dir.path / "two");
#ifdef RUMMAGE_TEST_TOKEI
    browser.show_insights(dir.path, InsightsView::View::languages);
    REQUIRE(until(loop, [&] { return !view.loading(); }));
    view.activate_row(row(view, "Markdown"));
    REQUIRE(view.view() == InsightsView::View::code_matches);
    REQUIRE(view.rows().size() == 3);
    view.select_row(0);
    REQUIRE(until(loop, [&] { return !view.loading(); }));
    draw("pane-source-files");
    opened.clear();
    browser.key({.keysym = XKB_KEY_Return, .text = {}}, Browser::Clock::now());
    CHECK_FALSE(opened.empty());
    CHECK(view.is_open());
    // Tab moves focus away from the result; Enter must activate that control.
    browser.key({.keysym = XKB_KEY_Tab, .text = {}, .modifiers = {.shift = true}},
                Browser::Clock::now());
    opened.clear();
    browser.key({.keysym = XKB_KEY_Return, .text = {}}, Browser::Clock::now());
    CHECK(opened.empty());
#endif
}
