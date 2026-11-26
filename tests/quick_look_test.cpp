#include "rummage/views/quick_look.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/log.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/engines/engines.hpp"
#include "rummage/formats/image/load.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/picture/png.hpp"
#include "rummage/theme/palette.hpp"
#include "rummage/ui/icons.hpp"
#include "rummage/ui/offscreen.hpp"
#include "rummage/ui/renderer.hpp"
#include "rummage/ui/text.hpp"
#include "rummage/views/browser.hpp"
#include "temp_dir.hpp"

using rummage::core::EventLoop;
using rummage::core::WorkerPool;
using rummage::views::Browser;
using rummage::views::QuickLook;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

const fs::path kData = fs::path{RUMMAGE_TEST_DATA} / "image";

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

// dir/{sub/, a.png (48x32), b.jpg (EXIF orientation 6: 32x48 upright), notes.txt}
struct Fixture {
    rummage::test::TempDir dir;
    EventLoop loop;
    WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop,    pool,
                    watcher, rummage::fs::MimeDatabase::load(),
                    [] {},   [](rummage::ui::CursorShape /*shape*/) {}};

    explicit Fixture(bool sniff = true) {
        fs::create_directory(dir.path / "sub");
        fs::copy_file(kData / "blocks.png", dir.path / "a.png");
        fs::copy_file(kData / "blocks-orient6.jpg", dir.path / "b.jpg");
        std::ofstream{dir.path / "notes.txt"} << "notes";
        browser.open(dir.path);
        // Nothing is drawn here: ask for the types the views would sniff for their rows.
        run_until(loop, [this] { return !browser.loading(); });
        if (sniff) {
            browser.columns().column(0).folder().sniff(0, 10);
        }
    }

    // Loaded, and every entry's type known (a name like a.png may need its content).
    bool settle() {
        return run_until(loop, [this] {
            const auto& folder = browser.columns().column(0).folder();
            for (const rummage::fs::Entry& entry : folder.entries()) {
                if (folder.mime_of(entry) == rummage::fs::kUnknownMime) {
                    return false;
                }
            }
            return !browser.loading();
        });
    }

    void press(xkb_keysym_t key, rummage::ui::Modifiers modifiers = {}) {
        rummage::ui::KeyEvent event;
        event.keysym = key;
        event.modifiers = modifiers;
        browser.key(event, Browser::Clock::now());
    }

    using Size = std::optional<std::pair<std::uint32_t, std::uint32_t>>;

    Size image_size() { return browser.quick_look().image_size(); }
};

}  // namespace

TEST_CASE("quick look: Space opens it on the cursor, arrows walk, Space and Escape close") {
    Fixture f;
    REQUIRE(f.settle());
    auto& look = f.browser.quick_look();
    f.press(XKB_KEY_space);  // no cursor yet: nothing to show
    CHECK_FALSE(look.is_open());

    f.press(XKB_KEY_Down);  // sub
    f.press(XKB_KEY_Down);  // a.png
    f.press(XKB_KEY_space);
    REQUIRE(look.is_open());
    CHECK(look.shown() == "a.png");
    REQUIRE(f.settle());
    CHECK(f.image_size() == Fixture::Size{{48, 32}});

    f.press(XKB_KEY_Down);  // the view moves, Quick Look follows
    CHECK(look.shown() == "b.jpg");
    REQUIRE(f.settle());
    CHECK(f.image_size() == Fixture::Size{{32, 48}});  // upright

    f.press(XKB_KEY_Down);  // text: a page of lines
    CHECK(look.shown() == "notes.txt");
    REQUIRE(f.settle());
    CHECK_FALSE(f.image_size());
    REQUIRE(look.content() != nullptr);
    const auto* text = std::get_if<rummage::preview::Text>(look.content());
    REQUIRE(text != nullptr);
    CHECK(text->lines == std::vector<std::string>{"notes"});

    f.press(XKB_KEY_Up);
    f.press(XKB_KEY_Escape);
    CHECK_FALSE(look.is_open());
    CHECK(f.browser.columns().column(0).folder().cursor_entry()->name == "b.jpg");

    f.press(XKB_KEY_space);
    CHECK(look.is_open());
    f.press(XKB_KEY_space);
    CHECK_FALSE(look.is_open());
}

