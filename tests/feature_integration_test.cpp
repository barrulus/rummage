#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <utility>
#include <variant>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/archive_folder.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/preview/preview.hpp"
#include "rummage/views/browser.hpp"
#include "temp_dir.hpp"

namespace {
using namespace std::chrono_literals;
namespace fs = std::filesystem;
using rummage::views::Browser;

bool settled(rummage::core::EventLoop& loop, const std::function<bool()>& done) {
    bool complete = false;
    const auto poll = loop.add_timer(1ms, 1ms, [&] {
        if (done()) {
            complete = true;
            loop.quit(0);
        }
    });
    const auto limit = loop.add_timer(5s, 0ms, [&] { loop.quit(1); });
    loop.run();
    loop.remove(poll);
    loop.remove(limit);
    return complete;
}

std::string contents(const fs::path& path) {
    std::ifstream file{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}
}  // namespace

TEST_CASE("feature integration: preview Markdown in an archive then extract and batch rename") {
    rummage::test::TempDir dir;
    const auto archive = dir.path / "notes.zip";
    fs::copy_file(fs::path{RUMMAGE_TEST_DATA} / "feature-integration" / "notes.zip", archive);
    const auto original_archive = contents(archive);
    const auto destination = dir.path / "output";
    fs::create_directory(destination);
    rummage::fs::forget_archive_trees();
    rummage::core::EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop,    pool,
                    watcher, rummage::fs::MimeDatabase::load(),
                    [] {},   [](rummage::ui::CursorShape) {}};
    const auto wait = [&] { return settled(loop, [&] { return !browser.loading(); }); };
    const auto press = [&](xkb_keysym_t key, rummage::ui::Modifiers modifiers = {},
                           std::string text = {}) {
        rummage::ui::KeyEvent event;
        event.keysym = key;
        event.modifiers = modifiers;
        event.text = std::move(text);
        browser.key(event, Browser::Clock::now());
    };

    browser.go_to(archive, "a.md");
    REQUIRE(wait());
    REQUIRE(browser.columns().focused_folder()->is_archive());
    press(XKB_KEY_space);
    REQUIRE(settled(loop, [&] { return !browser.quick_look().loading(); }));
    const auto* content = browser.quick_look().content();
    REQUIRE(content != nullptr);
    const auto* text = std::get_if<rummage::preview::Text>(content);
    REQUIRE(text != nullptr);
    CHECK(text->language == "Markdown");
    REQUIRE_FALSE(text->markdown.blocks.empty());
    CHECK(text->markdown.blocks.front().kind == rummage::text::MarkdownKind::heading);
    CHECK(browser.quick_look().markdown_rendered());
    press(XKB_KEY_m);
    CHECK_FALSE(browser.quick_look().markdown_rendered());
    press(XKB_KEY_m);
    CHECK(browser.quick_look().markdown_rendered());
    press(XKB_KEY_Escape);

    press(XKB_KEY_a, {.ctrl = true});
    REQUIRE(browser.columns().focused_folder()->selected_count() == 2);
    press(XKB_KEY_F2);
    CHECK_FALSE(browser.batch_rename_dialog().is_open());
    CHECK(browser.status_bar().message().find("read-only") != std::string::npos);
    press(XKB_KEY_c, {.ctrl = true});
    browser.open(destination);
    REQUIRE(wait());
    press(XKB_KEY_v, {.ctrl = true});
    REQUIRE(settled(loop, [&] {
        return fs::exists(destination / "a.md") && fs::exists(destination / "b.md") &&
               !browser.loading() && browser.columns().focused_folder()->entries().size() == 2;
    }));
    const auto first = contents(destination / "a.md");
    const auto second = contents(destination / "b.md");
    CHECK(first.starts_with("# First note"));
    CHECK(second.starts_with("# Second note"));

    press(XKB_KEY_a, {.ctrl = true});
    press(XKB_KEY_F2);
    REQUIRE(settled(loop, [&] {
        return browser.batch_rename_dialog().is_open() && !browser.batch_rename_dialog().busy();
    }));
    press(XKB_KEY_Tab);
    press(XKB_KEY_Tab);  // template, selected on focus
    press(XKB_KEY_NoSymbol, {}, "Note_{n}{ext}");
    REQUIRE(browser.batch_rename_dialog().can_apply());
    press(XKB_KEY_Return);
    REQUIRE(settled(loop, [&] {
        return fs::exists(destination / "Note_01.md") && fs::exists(destination / "Note_02.md") &&
               browser.last_operation() &&
               browser.last_operation()->kind == rummage::views::FileOperation::Kind::batch_rename;
    }));
    CHECK(contents(destination / "Note_01.md") == first);
    CHECK(contents(destination / "Note_02.md") == second);
    press(XKB_KEY_z, {.ctrl = true});
    REQUIRE(settled(loop, [&] {
        return fs::exists(destination / "a.md") && fs::exists(destination / "b.md") &&
               browser.undone_operation().has_value();
    }));
    CHECK(contents(destination / "a.md") == first);
    CHECK(contents(destination / "b.md") == second);
    CHECK(contents(archive) == original_archive);
}
