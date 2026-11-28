#include "rummage/views/location_dialog.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "temp_dir.hpp"

using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {
rummage::ui::KeyEvent key(xkb_keysym_t keysym) {
    return {.keysym = keysym, .text = {}, .modifiers = {}};
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

TEST_CASE("location dialog: Enter goes, a wrong address stays with what is wrong") {
    rummage::views::LocationDialog dialog{[] {}};
    std::vector<std::pair<std::string, bool>> asked;
    dialog.open("/home/me", [&](const std::string& text, bool add) {
        asked.emplace_back(text, add);
        return text.starts_with("smb://") ? std::string{} : std::string{"wrong"};
    });
    REQUIRE(dialog.is_open());
    CHECK_FALSE(dialog.add());
    dialog.paste("nas/x");  // replaces the folder, all selected
    CHECK(dialog.text() == "nas/x");
    dialog.key(key(XKB_KEY_Return));
    CHECK(dialog.is_open());
    CHECK(dialog.error() == "wrong");
    // Adding a server is an explicit choice; Ctrl+Tab reaches the checkbox.
    auto focus = key(XKB_KEY_Tab);
    focus.modifiers.ctrl = true;
    dialog.key(focus);
    dialog.key(key(XKB_KEY_space));
    CHECK(dialog.add());
    dialog.key(key(XKB_KEY_Tab));
    dialog.key(key(XKB_KEY_Home));
    dialog.paste("smb://");
    CHECK(dialog.error().empty());  // typing again clears it
    dialog.paste("\nnot this");
    dialog.key(key(XKB_KEY_KP_Enter));
    CHECK_FALSE(dialog.is_open());
    REQUIRE(asked.size() == 2);
    CHECK(asked[1] == std::pair<std::string, bool>{"smb://nas/x", true});

    dialog.open("/", [](const std::string& /*text*/, bool /*add*/) { return std::string{}; });
    CHECK_FALSE(dialog.add());  // every browsing visit starts without saving a server
    dialog.key(key(XKB_KEY_Escape));
    CHECK_FALSE(dialog.is_open());
}

TEST_CASE("location dialog: Tab completes and cycles local paths without leaving the field") {
    rummage::test::TempDir dir;
    fs::create_directory(dir.path / "École");
    fs::create_directory(dir.path / "Été");
    fs::create_directory(dir.path / "École" / "Nested");
    rummage::core::EventLoop loop;
    rummage::core::WorkerPool pool{loop, 1};
    rummage::views::LocationDialog dialog{[] {}, &pool};
    const auto open = [&] {
        dialog.open(dir.path.string(), [](const auto&, bool) { return std::string{}; }, dir.path);
    };
    open();
    dialog.paste("~/É");
    dialog.key(key(XKB_KEY_Tab));
    REQUIRE(run_until(loop, [&] { return dialog.text() == (dir.path / "École").string() + "/"; }));
    CHECK(dialog.input_field() != nullptr);
    CHECK_FALSE(dialog.add());
    dialog.key(key(XKB_KEY_Tab));
    CHECK(dialog.text() == (dir.path / "Été").string() + "/");
    dialog.key(key(XKB_KEY_ISO_Left_Tab));
    CHECK(dialog.text() == (dir.path / "École").string() + "/");
    dialog.paste("Ne");
    dialog.key(key(XKB_KEY_Tab));
    REQUIRE(run_until(
        loop, [&] { return dialog.text() == (dir.path / "École" / "Nested").string() + "/"; }));
    CHECK(dialog.error().empty());

    open();
    dialog.paste("É");  // resolves against the folder shown when Ctrl+L opened
    dialog.key(key(XKB_KEY_Tab));
    REQUIRE(run_until(loop, [&] { return dialog.text() == (dir.path / "École").string() + "/"; }));
    open();
    dialog.paste("~/missing");
    dialog.key(key(XKB_KEY_Tab));
    REQUIRE(run_until(loop, [&] { return !dialog.error().empty(); }));
    CHECK(dialog.input_field() != nullptr);
    CHECK_FALSE(dialog.add());
    CHECK(dialog.text() == "~/missing");
}

TEST_CASE("location dialog: pending completion cannot overwrite edits or a reopened dialog") {
    rummage::test::TempDir dir;
    fs::create_directory(dir.path / "Match");
    rummage::core::EventLoop loop;
    rummage::core::WorkerPool pool{loop, 1};
    rummage::views::LocationDialog dialog{[] {}, &pool};
    const auto open = [&] {
        dialog.open(dir.path.string(), [](const auto&, bool) { return std::string{}; }, dir.path);
    };
    const auto drain = [&] {
        bool done = false;
        pool.submit(
            rummage::core::Priority::visible, [](const auto&) { return true; },
            [&](bool result) { done = result; });
        CHECK(run_until(loop, [&] { return done; }));
    };
    open();
    dialog.paste("~/Ma");
    dialog.key(key(XKB_KEY_Tab));
    dialog.paste("nual");
    drain();
    CHECK(dialog.text() == "~/Manual");

    open();
    dialog.paste("~/Ma");
    dialog.key(key(XKB_KEY_Tab));
    dialog.close();
    open();
    drain();
    CHECK(dialog.text() == dir.path.string());
    CHECK(dialog.error().empty());
    CHECK_FALSE(dialog.add());

    {
        rummage::views::LocationDialog transient{[] {}, &pool};
        transient.open("~/Ma", [](const auto&, bool) { return std::string{}; }, dir.path);
        transient.key(key(XKB_KEY_Tab));
    }
    drain();  // cancelled delivery after destruction must never access the dialog
}