TEST_CASE("quick look: Escape closes it without reaching the view's selection") {
    Fixture f;
    REQUIRE(f.settle());
    f.press(XKB_KEY_Down);
    f.press(XKB_KEY_Down);                   // a.png
    f.press(XKB_KEY_Down, {.shift = true});  // a.png, b.jpg
    auto& folder = f.browser.columns().column(0).folder();
    REQUIRE(folder.selected_count() == 2);

    f.press(XKB_KEY_space);
    CHECK(f.browser.quick_look().shown() == "b.jpg");
    f.press(XKB_KEY_Escape);
    CHECK_FALSE(f.browser.quick_look().is_open());
    CHECK(folder.selected_count() == 2);  // a second Escape would collapse it

    f.press(XKB_KEY_space, {.ctrl = true});  // Ctrl+Space is selection, not Quick Look
    CHECK_FALSE(f.browser.quick_look().is_open());
    CHECK(folder.selected_count() == 1);
}

TEST_CASE("quick look: works over the grid, and a click closes it") {
    Fixture f;
    REQUIRE(f.settle());
    f.press(XKB_KEY_2, {.ctrl = true});
    f.press(XKB_KEY_Right);  // sub
    f.press(XKB_KEY_Right);  // a.png
    f.press(XKB_KEY_space);
    CHECK(f.browser.quick_look().shown() == "a.png");
    f.press(XKB_KEY_Right);
    CHECK(f.browser.quick_look().shown() == "b.jpg");
    rummage::ui::PointerEvent click;
    click.kind = rummage::ui::PointerEvent::Kind::press;
    click.button = rummage::ui::PointerButton::left;
    f.browser.pointer(click, Browser::Clock::now());
    CHECK_FALSE(f.browser.quick_look().is_open());
}

TEST_CASE("quick look: an image whose type is still unknown is shown once it is sniffed") {
    Fixture f{false};
    f.press(XKB_KEY_Down);
    f.press(XKB_KEY_Down);  // a.png: its name alone does not decide its type
    auto& folder = f.browser.columns().column(0).folder();
    REQUIRE(folder.mime_of(*folder.cursor_entry()) == rummage::fs::kUnknownMime);
    f.press(XKB_KEY_space);
    REQUIRE(f.browser.quick_look().is_open());
    REQUIRE(f.settle());  // Quick Look asked for its type; meanwhile, the information card
    CHECK_FALSE(f.image_size());
    f.browser.quick_look().follow(f.browser.columns().focused_folder());  // as every frame does
    REQUIRE(f.settle());
    CHECK(f.image_size() == Fixture::Size{{48, 32}});
}

TEST_CASE("quick look: a folder's card counts its entries; a wheel turn over text is taken") {
    Fixture f;
    REQUIRE(f.settle());
    auto& look = f.browser.quick_look();
    f.press(XKB_KEY_Down);  // sub
    f.press(XKB_KEY_space);
    REQUIRE(f.settle());
    REQUIRE(look.content() != nullptr);
    const auto* info = std::get_if<rummage::preview::Info>(look.content());
    REQUIRE(info != nullptr);
    CHECK(info->items == std::optional<std::uint64_t>{0});
    CHECK(info->permissions.front() == 'd');

    f.press(XKB_KEY_End);  // notes.txt
    REQUIRE(f.settle());
    rummage::ui::PointerEvent wheel;
    wheel.kind = rummage::ui::PointerEvent::Kind::scroll;
    wheel.wheel_y = 3.0F;
    f.browser.pointer(wheel, Browser::Clock::now());
    CHECK(look.is_open());  // scrolling does not close it
    CHECK(look.shown() == "notes.txt");
}

TEST_CASE("quick look: Ctrl+A and Ctrl+C take a text page's text, not the files") {
    Fixture f;
    std::ofstream{f.dir.path / "notes.txt"} << "first line\nsecond line\n";
    std::vector<rummage::views::SystemClipboard::Format> offered;
    rummage::views::SystemClipboard clipboard;
    clipboard.offer = [&](std::vector<rummage::views::SystemClipboard::Format> formats) {
        offered = std::move(formats);
    };
    f.browser.set_system_clipboard(std::move(clipboard));
    REQUIRE(f.settle());
    auto& look = f.browser.quick_look();
    f.press(XKB_KEY_End);  // notes.txt
    f.press(XKB_KEY_space);
    REQUIRE(f.settle());
    REQUIRE(look.shows_text());
    // Nothing selected: Ctrl+C copies the file, as without Quick Look.
    f.press(XKB_KEY_c, {.ctrl = true});
    REQUIRE_FALSE(offered.empty());
    CHECK(offered.front().first == "x-special/gnome-copied-files");
    f.press(XKB_KEY_a, {.ctrl = true});
    CHECK(look.selected_text() == "first line\nsecond line");
    f.press(XKB_KEY_c, {.ctrl = true});
    REQUIRE(offered.size() == 3);
    CHECK(offered.front().first == "text/plain;charset=utf-8");
    CHECK(offered.front().second == "first line\nsecond line");
    CHECK(look.is_open());
    // Walking to another entry drops the selection.
    f.press(XKB_KEY_Up);
    CHECK(look.selected_text().empty());
}

