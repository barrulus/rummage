#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>

#include <doctest/doctest.h>

#include "rummage/core/worker_pool.hpp"
#include "rummage/engines/engines.hpp"
#include "rummage/isolate/helpers.hpp"
#include "rummage/picture/load.hpp"
#include "rummage/preview/preview.hpp"
#include "rummage/thumbs/cache.hpp"
#include "temp_dir.hpp"
namespace fs = std::filesystem;
namespace isolate = rummage::isolate;
namespace picture = rummage::picture;
namespace engines = rummage::engines;
using rummage::test::TempDir;
static const fs::path fixtures{RUMMAGE_MEDIA_DATA};

TEST_CASE("media: actual sandbox decodes common codecs, rotation, fades and dark clips") {
    isolate::Helpers helpers{
        {.program = RUMMAGE_MEDIA_HELPER, .args = {}, .timeout = std::chrono::seconds{8}}};
    for (const auto* file :
         {"red.mp4", "red-hevc.mp4", "red-vp9.webm", "red-av1.mkv", "fade.mp4"}) {
        INFO(file);
        auto loaded = helpers.load({.path = fixtures / file, .max_width = 128, .max_height = 128});
        INFO(loaded.detail);
        REQUIRE(loaded.failure == picture::Failure::none);
        CHECK(loaded.image.width == 96);
        CHECK(loaded.image.height == 48);
        CHECK(loaded.full_width == 96);
        CHECK(loaded.full_height == 48);
        CHECK(loaded.image.rgba[0] > 220);
        CHECK(loaded.image.rgba[1] < 30);
    }
    auto rotated =
        helpers.load({.path = fixtures / "rotated.mp4", .max_width = 128, .max_height = 128});
    REQUIRE(rotated.failure == picture::Failure::none);
    CHECK(rotated.image.width == 48);
    CHECK(rotated.image.height == 96);
    CHECK(rotated.full_width == 48);  // the whole video, upright
    CHECK(rotated.full_height == 96);
    auto black =
        helpers.load({.path = fixtures / "black.mp4", .max_width = 128, .max_height = 128});
    REQUIRE(black.failure == picture::Failure::none);
    CHECK(black.image.rgba[0] < 12);
    auto scaled = helpers.load({.path = fixtures / "red.mp4", .max_width = 32, .max_height = 32});
    REQUIRE(scaled.failure == picture::Failure::none);
    CHECK(scaled.image.width == 32);
    CHECK(scaled.image.height == 16);
    // Made smaller than the video: the whole size still says how large it is.
    CHECK(scaled.full_width == 96);
    CHECK(scaled.full_height == 48);
}

TEST_CASE("media: damaged files and external references fail without killing the app") {
    TempDir dir;
    isolate::Helpers helpers{{.program = RUMMAGE_MEDIA_HELPER, .args = {}}};
    for (const auto& path :
         {dir.write("broken.mp4", "not a video"),
          dir.write(
              "playlist.m3u8",
              "#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXTINF:1,\nhttp://127.0.0.1/private.ts\n")}) {
        auto result = helpers.load({.path = path, .max_width = 128, .max_height = 128});
        CHECK(result.failure != picture::Failure::none);
    }
    CHECK(
        helpers.load({.path = fixtures / "red.mp4", .max_width = 128, .max_height = 128}).failure ==
        picture::Failure::none);
}

TEST_CASE("media: MIME routes to its own helper; optional absence does not poison cache") {
    TempDir dir;
    const auto source = dir.path / "a 'clip'; $test.mp4";
    fs::copy_file(fixtures / "red.mp4", source);
    const auto registry = engines::Registry::load(RUMMAGE_ENGINE_DATA, [](std::string_view name) {
        return fs::path{name == "rummage-media" ? RUMMAGE_MEDIA_HELPER : RUMMAGE_BASE_HELPER};
    });
    isolate::use_engines(registry);
    const rummage::thumbs::Cache cache{dir.path / "cache", "test"};
    const auto generated = cache.get(source, rummage::thumbs::Size::normal, "video/mp4");
    INFO(generated.error);
    REQUIRE_FALSE(generated.image.empty());
    // Actual clean pixels, with no perforations baked into the cache.
    CHECK(generated.image.rgba[0] > 220);
    CHECK(picture::load_picture({.path = fs::path{RUMMAGE_IMAGE_DATA} / "blocks.png",
                                 .max_width = 64,
                                 .max_height = 64,
                                 .mime = "image/png"})
              .failure == picture::Failure::none);
    const auto base = engines::Registry::load(RUMMAGE_ENGINE_DATA, [](std::string_view name) {
        return name == "rummage-preview" ? fs::path{RUMMAGE_BASE_HELPER} : fs::path{};
    });
    isolate::use_engines(base);
    CHECK(cache.get(source, rummage::thumbs::Size::normal, "video/mp4").source ==
          rummage::thumbs::Source::cache);
    CHECK(cache.get(source, rummage::thumbs::Size::large, "video/mp4").source ==
          rummage::thumbs::Source::failed);
    CHECK_FALSE(fs::exists(dir.path / "cache/fail"));
    const auto mime = rummage::fs::MimeDatabase::load();
    const auto& provider = rummage::preview::provider_for(*mime, mime->find("video/mp4"));
    const auto content =
        provider.load({.path = source, .mime = "video/mp4", .width = 128, .height = 128}, {});
    CHECK(std::holds_alternative<rummage::preview::Info>(content));
    isolate::use_engines(registry);
    CHECK(cache.get(source, rummage::thumbs::Size::large, "video/mp4").source ==
          rummage::thumbs::Source::decoded);
}

