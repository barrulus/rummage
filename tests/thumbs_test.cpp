#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <thread>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/formats/image/load.hpp"
#include "rummage/fs/entry.hpp"
#include "rummage/picture/load.hpp"
#include "rummage/picture/png.hpp"
#include "rummage/thumbs/cache.hpp"
#include "rummage/thumbs/md5.hpp"
#include "rummage/ui/offscreen.hpp"
#include "rummage/views/thumbnails.hpp"
#include "temp_dir.hpp"

namespace thumbs = rummage::thumbs;
namespace fs = std::filesystem;
using rummage::test::TempDir;

namespace {

const fs::path kData = fs::path{RUMMAGE_TEST_DATA} / "image";

int mode_of(const fs::path& path) {
    struct stat info{};
    REQUIRE(::stat(path.c_str(), &info) == 0);
    return static_cast<int>(info.st_mode & 0777);
}

}  // namespace

TEST_CASE("thumbs: MD5 of the RFC 1321 test suite") {
    CHECK(thumbs::md5_hex("") == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(thumbs::md5_hex("a") == "0cc175b9c0f1b6a831c399e269772661");
    CHECK(thumbs::md5_hex("abc") == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(thumbs::md5_hex("message digest") == "f96b697d7cb7938d525a2f31aaf161d0");
    CHECK(thumbs::md5_hex("abcdefghijklmnopqrstuvwxyz") == "c3fcd3d76192e4007dfb496cca67e13b");
    CHECK(
        thumbs::md5_hex("12345678901234567890123456789012345678901234567890123456789012345678901234"
                        "567890") == "57edf4a22be3c955ac49da2e2107b67a");
}

TEST_CASE("thumbs: file URIs are escaped exactly as GLib does") {
    // Expected values from `gio info` (GLib's g_filename_to_uri).
    CHECK(thumbs::file_uri("/d/plain.jpg") == "file:///d/plain.jpg");
    CHECK(thumbs::file_uri("/d/spa ce.jpg") == "file:///d/spa%20ce.jpg");
    CHECK(thumbs::file_uri("/d/semi;colon.jpg") == "file:///d/semi%3Bcolon.jpg");
    CHECK(thumbs::file_uri("/d/excl!amp&'()*+,=:@$~_-.jpg") ==
          "file:///d/excl!amp&'()*+,=:@$~_-.jpg");
    CHECK(thumbs::file_uri("/d/pct%hash#q?.jpg") == "file:///d/pct%25hash%23q%3F.jpg");
    CHECK(thumbs::file_uri("/d/accent\xc3\xb2\xe2\x82\xac.jpg") ==
          "file:///d/accent%C3%B2%E2%82%AC.jpg");
    CHECK(thumbs::file_uri("/d/brack[et]{x}^|`\"<>.jpg") ==
          "file:///d/brack%5Bet%5D%7Bx%7D%5E%7C%60%22%3C%3E.jpg");
    // The spec's own example: the thumbnail name is the MD5 of the URI.
    CHECK(thumbs::md5_hex("file:///home/jens/photos/me.png") == "c6ee772d9e49320e97ec29a7eb5b1697");
}

TEST_CASE("thumbs: sizes") {
    CHECK(thumbs::size_for(1) == thumbs::Size::normal);
    CHECK(thumbs::size_for(128) == thumbs::Size::normal);
    CHECK(thumbs::size_for(129) == thumbs::Size::large);
    CHECK(thumbs::size_for(384) == thumbs::Size::x_large);
    CHECK(thumbs::size_for(5000) == thumbs::Size::xx_large);
    CHECK(thumbs::directory_name(thumbs::Size::x_large) == "x-large");
}

TEST_CASE("thumbs: made once, then read from the cache until the file changes") {
    TempDir dir;
    const fs::path photo = dir.path / "photo one.jpg";
    fs::copy_file(kData / "with-thumbnail.jpg", photo);
    const thumbs::Cache cache{dir.path / "thumbnails", "rummage-test"};

    // 480x320 with a 192x128 EXIF thumbnail: enough for 128, so the thumbnail comes from it,
    // scaled to fit, then turned upright (orientation 6).
    const thumbs::Thumbnail made = cache.get(photo, thumbs::Size::normal);
    REQUIRE(made.source == thumbs::Source::preview);
    CHECK(made.image.width == 85);
    CHECK(made.image.height == 128);
    const fs::path stored = cache.path_for(photo, thumbs::Size::normal);
    CHECK(stored ==
          dir.path / "thumbnails" / "normal" / (thumbs::md5_hex(thumbs::file_uri(photo)) + ".png"));
    REQUIRE(fs::exists(stored));
    CHECK(mode_of(stored) == 0600);
    CHECK(mode_of(stored.parent_path()) == 0700);
    CHECK(mode_of(dir.path / "thumbnails") == 0700);

    const thumbs::Thumbnail cached = cache.get(photo, thumbs::Size::normal);
    CHECK(cached.source == thumbs::Source::cache);
    CHECK(cached.image.rgba == made.image.rgba);
    // The whole image's size, upright, stored with it (Thumb::Image::Width/Height): Quick Look
    // shows the thumbnail where the sharp image will land.
    CHECK(made.full_width == 320);
    CHECK(made.full_height == 480);
    CHECK(cached.full_width == 320);
    CHECK(cached.full_height == 480);

    // Changed: the stored one no longer matches its Thumb::MTime.
    fs::last_write_time(photo, fs::last_write_time(photo) + std::chrono::seconds{10});
    CHECK(cache.get(photo, thumbs::Size::normal).source == thumbs::Source::preview);
    CHECK(cache.get(photo, thumbs::Size::normal).source == thumbs::Source::cache);

    // At 256 the embedded 192x128 is too small: the whole image, upright.
    const thumbs::Thumbnail large = cache.get(photo, thumbs::Size::large);
    CHECK(large.source == thumbs::Source::decoded);
    CHECK(large.image.width == 171);
    CHECK(large.image.height == 256);
    CHECK(large.full_width == 320);
    CHECK(large.full_height == 480);
}

TEST_CASE("thumbs: shared-cache dimensions follow upright pixels or stay unknown") {
    TempDir dir;
    const fs::path photo = dir.path / "photo.jpg";
    fs::copy_file(kData / "with-thumbnail.jpg", photo);
    const thumbs::Cache cache{dir.path / "thumbnails", "rummage-test"};
    const auto target = cache.path_for(photo, thumbs::Size::large);
    fs::create_directories(target.parent_path());
    struct stat info{};
    REQUIRE(::stat(photo.c_str(), &info) == 0);
    const rummage::picture::Image image{
        .width = 171, .height = 256, .rgba = std::vector<std::uint8_t>(171 * 256 * 4, 255)};
    const auto check = [&](const std::string& width, const std::string& height,
                           std::uint32_t expected_width, std::uint32_t expected_height) {
        rummage::picture::PngText text{{"Thumb::URI", thumbs::file_uri(photo)},
                                       {"Thumb::MTime", std::to_string(info.st_mtime)}};
        if (!width.empty())
            text.emplace_back("Thumb::Image::Width", width);
        if (!height.empty())
            text.emplace_back("Thumb::Image::Height", height);
        const auto png = rummage::picture::encode_png(image, text);
        {
            std::ofstream out{target, std::ios::binary};
            out.write(reinterpret_cast<const char*>(png.data()),
                      static_cast<std::streamsize>(png.size()));
        }
        const auto cached = cache.get(photo, thumbs::Size::large);
        REQUIRE(cached.source == thumbs::Source::cache);
        CHECK(cached.full_width == expected_width);
        CHECK(cached.full_height == expected_height);
        CHECK(cached.image.rgba == image.rgba);
    };
    check("480", "320", 320, 480);  // gdk-pixbuf: original dimensions, rotated pixels
    check("320", "480", 320, 480);  // already upright; keep rounding in 171 x 256
    check("", "", 0, 0);            // older thumbnails
    check("480", "", 0, 0);
    check("0", "480", 0, 0);
    check("-320", "480", 0, 0);
    check("4294967296", "480", 0, 0);
    check("320px", "480", 0, 0);
    check("480", "480", 0, 0);  // a crop/padded thumbnail cannot establish orientation
}

TEST_CASE("thumbs: offline catalog reads keep cached images without opening originals") {
    TempDir dir;
    const fs::path photo = dir.path / "photo.jpg";
    fs::copy_file(kData / "with-thumbnail.jpg", photo);
    rummage::picture::set_loader([](const rummage::picture::LoadRequest& request) {
        return rummage::formats::image::load(request);
    });
    const thumbs::Cache cache{dir.path / "thumbnails", "rummage-test"};
    const auto made = cache.get(photo, thumbs::Size::normal);
    REQUIRE_FALSE(made.image.empty());
    struct stat info{};
    REQUIRE(::stat(photo.c_str(), &info) == 0);
    fs::rename(photo, dir.path / "disconnected.jpg");
    const auto offline = cache.cached(photo, thumbs::Size::normal, info.st_mtime);
    CHECK(offline.source == thumbs::Source::cache);
    CHECK(offline.image.rgba == made.image.rgba);
    CHECK(cache.cached(photo, thumbs::Size::normal, info.st_mtime + 1).image.empty());
    fs::remove(cache.path_for(photo, thumbs::Size::normal));
    CHECK(cache.cached(photo, thumbs::Size::normal, info.st_mtime).image.empty());
    CHECK_FALSE(fs::exists(photo));
}

TEST_CASE("thumbs: larger cached tiers fit the requested size without reopening originals") {
    TempDir dir;
    const fs::path photo = dir.path / "photo.jpg";
    fs::copy_file(kData / "with-thumbnail.jpg", photo);
    const thumbs::Cache cache{dir.path / "thumbnails", "rummage-test"};
    const auto large = cache.get(photo, thumbs::Size::large);
    REQUIRE_FALSE(large.image.empty());
    struct stat info{};
    REQUIRE(::stat(photo.c_str(), &info) == 0);
    fs::rename(photo, dir.path / "offline.jpg");
    const auto small = cache.cached(photo, thumbs::Size::normal, info.st_mtime);
    CHECK(small.source == thumbs::Source::cache);
    CHECK(small.image.width == 86);
    CHECK(small.image.height == 128);
    CHECK(small.full_width == 320);
    CHECK(small.full_height == 480);
    CHECK_FALSE(fs::exists(photo));
    CHECK(cache.cached(photo, thumbs::Size::normal, info.st_mtime + 1).image.empty());
    // Bigger is useful; smaller must never be substituted for the requested quality.
    CHECK(cache.cached(photo, thumbs::Size::x_large, info.st_mtime).image.empty());

    const auto other = dir.path / "different.jpg";
    fs::copy_file(cache.path_for(photo, thumbs::Size::large),
                  cache.path_for(other, thumbs::Size::large));
    CHECK(cache.cached(other, thumbs::Size::normal, info.st_mtime).image.empty());
}

TEST_CASE("thumbs: a failure is remembered until the file changes, make() tries again") {
    TempDir dir;
    const fs::path broken = dir.path / "broken.jpg";
    fs::copy_file(kData / "truncated.jpg", broken);
    const thumbs::Cache cache{dir.path / "thumbnails", "rummage-test"};

    const thumbs::Thumbnail first = cache.get(broken, thumbs::Size::large);
    CHECK(first.source == thumbs::Source::failed);
    CHECK(first.image.empty());
    CHECK(fs::exists(dir.path / "thumbnails" / "fail" / "rummage-test" /
                     (thumbs::md5_hex(thumbs::file_uri(broken)) + ".png")));
    CHECK(cache.get(broken, thumbs::Size::large).error == "failed before, and unchanged since");
    CHECK(cache.make(broken, thumbs::Size::large).error != "failed before, and unchanged since");

    // Fixed: a good image under the same name.
    fs::copy_file(kData / "blocks.jpg", broken, fs::copy_options::overwrite_existing);
    fs::last_write_time(broken, fs::last_write_time(broken) + std::chrono::seconds{10});
    CHECK(cache.get(broken, thumbs::Size::large).source == thumbs::Source::decoded);

    CHECK(cache.get(dir.path / "missing.jpg", thumbs::Size::large).source ==
          thumbs::Source::failed);
}

TEST_CASE("thumbs: the cache never thumbnails its own files") {
    TempDir dir;
    const thumbs::Cache cache{dir.path / "thumbnails", "rummage-test"};
    const fs::path photo = dir.path / "a.png";
    fs::copy_file(kData / "blocks.png", photo);
    REQUIRE(cache.get(photo, thumbs::Size::normal).source == thumbs::Source::decoded);
    const thumbs::Thumbnail own =
        cache.get(cache.path_for(photo, thumbs::Size::normal), thumbs::Size::normal);
    CHECK(own.source == thumbs::Source::failed);
}

namespace {

using namespace std::chrono_literals;
namespace picture = rummage::picture;
namespace thumbs = rummage::thumbs;

struct RestoreLoader {
    ~RestoreLoader() {
        picture::set_loader([](const picture::LoadRequest& request) {
            return rummage::formats::image::load(request);
        });
    }
};

struct Fixture {
    rummage::test::TempDir dir;
    std::filesystem::path file{dir.write("photo.heic", "injected picture loader")};
    std::atomic<unsigned> calls{0};
    std::atomic<unsigned> failures{1};
    picture::Failure reason{picture::Failure::unavailable};
    unsigned redraws{0};
    RestoreLoader restore;  // restored after the workers have joined
    rummage::core::EventLoop loop;
    rummage::core::WorkerPool pool{loop, 1};
    std::shared_ptr<thumbs::Cache> cache{
        std::make_shared<thumbs::Cache>(dir.path / "cache", "test")};
    std::unique_ptr<rummage::views::Thumbnails> view{
        std::make_unique<rummage::views::Thumbnails>(loop, pool, cache, [this] { ++redraws; })};

    Fixture() {
        picture::set_loader([this](const picture::LoadRequest&) {
            picture::Loaded result;
            if (++calls <= failures.load()) {
                result.failure = reason;
                result.detail = "injected failure";
            } else {
                result.image = {.width = 1, .height = 1, .rgba = {255, 0, 0, 255}};
            }
            return result;
        });
    }

    // Successful pixels are inspected before GPU upload, so no GL context is needed.
    void ask(thumbs::Size size = thumbs::Size::large) {
        static_cast<void>(view->get(file, 1, size, "image/heic"));
    }

    bool until(const std::function<bool()>& done) {
        bool reached = false;
        const auto poll = loop.add_timer(1ms, 1ms, [&] {
            if (done()) {
                reached = true;
                loop.quit();
            }
        });
        const auto watchdog = loop.add_timer(5s, 0ms, [&] { loop.quit(); });
        loop.run();
        loop.remove(poll);
        loop.remove(watchdog);
        return reached;
    }

    void wait(std::chrono::milliseconds duration) {
        loop.add_timer(duration, 0ms, [this] { loop.quit(); });
        loop.run();
    }

    void leave() {
        view->end_frame();
        view->end_frame();  // no request in this frame
    }
};

}  // namespace

TEST_CASE("thumbnails: a transient failure recovers without another frame") {
    Fixture f;
    f.ask();
    REQUIRE(f.until([&] { return f.redraws > 0; }));
    CHECK(f.calls == 1);
    CHECK(f.view->loading());  // the delayed retry is outstanding
    for (int i = 0; i < 100; ++i) {
        f.ask();
        f.view->end_frame();
    }
    CHECK(f.calls == 1);  // redraws cannot bypass the backoff
    REQUIRE(f.until([&] { return !f.view->loading(); }));
    CHECK(f.calls == 2);
    CHECK(f.view->usage().pixel_bytes == 4);
    CHECK(f.cache->get(f.file, thumbs::Size::large, "image/heic").source == thumbs::Source::cache);
}

TEST_CASE("thumbnails: retries are bounded and returning to the file starts afresh") {
    Fixture f;
    f.failures = 100;
    f.ask();
    REQUIRE(f.until([&] { return !f.view->loading(); }));
    CHECK(f.calls == 3);
    for (int i = 0; i < 100; ++i) {
        f.ask();
        f.view->end_frame();
    }
    f.wait(300ms);
    CHECK(f.calls == 3);
    CHECK_FALSE(f.view->loading());

    f.leave();
    CHECK(f.view->usage().slots == 0);
    f.failures = 0;
    f.ask();
    REQUIRE(f.until([&] { return !f.view->loading(); }));
    CHECK(f.calls == 4);
    CHECK(f.view->usage().pixel_bytes == 4);
}

TEST_CASE("thumbnails: permanent failures are not retried") {
    Fixture f;
    f.failures = 100;
    f.reason = picture::Failure::damaged;
    f.ask();
    REQUIRE(f.until([&] { return !f.view->loading(); }));
    f.leave();
    CHECK(f.view->usage().slots == 1);
    f.ask();
    f.wait(300ms);
    CHECK(f.calls == 1);
    CHECK_FALSE(f.view->loading());
    picture::Failure reason{};
    static_cast<void>(f.view->get(f.file, 1, thumbs::Size::large, "image/heic", &reason));
    CHECK(reason == picture::Failure::damaged);
}

TEST_CASE("thumbnails: leaving cancels a retry without affecting a replacement slot") {
    Fixture f;
    f.ask();
    REQUIRE(f.until([&] { return f.redraws > 0; }));
    REQUIRE(f.view->loading());
    f.leave();
    CHECK_FALSE(f.view->loading());
    CHECK(f.view->usage().slots == 0);
    f.ask();
    REQUIRE(f.until([&] { return !f.view->loading(); }));
    const auto redraws = f.redraws;
    f.wait(350ms);  // the removed slot's timer must not touch its replacement
    CHECK(f.calls == 2);
    CHECK(f.redraws == redraws);
    CHECK_FALSE(f.view->loading());
    CHECK(f.view->usage().pixel_bytes == 4);
}

TEST_CASE("thumbnails: destruction cancels pending retries") {
    Fixture f;
    f.ask();
    REQUIRE(f.until([&] { return f.redraws > 0; }));
    REQUIRE(f.view->loading());
    f.view.reset();
    const auto redraws = f.redraws;
    f.wait(350ms);
    CHECK(f.calls == 1);
    CHECK(f.redraws == redraws);
}

TEST_CASE("thumbnails: another size succeeding does not strand a failed size") {
    Fixture f;
    f.ask();
    REQUIRE(f.until([&] { return f.redraws > 0; }));
    f.ask(thumbs::Size::normal);
    REQUIRE(f.until([&] { return !f.view->loading(); }));
    CHECK(f.calls == 3);
    CHECK(f.view->usage().pixel_bytes == 8);
    CHECK(f.cache->get(f.file, thumbs::Size::large, "image/heic").source == thumbs::Source::cache);
    CHECK(f.cache->get(f.file, thumbs::Size::normal, "image/heic").source == thumbs::Source::cache);
}

namespace {

// A bounded stand-in for remote reads. Shared ownership keeps the state alive even when a
// failed assertion unwinds the test before the worker completes; timeout also prevents hangs.
struct SlowReads {
    std::atomic<bool> release{false};
    std::atomic<unsigned> calls{0};
    unsigned blocking_above{0};  // Set before publishing the loader to workers.

    picture::Loaded load(const picture::LoadRequest& request) {
        ++calls;
        if (request.path.filename().string().starts_with("slow") ||
            (blocking_above != 0 && request.max_width > blocking_above)) {
            const auto deadline = std::chrono::steady_clock::now() + 3s;
            while (!release && std::chrono::steady_clock::now() < deadline &&
                   (request.cancel == nullptr || !request.cancel->cancelled())) {
                std::this_thread::sleep_for(1ms);
            }
        }
        picture::Loaded result;
        if (request.cancel != nullptr && request.cancel->cancelled()) {
            result.failure = picture::Failure::unavailable;
        } else {
            result.image = {.width = 1, .height = 1, .rgba = {255, 0, 0, 255}};
        }
        return result;
    }
};

}  // namespace

TEST_CASE("thumbnails: inspector promotes queued grid work while the grid worker is blocked") {
    Fixture f;
    const auto slow = std::make_shared<SlowReads>();
    picture::set_loader(
        [slow](const picture::LoadRequest& request) { return slow->load(request); });
    const auto grid = f.dir.write("slow-grid.heic", "photo");
    static_cast<void>(f.view->get(grid, 1, thumbs::Size::large, "image/heic"));
    REQUIRE(f.until([&] { return slow->calls == 1; }));
    f.ask();  // Queued behind the slow grid read on the single shared worker.
    static_cast<void>(
        f.view->get(f.file, 1, thumbs::Size::large, "image/heic", nullptr, {.inspector = true}));
    REQUIRE(f.until([&] { return f.view->usage().pixel_bytes == 4; }));
    CHECK(slow->calls == 2);
    CHECK(fs::exists(f.cache->path_for(f.file, thumbs::Size::large)));
    CHECK_FALSE(fs::exists(f.cache->path_for(grid, thumbs::Size::large)));
    slow->release = true;
    REQUIRE(f.until([&] { return !f.view->loading(); }));
    CHECK(slow->calls == 2);  // The promoted old queue entry never decodes a second time.
    CHECK(f.view->usage().pixel_bytes == 8);
}

TEST_CASE("thumbnails: remote inspector reads warm cache with bounded obsolete work and pixels") {
    Fixture f;
    const auto slow = std::make_shared<SlowReads>();
    picture::set_loader(
        [slow](const picture::LoadRequest& request) { return slow->load(request); });
    const auto first = f.dir.write("slow-first.heic", "photo");
    const auto second = f.dir.write("slow-second.heic", "photo");
    const auto ask = [&](const fs::path& file) {
        static_cast<void>(f.view->get(file, 1, thumbs::Size::x_large, "image/heic", nullptr,
                                      {.inspector = true}));
        f.view->end_frame();
    };
    ask(first);
    REQUIRE(f.until([&] { return slow->calls == 1; }));
    ask(second);
    REQUIRE(f.until([&] { return slow->calls == 2; }));
    fs::path last;
    for (unsigned i = 0; i < 100; ++i) {
        last = f.dir.write("queued-" + std::to_string(i) + ".heic", "photo");
        ask(last);
        CHECK(f.view->usage().slots <= 3);  // Two running reads and only the latest selection.
    }
    CHECK(slow->calls == 2);
    CHECK(f.view->usage().pixel_bytes == 0);
    slow->release = true;
    REQUIRE(f.until([&] { return !f.view->loading(); }));
    CHECK(slow->calls == 3);
    CHECK(f.view->usage().slots == 1);
    CHECK(f.view->usage().pixel_bytes == 4);  // No pixels retained for old selections.
    CHECK(fs::exists(f.cache->path_for(first, thumbs::Size::x_large)));
    CHECK(fs::exists(f.cache->path_for(second, thumbs::Size::x_large)));
    CHECK(fs::exists(f.cache->path_for(last, thumbs::Size::x_large)));
    CHECK_FALSE(fs::exists(f.cache->path_for(f.dir.path / "queued-0.heic", thumbs::Size::x_large)));
    CHECK(f.cache->get(first, thumbs::Size::x_large, "image/heic").source == thumbs::Source::cache);
    CHECK(slow->calls == 3);  // Revisiting a completed offscreen read needs no remote decode.
}

TEST_CASE("thumbnails: offscreen grid reads have a fixed warming bound") {
    Fixture f;
    f.view.reset();
    const auto slow = std::make_shared<SlowReads>();
    picture::set_loader(
        [slow](const picture::LoadRequest& request) { return slow->load(request); });
    rummage::core::WorkerPool workers{f.loop, 6};
    rummage::views::Thumbnails view{f.loop, workers, f.cache, [] {}};
    std::vector<fs::path> files;
    for (unsigned i = 0; i < 6; ++i) {
        files.push_back(f.dir.write("slow-grid-" + std::to_string(i) + ".heic", "photo"));
        static_cast<void>(view.get(files.back(), 1, thumbs::Size::large, "image/heic"));
    }
    REQUIRE(f.until([&] { return slow->calls == 6; }));
    view.end_frame();
    view.end_frame();
    CHECK(view.usage().slots == 4);
    slow->release = true;
    REQUIRE(f.until([&] { return !view.loading(); }));
    CHECK(view.usage().slots == 0);
    CHECK(view.usage().pixel_bytes == 0);
    unsigned cached = 0;
    for (const auto& file : files) {
        cached += fs::exists(f.cache->path_for(file, thumbs::Size::large)) ? 1U : 0U;
    }
    CHECK(cached == 4);
}

namespace {

using rummage::views::ThumbnailCache;
using rummage::views::ThumbnailOptions;

std::int64_t modification_time(const fs::path& path) {
    struct stat info{};
    REQUIRE(::stat(path.c_str(), &info) == 0);
    return info.st_mtim.tv_sec * 1'000'000'000 + info.st_mtim.tv_nsec;
}

}  // namespace

TEST_CASE("thumbnails: source policy trusts only known unchanged entry snapshots") {
    rummage::fs::Entry entry;
    entry.modified_ns = 0;  // Epoch is a valid known timestamp.
    CHECK(ThumbnailOptions::for_entry(entry).cache == ThumbnailCache::snapshot);
    CHECK(ThumbnailOptions::for_entry(entry, true).inspector);
    entry.modified_known = false;
    CHECK(ThumbnailOptions::for_entry(entry).cache == ThumbnailCache::current);
    entry.modified_known = true;
    entry.available = false;
    CHECK(ThumbnailOptions::for_entry(entry).cache == ThumbnailCache::offline);
    entry.modified_known = false;
    CHECK(ThumbnailOptions::for_entry(entry).cache == ThumbnailCache::offline);
    entry.modified_known = true;
    entry.preview_current_location = true;
    CHECK(ThumbnailOptions::for_entry(entry).cache == ThumbnailCache::current);
}

TEST_CASE("thumbnails: a known snapshot cache hit never needs the original online or offline") {
    Fixture f;
    f.failures = 0;
    const auto modified = modification_time(f.file);
    REQUIRE_FALSE(f.cache->get(f.file, thumbs::Size::large, "image/heic").image.empty());
    REQUIRE(f.calls == 1);
    fs::rename(f.file, f.dir.path / "disconnected.heic");
    static_cast<void>(f.view->get(f.file, modified, thumbs::Size::large, "image/heic", nullptr,
                                  {.cache = ThumbnailCache::snapshot}));
    REQUIRE(f.until([&] { return !f.view->loading(); }));
    CHECK(f.view->usage().pixel_bytes == 4);
    static_cast<void>(f.view->get(f.file, modified, thumbs::Size::large, "image/heic", nullptr,
                                  {.cache = ThumbnailCache::offline}));
    REQUIRE(f.until([&] { return !f.view->loading(); }));
    CHECK(f.view->usage().pixel_bytes == 8);
    CHECK(f.calls == 1);
    CHECK_FALSE(fs::exists(f.file));
}

TEST_CASE("thumbnails: snapshot cache misses fall through to a current original load") {
    Fixture f;
    f.failures = 0;
    const auto modified = modification_time(f.file);
    REQUIRE_FALSE(f.cache->get(f.file, thumbs::Size::large, "image/heic").image.empty());
    REQUIRE(f.calls == 1);
    fs::last_write_time(f.file, fs::last_write_time(f.file) + 10s);
    static_cast<void>(f.view->get(f.file, modified + 10'000'000'000, thumbs::Size::large,
                                  "image/heic", nullptr, {.cache = ThumbnailCache::snapshot}));
    REQUIRE(f.until([&] { return !f.view->loading(); }));
    CHECK(f.calls == 2);
    CHECK(f.view->usage().pixel_bytes == 4);
}

TEST_CASE("thumbnails: changed or unknown entries cannot reuse an old snapshot texture") {
    rummage::ui::Offscreen gl{32, 32};
    Fixture f;
    f.failures = 0;
    const auto modified = modification_time(f.file);
    static_cast<void>(f.view->get(f.file, modified, thumbs::Size::large, "image/heic", nullptr,
                                  {.cache = ThumbnailCache::snapshot}));
    REQUIRE(f.until([&] { return !f.view->loading(); }));
    const auto* snapshot = f.view->get(f.file, modified, thumbs::Size::large, "image/heic", nullptr,
                                       {.cache = ThumbnailCache::snapshot});
    REQUIRE(snapshot != nullptr);
    REQUIRE(snapshot->width() == 1);
    fs::last_write_time(f.file, fs::last_write_time(f.file) + 10s);
    picture::set_loader([](const picture::LoadRequest&) {
        picture::Loaded made;
        made.image = {.width = 2, .height = 1, .rgba = {0, 255, 0, 255, 0, 255, 0, 255}};
        return made;
    });
    rummage::fs::Entry entry{.name = "photo.heic", .modified_ns = modified};
    SUBCASE("changed catalog location") {
        entry.available = false;
        entry.preview_current_location = true;
    }
    SUBCASE("unknown modification time") { entry.modified_known = false; }
    const auto options = ThumbnailOptions::for_entry(entry, true);
    CHECK(options.cache == ThumbnailCache::current);
    CHECK(f.view->get(f.file, modified, thumbs::Size::large, "image/heic", nullptr, options) ==
          nullptr);
    REQUIRE(f.until([&] { return !f.view->loading(); }));
    const auto* current =
        f.view->get(f.file, modified, thumbs::Size::large, "image/heic", nullptr, options);
    REQUIRE(current != nullptr);
    CHECK(current != snapshot);
    CHECK(current->width() == 2);
    CHECK(f.view->get(f.file, modified, thumbs::Size::large, "image/heic", nullptr,
                      {.cache = ThumbnailCache::snapshot}) == snapshot);
}

TEST_CASE("thumbnails: inspector shows this photo's smaller texture then upgrades its tier") {
    rummage::ui::Offscreen gl{32, 32};
    Fixture f;
    const auto slow = std::make_shared<SlowReads>();
    slow->blocking_above = 256;
    picture::set_loader(
        [slow](const picture::LoadRequest& request) { return slow->load(request); });
    f.ask();
    REQUIRE(f.until([&] { return !f.view->loading(); }));
    const auto* smaller = f.view->get(f.file, 1, thumbs::Size::large, "image/heic");
    REQUIRE(smaller != nullptr);
    thumbs::Size delivered = thumbs::Size::normal;
    const auto* progressive = f.view->get(f.file, 1, thumbs::Size::x_large, "image/heic", nullptr,
                                          {.inspector = true}, &delivered);
    CHECK(progressive == smaller);
    CHECK(delivered == thumbs::Size::large);
    CHECK(f.view->loading());
    REQUIRE(f.until([&] { return slow->calls == 2; }));
    // Ordinary grid requests retain their requested resolution contract.
    CHECK(f.view->get(f.file, 1, thumbs::Size::x_large, "image/heic") == nullptr);
    slow->release = true;
    REQUIRE(f.until([&] { return !f.view->loading(); }));
    const auto* full = f.view->get(f.file, 1, thumbs::Size::x_large, "image/heic", nullptr,
                                   {.inspector = true}, &delivered);
    REQUIRE(full != nullptr);
    CHECK(full != smaller);
    CHECK(delivered == thumbs::Size::x_large);
    CHECK(slow->calls == 2);
}

TEST_CASE("thumbnails: progressive inspector never borrows another snapshot or cache policy") {
    rummage::ui::Offscreen gl{32, 32};
    Fixture f;
    const auto slow = std::make_shared<SlowReads>();
    slow->blocking_above = 256;
    picture::set_loader(
        [slow](const picture::LoadRequest& request) { return slow->load(request); });
    f.ask();
    REQUIRE(f.until([&] { return !f.view->loading(); }));
    REQUIRE(f.view->get(f.file, 1, thumbs::Size::large, "image/heic") != nullptr);
    auto file = f.file;
    std::int64_t modified = 1;
    std::string mime = "image/heic";
    ThumbnailOptions options{.inspector = true};
    SUBCASE("different path") { file = f.dir.write("other.heic", "photo"); }
    SUBCASE("different nanosecond timestamp") { modified = 2; }
    SUBCASE("different MIME type") { mime = "image/jpeg"; }
    SUBCASE("offline policy") { options.cache = ThumbnailCache::offline; }
    SUBCASE("snapshot policy") { options.cache = ThumbnailCache::snapshot; }
    thumbs::Size delivered = thumbs::Size::normal;
    CHECK(f.view->get(file, modified, thumbs::Size::x_large, mime, nullptr, options, &delivered) ==
          nullptr);
    CHECK(delivered == thumbs::Size::x_large);
    slow->release = true;
    REQUIRE(f.until([&] { return !f.view->loading(); }));
}

TEST_CASE("thumbnails: exact snapshot cache hit bypasses blocked source and inspector workers") {
    rummage::ui::Offscreen gl{32, 32};
    Fixture f;
    f.failures = 0;
    const auto modified = modification_time(f.file);
    REQUIRE_FALSE(f.cache->get(f.file, thumbs::Size::x_large, "image/heic").image.empty());
    fs::rename(f.file, f.dir.path / "disconnected.heic");
    const auto slow = std::make_shared<SlowReads>();
    picture::set_loader(
        [slow](const picture::LoadRequest& request) { return slow->load(request); });
    const auto grid = f.dir.write("slow-grid.heic", "photo");
    const auto first = f.dir.write("slow-first.heic", "photo");
    const auto second = f.dir.write("slow-second.heic", "photo");
    static_cast<void>(f.view->get(grid, 1, thumbs::Size::large, "image/heic"));
    static_cast<void>(
        f.view->get(first, 1, thumbs::Size::x_large, "image/heic", nullptr, {.inspector = true}));
    static_cast<void>(
        f.view->get(second, 1, thumbs::Size::x_large, "image/heic", nullptr, {.inspector = true}));
    REQUIRE(f.until([&] { return slow->calls == 3; }));
    thumbs::Size delivered = thumbs::Size::normal;
    CHECK(f.view->get(f.file, modified, thumbs::Size::x_large, "image/heic", nullptr,
                      {.cache = ThumbnailCache::snapshot, .inspector = true},
                      &delivered) == nullptr);
    REQUIRE(f.until([&] { return f.view->usage().pixel_bytes == 4; }));
    CHECK(f.view->get(f.file, modified, thumbs::Size::x_large, "image/heic", nullptr,
                      {.cache = ThumbnailCache::snapshot, .inspector = true},
                      &delivered) != nullptr);
    CHECK(delivered == thumbs::Size::x_large);
    CHECK_FALSE(fs::exists(f.cache->path_for(grid, thumbs::Size::large)));
    CHECK_FALSE(fs::exists(f.cache->path_for(first, thumbs::Size::x_large)));
    CHECK_FALSE(fs::exists(f.cache->path_for(second, thumbs::Size::x_large)));
    CHECK(f.view->loading());
    slow->release = true;
    REQUIRE(f.until([&] { return !f.view->loading(); }));
    CHECK(slow->calls == 3);
}

TEST_CASE("thumbnails: snapshot retries count source attempts rather than cache stages") {
    Fixture f;
    f.failures = 100;
    static_cast<void>(f.view->get(f.file, 1, thumbs::Size::x_large, "image/heic", nullptr,
                                  {.cache = ThumbnailCache::snapshot, .inspector = true}));
    REQUIRE(f.until([&] { return !f.view->loading(); }));
    CHECK(f.calls == 3);
    CHECK(f.view->usage().pixel_bytes == 0);
    f.leave();
    CHECK(f.view->usage().slots == 0);
}

TEST_CASE("thumbnails: queued cache probes cancel before they can launch source reads") {
    Fixture f;
    const auto slow = std::make_shared<SlowReads>();
    bool blocker_done = false;
    static_cast<void>(f.pool.thumbnail_cache_lane().submit(
        rummage::core::Priority::visible,
        [slow](const rummage::core::CancelToken& token) {
            return slow->load({.path = "slow-blocker.heic", .cancel = &token});
        },
        [&](picture::Loaded) { blocker_done = true; }));
    REQUIRE(f.until([&] { return slow->calls == 1; }));
    static_cast<void>(f.view->get(f.file, 1, thumbs::Size::large, "image/heic", nullptr,
                                  {.cache = ThumbnailCache::snapshot}));
    f.leave();
    CHECK_FALSE(f.view->loading());
    CHECK(f.view->usage().slots == 0);
    slow->release = true;
    REQUIRE(f.until([&] { return blocker_done; }));
    f.wait(20ms);
    CHECK(f.calls == 0);
    CHECK(f.view->usage().slots == 0);
}

TEST_CASE("thumbnails: queued inspector cache probe promotes ahead of grid probes") {
    rummage::ui::Offscreen gl{32, 32};
    Fixture f;
    f.failures = 0;
    const auto grid = f.dir.write("grid.heic", "photo");
    const auto modified = modification_time(f.file);
    const auto grid_modified = modification_time(grid);
    REQUIRE_FALSE(f.cache->get(f.file, thumbs::Size::large, "image/heic").image.empty());
    REQUIRE_FALSE(f.cache->get(grid, thumbs::Size::large, "image/heic").image.empty());
    f.view.reset();
    bool first_redraw = false;
    bool selected_was_first = false;
    f.view = std::make_unique<rummage::views::Thumbnails>(f.loop, f.pool, f.cache, [&] {
        if (!first_redraw) {
            first_redraw = true;
            selected_was_first =
                f.view->get(f.file, modified, thumbs::Size::large, "image/heic", nullptr,
                            {.cache = ThumbnailCache::snapshot, .inspector = true}) != nullptr;
        }
    });
    const auto slow = std::make_shared<SlowReads>();
    static_cast<void>(f.pool.thumbnail_cache_lane().submit(
        rummage::core::Priority::visible,
        [slow](const rummage::core::CancelToken& token) {
            return slow->load({.path = "slow-blocker.heic", .cancel = &token});
        },
        [](picture::Loaded) {}));
    REQUIRE(f.until([&] { return slow->calls == 1; }));
    static_cast<void>(f.view->get(grid, grid_modified, thumbs::Size::large, "image/heic", nullptr,
                                  {.cache = ThumbnailCache::snapshot}));
    static_cast<void>(f.view->get(f.file, modified, thumbs::Size::large, "image/heic", nullptr,
                                  {.cache = ThumbnailCache::snapshot}));
    static_cast<void>(f.view->get(f.file, modified, thumbs::Size::large, "image/heic", nullptr,
                                  {.cache = ThumbnailCache::snapshot, .inspector = true}));
    slow->release = true;
    REQUIRE(f.until([&] { return !f.view->loading(); }));
    CHECK(first_redraw);
    CHECK(selected_was_first);
    CHECK(f.calls == 2);  // Only the seeded originals were decoded.
    CHECK(f.view->usage().textures == 1);
    CHECK(f.view->usage().pixel_bytes == 4);
}

TEST_CASE("thumbnails: offline cache miss never dispatches an original read") {
    Fixture f;
    f.failures = 0;
    static_cast<void>(f.view->get(f.file, modification_time(f.file), thumbs::Size::large,
                                  "image/heic", nullptr, {.cache = ThumbnailCache::offline}));
    REQUIRE(f.until([&] { return !f.view->loading(); }));
    CHECK(f.calls == 0);
    CHECK(f.view->usage().pixel_bytes == 0);
    CHECK_FALSE(fs::exists(f.cache->path_for(f.file, thumbs::Size::large)));
}