TEST_CASE("quick look: on a text page the pointer selects; a click beside it closes") {
    constexpr std::int32_t width = 1000;
    constexpr std::int32_t height = 700;
    std::unique_ptr<rummage::ui::Offscreen> gl;
    try {
        gl = std::make_unique<rummage::ui::Offscreen>(width, height);
    } catch (const std::runtime_error& error) {
        MESSAGE("no offscreen OpenGL: " << error.what());
        return;
    }
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    Fixture f;
    // Ten lines of the same words: the page's middle is on one of them.
    std::ofstream notes{f.dir.path / "notes.txt"};
    for (int i = 0; i < 10; ++i) {
        notes << "alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi\n";
    }
    notes.close();
    REQUIRE(f.settle());
    auto& look = f.browser.quick_look();
    f.press(XKB_KEY_End);
    f.press(XKB_KEY_space);
    REQUIRE(f.settle());
    REQUIRE(look.shows_text());
    const auto draw = [&] {
        renderer.begin_frame(width, height, 1.0F, {});
        f.browser.draw(renderer, {0.0F, 0.0F, width, height}, rummage::theme::Palette{}, icons,
                       Browser::Clock::now());
        renderer.end_frame();
    };
    draw();
    const auto pointer = [&](rummage::ui::PointerEvent::Kind kind, float x, float y, int clicks) {
        rummage::ui::PointerEvent event;
        event.kind = kind;
        event.x = x;
        event.y = y;
        event.button = rummage::ui::PointerButton::left;
        event.click_count = clicks;
        f.browser.pointer(event, Browser::Clock::now());
    };
    using Kind = rummage::ui::PointerEvent::Kind;
    // A double click in the middle of the page: a whole word, Quick Look still open.
    const float middle_x = width / 2.0F;
    const float middle_y = (height - 44.0F) / 2.0F;
    pointer(Kind::press, middle_x, middle_y, 2);
    pointer(Kind::release, middle_x, middle_y, 2);
    CHECK(look.is_open());
    const std::string word = look.selected_text();
    CHECK_FALSE(word.empty());
    CHECK(word.find(' ') == std::string::npos);
    // A drag down three lines: more than a line, from that word on.
    pointer(Kind::press, middle_x, middle_y, 1);
    pointer(Kind::motion, middle_x, middle_y + (3.0F * QuickLook::kTextLineHeight), 0);
    pointer(Kind::release, middle_x, middle_y + (3.0F * QuickLook::kTextLineHeight), 0);
    CHECK(std::ranges::count(look.selected_text(), '\n') == 3);
    draw();  // the selection drawn behind the text
    // Beside the page: Quick Look closes.
    pointer(Kind::press, 5.0F, 5.0F, 1);
    CHECK_FALSE(look.is_open());
}