TEST_CASE("media: cancellation interrupts an active helper request") {
    TempDir dir;
    isolate::Helpers helpers{
        {.program = RUMMAGE_FAKE_HELPER, .args = {"hang"}, .timeout = std::chrono::seconds{8}}};
    rummage::core::CancelToken token;
    auto future = std::async(std::launch::async, [&] {
        return helpers.load(
            {.path = fixtures / "red.mp4", .max_width = 128, .max_height = 128, .cancel = &token});
    });
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    token.cancel();
    CHECK(future.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
    CHECK(future.get().failure == picture::Failure::unavailable);
}

TEST_CASE("media: missing executable is disabled for this helper pool") {
    TempDir dir;
    const auto helper = dir.path / "optional-helper";
    isolate::Helpers helpers{{.program = helper, .args = {}}};
    CHECK(
        helpers.load({.path = fixtures / "red.mp4", .max_width = 128, .max_height = 128}).failure ==
        picture::Failure::unavailable);
    fs::create_symlink(RUMMAGE_MEDIA_HELPER, helper);
    CHECK(
        helpers.load({.path = fixtures / "red.mp4", .max_width = 128, .max_height = 128}).failure ==
        picture::Failure::unavailable);
    isolate::Helpers restarted{{.program = helper, .args = {}}};
    CHECK(restarted.load({.path = fixtures / "red.mp4", .max_width = 128, .max_height = 128})
              .failure == picture::Failure::none);
}

TEST_CASE("media: absent shared libraries disable the helper until restart") {
    TempDir dir;
    const auto executable = dir.path / "helper";
    fs::copy_file(RUMMAGE_FAKE_HELPER, executable);
    isolate::Helpers helpers{{.program = executable, .args = {"missing-libraries"}}};
    CHECK(
        helpers.load({.path = fixtures / "red.mp4", .max_width = 128, .max_height = 128}).failure ==
        picture::Failure::unavailable);
    fs::remove(executable);
    fs::create_symlink(RUMMAGE_MEDIA_HELPER, executable);
    CHECK(
        helpers.load({.path = fixtures / "red.mp4", .max_width = 128, .max_height = 128}).failure ==
        picture::Failure::unavailable);
}

TEST_CASE("media: sandboxed metadata includes video dimensions, duration, codec and frame rate") {
    isolate::Helpers helpers{{.program = RUMMAGE_MEDIA_HELPER, .args = {}}};
    const auto info = helpers.photo_info(fixtures / "red.mp4");
    CHECK(info.width == 96);
    CHECK(info.height == 48);
    CHECK(info.video_codec == "h264");
    CHECK_FALSE(info.duration.empty());
    CHECK_FALSE(info.frame_rate.empty());
    CHECK_FALSE(info.container.empty());
    CHECK_FALSE(info.bitrate.empty());
    CHECK(info.camera.empty());
    const auto tagged = helpers.photo_info(fixtures / "metadata.mp4");
    CHECK(tagged.audio_codec == "aac");
    CHECK(tagged.audio_channels == "2");
    CHECK(tagged.sample_rate == "48000 Hz");
    CHECK(tagged.media_title == "Metadata sample");
    CHECK(tagged.artist == "Rummage tests");
    const auto rotated = helpers.photo_info(fixtures / "rotated.mp4");
    CHECK(rotated.width == 48);
    CHECK(rotated.height == 96);
    CHECK_FALSE(rotated.rotation.empty());
    TempDir dir;
    CHECK(helpers.photo_info(dir.write("broken.mp4", "not video")).video_codec.empty());
    CHECK(helpers.photo_info(fixtures / "red.mp4").video_codec == "h264");
}

TEST_CASE("media: summary routes metadata to media and clears it when the helper is absent") {
    const auto registry = engines::Registry::load(RUMMAGE_ENGINE_DATA, [](std::string_view name) {
        return fs::path{name == "rummage-media" ? RUMMAGE_MEDIA_HELPER : RUMMAGE_BASE_HELPER};
    });
    isolate::use_engines(registry);
    auto summary = rummage::preview::summarize(fixtures / "red.mp4", "video/mp4",
                                               rummage::preview::Kind::image, {});
    REQUIRE(summary.photo);
    CHECK(summary.photo->video_codec == "h264");
    const auto base = engines::Registry::load(RUMMAGE_ENGINE_DATA, [](std::string_view name) {
        return name == "rummage-preview" ? fs::path{RUMMAGE_BASE_HELPER} : fs::path{};
    });
    isolate::use_engines(base);
    summary = rummage::preview::summarize(fixtures / "red.mp4", "video/mp4",
                                          rummage::preview::Kind::image, {});
    REQUIRE(summary.photo);
    CHECK(summary.photo->video_codec.empty());
    isolate::use_engines(registry);
}