TEST_CASE("quick look: a log opens at its end; keys and Shift+wheel move a text page") {
    constexpr std::int32_t width = 1000;
    constexpr std::int32_t height = 700;
    std::unique_ptr<rummage::ui::Offscreen> gl;
    try {
        gl = std::make_unique<rummage::ui::Offscreen>(width, height);
    } catch (const std::runtime_error& error) {
        MESSAGE("no offscreen OpenGL: " << error.what());
        return;
    }
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    Fixture f;
    // A log longer than what is read (256 KiB), lines longer than the page is wide.
    {
        std::ofstream log{f.dir.path / "z.log"};
        for (int i = 0; i < 4000; ++i) {
            log << "2026-11-09 12:00:00 line " << i << ' ' << std::string(100, 'x') << '\n';
        }
    }
    // Written after the folder was read: wait until it is listed.
    REQUIRE(run_until(f.loop, [&] {
        const auto& entries = f.browser.columns().column(0).folder().entries();
        return std::ranges::any_of(
            entries, [](const rummage::fs::Entry& entry) { return entry.name == "z.log"; });
    }));
    REQUIRE(f.settle());
    auto& look = f.browser.quick_look();
    const auto draw = [&] {
        renderer.begin_frame(width, height, 1.0F, {});
        f.browser.draw(renderer, {0.0F, 0.0F, width, height}, rummage::theme::Palette{}, icons,
                       Browser::Clock::now());
        renderer.end_frame();
    };
    f.press(XKB_KEY_End);  // z.log
    f.press(XKB_KEY_space);
    REQUIRE(f.settle());
    REQUIRE(look.shows_text());
    const auto* text = std::get_if<rummage::preview::Text>(look.content());
    REQUIRE(text != nullptr);
    CHECK(text->tail);
    CHECK(text->lines.back().starts_with("2026-11-09 12:00:00 line 3999 "));
    draw();
    auto& down = look.text_scroller();
    CHECK(down.offset() == down.max_offset());  // its newest lines in sight
    CHECK(down.max_offset() > 0.0F);
    // Home and End reach the top and the bottom; Page Down goes down a page.
    // Scrolling glides in time: draw until it has arrived.
    const auto settle_scroll = [&] {
        const auto until = Browser::Clock::now() + 2s;
        while (look.animating(Browser::Clock::now()) && Browser::Clock::now() < until) {
            draw();
        }
        draw();
    };
    f.press(XKB_KEY_Home);
    settle_scroll();
    CHECK(down.offset() == 0.0F);
    f.press(XKB_KEY_Page_Down);
    settle_scroll();
    CHECK(down.offset() > 0.0F);
    CHECK(down.offset() < down.max_offset());
    f.press(XKB_KEY_End);
    settle_scroll();
    CHECK(std::abs(down.offset() - down.max_offset()) < 1.0F);  // a glide stops within a pixel
    CHECK(look.shown() == "z.log");  // the keys moved the page, not the folder's cursor
    // Shift+wheel: sideways, the lines being wider than the page.
    auto& sideways = look.text_sideways();
    CHECK(sideways.max_offset() > 0.0F);
    rummage::ui::PointerEvent wheel;
    wheel.kind = rummage::ui::PointerEvent::Kind::scroll;
    wheel.x = width / 2.0F;
    wheel.y = height / 2.0F;
    wheel.wheel_y = 2.0F;
    wheel.modifiers = {.shift = true};
    f.browser.pointer(wheel, Browser::Clock::now());
    settle_scroll();
    CHECK(sideways.offset() > 0.0F);
    // The next file starts at its left and top again.
    f.press(XKB_KEY_Up);
    REQUIRE(f.settle());
    draw();
    CHECK(sideways.offset() == 0.0F);
}

TEST_CASE("quick look: Details follows the file and exposes EXIF without stale metadata") {
    Fixture f;
    fs::copy_file(kData / "shot.jpg", f.dir.path / "photo.jpg");
    REQUIRE(run_until(f.loop, [&] {
        const auto& entries = f.browser.columns().column(0).folder().entries();
        return std::ranges::any_of(entries,
                                   [](const auto& entry) { return entry.name == "photo.jpg"; });
    }));
    auto& folder = f.browser.columns().column(0).folder();
    folder.select_name("photo.jpg");
    f.press(XKB_KEY_space);
    auto& look = f.browser.quick_look();
    f.press(XKB_KEY_i);
    REQUIRE(look.details_shown());
    REQUIRE(run_until(f.loop, [&] { return look.summary() != nullptr && !look.loading(); }));
    REQUIRE(look.summary()->photo);
    CHECK_FALSE(look.summary()->photo->camera.empty());
    CHECK_FALSE(look.summary()->photo->exposure_time.empty());
    folder.select_name("notes.txt");
    look.follow(f.browser.columns().column(0).shared_folder());
    CHECK(look.summary() == nullptr);
    REQUIRE(run_until(f.loop, [&] { return look.summary() != nullptr && !look.loading(); }));
    CHECK_FALSE(look.summary()->photo);
    f.press(XKB_KEY_i);
    CHECK_FALSE(look.details_shown());
    look.close();
    CHECK(look.summary() == nullptr);
}

TEST_CASE("quick look: Details button stays inside a narrow scaled window") {
    rummage::ui::Offscreen gl{700, 600};
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    Fixture f;
    REQUIRE(f.settle());
    f.browser.columns().column(0).folder().select_name("b.jpg");
    f.press(XKB_KEY_space);
    auto& look = f.browser.quick_look();
    REQUIRE(run_until(f.loop, [&] { return !look.loading(); }));
    for (const float zoom : {1.0F, 1.5F, 2.0F}) {
        renderer.begin_frame(700, 600, 1, {});
        renderer.set_zoom(zoom);
        look.draw(renderer, {0, 0, 700 / zoom, 600 / zoom}, rummage::theme::Palette{}, icons,
                  QuickLook::Clock::now());
        renderer.end_frame();
        const auto button = look.details_button();
        CHECK(button.right() <= 700 / zoom);
        CHECK(button.x >= 0);
        const bool before = look.details_shown();
        CHECK(look.pointer({.kind = rummage::ui::PointerEvent::Kind::press,
                            .x = button.x + 4,
                            .y = button.y + 4,
                            .button = rummage::ui::PointerButton::left},
                           QuickLook::Clock::now()));
        CHECK(look.details_shown() != before);
        CHECK(look.is_open());
        REQUIRE(run_until(f.loop, [&] { return !look.loading(); }));
    }
}

TEST_CASE("quick look: upright thumbnail and reduced sharp frame keep size and opacity") {
    namespace picture = rummage::picture;
    namespace thumbs = rummage::thumbs;
    for (const float scale : {1.0F, 2.0F}) {
        INFO(scale);

        struct RestoreLoader {
            ~RestoreLoader() {
                picture::set_loader([](const picture::LoadRequest& request) {
                    return rummage::formats::image::load(request);
                });
            }
        } restore;

        const auto white = [](std::uint32_t width, std::uint32_t height) {
            picture::Image image{
                .width = width,
                .height = height,
                .rgba = std::vector<std::uint8_t>(std::size_t{width} * height * 4, 255)};
            for (std::size_t i = 3; i < image.rgba.size(); i += 4)
                image.rgba[i] = 128;
            return image;
        };
        const auto write = [](const fs::path& path, const auto& bytes) {
            fs::create_directories(path.parent_path());
            std::ofstream out{path, std::ios::binary};
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
        };
        rummage::test::TempDir dir;
        const auto photo = dir.path / "photos" / "portrait.png";
        write(photo, picture::encode_png(white(320, 480)));
        auto cache = std::make_shared<thumbs::Cache>(dir.path / "cache", "rummage-test");
        struct stat info{};
        REQUIRE(::stat(photo.c_str(), &info) == 0);
        write(cache->path_for(photo, thumbs::Size::large),
              picture::encode_png(white(171, 256), {{"Thumb::URI", thumbs::file_uri(photo)},
                                                    {"Thumb::MTime", std::to_string(info.st_mtime)},
                                                    {"Thumb::Image::Width", "480"},
                                                    {"Thumb::Image::Height", "320"}}));
        EventLoop loop;
        WorkerPool pool{loop, 2};
        // Destroying the promise also releases a blocked load if an assertion fails.
        std::promise<void> release_sharp;
        picture::set_loader([ready = release_sharp.get_future().share(),
                             white](const picture::LoadRequest&) {
            ready.wait();
            picture::Loaded loaded;
            loaded.image = white(160, 240);  // like a reduced video frame or embedded raw preview
            loaded.full_width = 320;
            loaded.full_height = 480;
            return loaded;
        });
        rummage::fs::DirectoryWatcher watcher{loop};
        auto mime = rummage::fs::MimeDatabase::load();
        auto folder = std::make_shared<rummage::views::Folder>(loop, pool, watcher, mime,
                                                               photo.parent_path(), [] {});
        REQUIRE(run_until(loop, [&] { return folder->entries().size() == 1; }));
        folder->sniff(0, 1);
        REQUIRE(run_until(loop, [&] {
            return folder->mime_of(folder->entries().front()) != rummage::fs::kUnknownMime;
        }));
        folder->select_name("portrait.png");
        rummage::ui::Offscreen surface{800, 900};
        rummage::ui::FontSystem fonts;
        rummage::ui::Renderer renderer{fonts};
        rummage::ui::IconSet icons{fonts};
        QuickLook look{pool, mime, cache, [] {}};
        const auto now = QuickLook::Clock::now();
        look.open(folder, now - 1s);
        const auto draw = [&](QuickLook::Clock::time_point at) {
            renderer.begin_frame(800, 900, scale, {});
            look.draw(renderer, {0, 0, 800 / scale, 900 / scale}, rummage::theme::Palette{}, icons,
                      at);
            renderer.end_frame();
            std::vector<unsigned char> pixels(800 * 900 * 4);
            glReadPixels(0, 0, 800, 900, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            return pixels;
        };
        // Image is white; UI text and shadows are outside this central scan line.
        const auto span = [](const auto& pixels) {
            int first = 800, last = -1;
            for (int x = 0; x < 800; ++x) {
                if (pixels[(450U * 800U + static_cast<unsigned>(x)) * 4U] > 100) {
                    first = std::min(first, x);
                    last = x;
                }
            }
            return last - first + 1;
        };
        // The loading icon also intersects the scan line (80/160 px). Wait for
        // the white image at its centre before measuring the thumbnail's span.
        const auto center = (450U * 800U + 400U) * 4U;
        REQUIRE(run_until(loop, [&] { return draw(now)[center] > 100; }));
        const auto thumbnail = draw(now);
        CHECK(span(thumbnail) == 320);
        REQUIRE(look.content() == nullptr);
        release_sharp.set_value();
        REQUIRE(run_until(loop, [&] { return !look.loading(); }));
        for (const auto offset : {0ms, 25ms, 100ms}) {
            const auto sharp = draw(now + offset);
            CHECK(span(sharp) == 320);
            CHECK(sharp[center] == thumbnail[center]);
        }
    }
}

TEST_CASE(
    "quick look: offline thumbnail latency starts at its request and changed files preview "
    "afresh") {
    rummage::test::TempDir dir;
    const auto photo = dir.path / "portrait.png";
    fs::copy_file(kData / "blocks.png", photo);
    auto cache = std::make_shared<rummage::thumbs::Cache>(dir.path / "cache", "rummage-test");
    REQUIRE_FALSE(cache->get(photo, rummage::thumbs::Size::large, "image/png").image.empty());
    struct stat info{};
    REQUIRE(::stat(photo.c_str(), &info) == 0);
    fs::remove(photo);

    EventLoop loop;
    WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    auto mime = rummage::fs::MimeDatabase::load();
    auto folder = std::make_shared<rummage::views::Folder>(loop, pool, watcher, mime, [] {});
    rummage::fs::Entry entry;
    entry.name = "portrait.png";
    entry.kind = rummage::fs::EntryKind::file;
    entry.modified_ns =
        static_cast<std::int64_t>(info.st_mtim.tv_sec) * 1000000000 + info.st_mtim.tv_nsec;
    entry.mime = mime->find("image/png");
    entry.identity = "photo:1";
    entry.location = photo;
    entry.available = false;
    folder->replace_collection({entry}, "Photos", 1);
    folder->select_name(entry.key());

    rummage::ui::Offscreen surface{800, 600};
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    QuickLook look{pool, mime, cache, [] {}};
    const auto requested = QuickLook::Clock::now();
    look.open(folder, requested - 1s);
    const auto draw = [&] {
        renderer.begin_frame(800, 600, 1, {});
        look.draw(renderer, {0, 0, 800, 600}, rummage::theme::Palette{}, icons,
                  QuickLook::Clock::now());
        renderer.end_frame();
    };

    const auto log_path = dir.path / "quick-look.log";

    // Capture the public diagnostic rather than expose private slot timestamps just for testing.
    struct CaptureLog {
        FILE* file;
        int saved;
        rummage::log::Level level{rummage::log::level()};

        ~CaptureLog() {
            std::fflush(stderr);
            if (saved >= 0) {
                ::dup2(saved, STDERR_FILENO);
                ::close(saved);
            }
            if (file)
                std::fclose(file);
            rummage::log::set_level(level);
        }
    } captured{std::fopen(log_path.c_str(), "w"), ::dup(STDERR_FILENO)};

    REQUIRE(captured.file != nullptr);
    REQUIRE(captured.saved >= 0);
    REQUIRE(::dup2(::fileno(captured.file), STDERR_FILENO) >= 0);
    rummage::log::set_level(rummage::log::Level::debug);
    std::string output;
    constexpr std::string_view marker = "thumbnail on screen ";
    REQUIRE(run_until(loop, [&] {
        draw();
        std::ifstream input{log_path};
        output.assign(std::istreambuf_iterator<char>{input}, {});
        return output.find(marker) != std::string::npos;
    }));
    const auto start = output.find(marker) + marker.size();
    const double latency = std::stod(output.substr(start));
    const double elapsed =
        std::chrono::duration<double, std::milli>(QuickLook::Clock::now() - requested).count();
    CHECK(latency >= 0);
    CHECK(latency <= elapsed + 1);
    CHECK(look.content() == nullptr);
    CHECK_FALSE(fs::exists(photo));

    // A changed identity can preview the current file, but remains unavailable for file actions.
    fs::copy_file(kData / "blocks.png", photo);
    entry.preview_current_location = true;
    folder->replace_collection({entry}, "Photos", 1);
    look.follow(folder);
    REQUIRE(run_until(loop, [&] { return look.content() != nullptr && !look.loading(); }));
    REQUIRE(look.image_size());
    CHECK(look.image_size() == Fixture::Size{{48, 32}});
    CHECK_FALSE(folder->cursor_entry()->available);
    draw();

    // Returning to offline mode invalidates the readable slot, even with the same path/mtime.
    entry.preview_current_location = false;
    folder->replace_collection({entry}, "Photos", 1);
    look.follow(folder);
    CHECK(look.content() == nullptr);
}

TEST_CASE("quick look: PDF page requests, boundaries, zoom and cancelled navigation") {
    namespace picture = rummage::picture;
    namespace engines = rummage::engines;
    const auto original = engines::installed();

    struct Restore {
        std::shared_ptr<const engines::Registry> registry;

        ~Restore() {
            engines::set_installed(registry);
            picture::set_loader([](const picture::LoadRequest& request) {
                return rummage::formats::image::load(request);
            });
        }
    } restore{original};

    auto registry = std::make_shared<engines::Registry>(*original);
    REQUIRE(registry->add({.manifest = {.name = "pdf",
                                        .program = "rummage-pdf",
                                        .families = {{.name = "pdf",
                                                      .shows = engines::Shows::picture,
                                                      .mime = {"application/pdf"}}}},
                           .program = "rummage-pdf",
                           .manifest_path = {}}));
    engines::set_installed(registry);
    picture::set_loader([](const picture::LoadRequest& request) {
        picture::Loaded result;
        result.image = {.width = 64 + request.page,
                        .height = 32,
                        .rgba = std::vector<std::uint8_t>((64 + request.page) * 32 * 4, 255)};
        result.pages = 3;
        return result;
    });
    rummage::test::TempDir dir;
    dir.write("paper.pdf", "%PDF-1.4");
    EventLoop loop;
    WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    const auto mime = rummage::fs::MimeDatabase::load();
    auto folder =
        std::make_shared<rummage::views::Folder>(loop, pool, watcher, mime, dir.path, [] {});
    REQUIRE(run_until(loop, [&] { return folder->entries().size() == 1; }));
    folder->sniff(0, 1);
    REQUIRE(run_until(loop, [&] {
        return mime->name(folder->mime_of(folder->entries()[0])) == "application/pdf";
    }));
    folder->select_name("paper.pdf");
    QuickLook look{pool, mime, {}, [] {}};
    look.open(folder, QuickLook::Clock::now());
    REQUIRE(run_until(loop, [&] { return !look.loading(); }));
    CHECK(look.document_page() == 0);
    CHECK(look.document_pages() == 3);
    look.set_document_page(1);
    look.set_document_page(2);
    look.set_document_page(1);  // old requests must never replace the latest page
    REQUIRE(run_until(loop, [&] { return !look.loading(); }));
    REQUIRE(look.image_size());
    CHECK(look.image_size()->first == 65);
    look.set_document_page(3);
    CHECK(look.document_page() == 1);
    const auto key = [&](xkb_keysym_t symbol) {
        rummage::ui::KeyEvent event;
        event.keysym = symbol;
        return look.text_key(event, QuickLook::Clock::now());
    };
    CHECK(key(XKB_KEY_End));
    CHECK(look.document_page() == 2);
    CHECK(key(XKB_KEY_Page_Down));
    CHECK(look.document_page() == 2);
    CHECK(key(XKB_KEY_Home));
    CHECK(look.document_page() == 0);
    for (int i = 0; i < 20; ++i)
        CHECK(key(XKB_KEY_equal));
    CHECK(look.document_zoom() == 4);
    CHECK(key(XKB_KEY_0));
    CHECK(look.document_zoom() == 1);
    CHECK(key(XKB_KEY_minus));
    CHECK(look.document_zoom() == 1);
    look.close();
    CHECK_FALSE(look.loading());
}

TEST_CASE("quick look: media controls keep navigation keys and release playback on file change") {
    const auto original = rummage::engines::installed();

    struct RestorePlaybackRegistry {
        std::shared_ptr<const rummage::engines::Registry> previous;

        ~RestorePlaybackRegistry() { rummage::engines::set_installed(previous); }
    } restore{original};

    auto registry = std::make_shared<rummage::engines::Registry>();
    const auto media = fs::path{RUMMAGE_EXECUTABLE}.parent_path() / "rummage-media";
    REQUIRE(
        registry->add({.manifest = {.name = "media", .program = "rummage-media", .families = {}},
                       .program = media,
                       .manifest_path = {}}));
    rummage::engines::set_installed(registry);
    rummage::test::TempDir dir;
    fs::copy_file(fs::path{RUMMAGE_TEST_DATA} / "media" / "metadata.mp4", dir.path / "clip.mp4");
    dir.write("notes.txt", "notes");
    EventLoop loop;
    WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    const auto mime = rummage::fs::MimeDatabase::load();
    auto folder =
        std::make_shared<rummage::views::Folder>(loop, pool, watcher, mime, dir.path, [] {});
    REQUIRE(run_until(loop, [&] { return folder->entries().size() == 2; }));
    folder->sniff(0, 2);
    REQUIRE(run_until(loop, [&] {
        return std::ranges::all_of(folder->entries(), [&](const auto& entry) {
            return folder->mime_of(entry) != rummage::fs::kUnknownMime;
        });
    }));
    folder->select_name("clip.mp4");
    QuickLook look{pool, mime, {}, [] {}};
    look.open(folder, QuickLook::Clock::now() - 1s);
    REQUIRE(look.playback());
    REQUIRE(run_until(loop, [&] { return !look.playback()->snapshot().loading; }));
    const auto key = [&](xkb_keysym_t symbol) {
        rummage::ui::KeyEvent event;
        event.keysym = symbol;
        return look.text_key(event, QuickLook::Clock::now());
    };
    CHECK(key(XKB_KEY_minus));
    CHECK(look.playback()->volume() == 70);
    CHECK(key(XKB_KEY_equal));
    CHECK(look.playback()->volume() == 80);
    CHECK(key(XKB_KEY_j));
    CHECK(key(XKB_KEY_l));
    for (int i = 0; i < 8; ++i)
        CHECK(key(XKB_KEY_minus));
    CHECK(key(XKB_KEY_p));
    CHECK(key(XKB_KEY_p));
    CHECK(key(XKB_KEY_Home));
    REQUIRE(run_until(loop, [&] { return !look.playback()->snapshot().loading; }));
    for (int i = 0; i < 8; ++i)
        CHECK(key(XKB_KEY_equal));
    if (fs::exists(media))
        REQUIRE(look.playback()->snapshot().image);
    CHECK_FALSE(key(XKB_KEY_Left));
    CHECK_FALSE(key(XKB_KEY_Right));
    CHECK_FALSE(key(XKB_KEY_space));
    rummage::ui::Offscreen surface{800, 600};
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    renderer.begin_frame(800, 600, 1, {});
    look.draw(renderer, {0, 0, 800, 600}, rummage::theme::Palette{}, icons,
              QuickLook::Clock::now());
    renderer.end_frame();
    if (const char* output = std::getenv("RUMMAGE_PLAYBACK_SNAPSHOT")) {
        rummage::picture::Image capture{
            .width = 800, .height = 600, .rgba = std::vector<std::uint8_t>(800 * 600 * 4)};
        std::vector<std::uint8_t> pixels(capture.rgba.size());
        glReadPixels(0, 0, 800, 600, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        for (std::size_t row = 0; row < 600; ++row)
            std::copy_n(pixels.data() + (599 - row) * 800 * 4, 800 * 4,
                        capture.rgba.data() + row * 800 * 4);
        const auto png = rummage::picture::encode_png(capture);
        std::ofstream file{output, std::ios::binary};
        file.write(reinterpret_cast<const char*>(png.data()),
                   static_cast<std::streamsize>(png.size()));
    }
    folder->select_name("notes.txt");
    look.follow(folder);
    CHECK(look.playback() == nullptr);
    folder->select_name("clip.mp4");
    look.follow(folder);
    REQUIRE(look.playback());
    look.close();
    CHECK(look.playback() == nullptr);
}
