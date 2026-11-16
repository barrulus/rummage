#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <variant>

#include <doctest/doctest.h>

#include "rummage/core/worker_pool.hpp"
#include "rummage/formats/image/load.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/isolate/helpers.hpp"
#include "rummage/preview/preview.hpp"
#include "rummage/thumbs/cache.hpp"
#include "temp_dir.hpp"

namespace {
namespace picture = rummage::picture;
namespace image = rummage::formats::image;
using rummage::test::TempDir;

constexpr auto kDrawing =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 80 40'>"
    "<rect width='40' height='40' fill='red' fill-opacity='.5'/></svg>";

picture::LoadRequest request(const std::filesystem::path& path, std::uint32_t width = 160,
                             std::uint32_t height = 120) {
    return {.path = path, .max_width = width, .max_height = height, .mime = "image/svg+xml"};
}

std::array<std::uint8_t, 4> pixel(const picture::Image& value, std::uint32_t x, std::uint32_t y) {
    const auto at = (std::size_t{y} * value.width + x) * 4;
    return {value.rgba.at(at), value.rgba.at(at + 1), value.rgba.at(at + 2), value.rgba.at(at + 3)};
}
}  // namespace

TEST_CASE("SVG previews: content detection, aspect ratio and straight alpha") {
    const TempDir dir;
    // A declaration and a long comment must not hide the root from a short magic probe.
    const auto path = dir.write("drawing.bin", "\xef\xbb\xbf \n<?xml version='1.0'?>\n<!--" +
                                                   std::string(70000, 'x') + "-->" + kDrawing);
    const auto reads = image::embedded_preview_reads();
    const auto loaded = image::load(request(path));
    REQUIRE_MESSAGE(loaded.failure == picture::Failure::none, loaded.detail);
    REQUIRE(loaded.image.width == 160);
    REQUIRE(loaded.image.height == 80);
    CHECK(pixel(loaded.image, 20, 20) == std::array<std::uint8_t, 4>{255, 0, 0, 128});
    CHECK(pixel(loaded.image, 120, 20)[3] == 0);
    CHECK_FALSE(loaded.from_preview);
    CHECK(loaded.full_width == 0);
    CHECK(loaded.full_height == 0);
    CHECK(image::embedded_preview_reads() == reads);

    // Fitting must respect both bounds, even when the longer requested side is vertical.
    const auto narrow = image::load(request(path, 30, 200));
    REQUIRE(narrow.failure == picture::Failure::none);
    CHECK(narrow.image.width == 30);
    CHECK(narrow.image.height == 15);
    const auto whitespace = dir.write("preamble.svg", std::string(70000, ' ') + kDrawing);
    CHECK(image::load(request(whitespace)).image.rgba == loaded.image.rgba);
}

TEST_CASE("SVG previews: output ceilings and unspecified bounds") {
    const TempDir dir;
    const auto path = dir.write("drawing.svg", kDrawing);
    for (const auto bound : {0U, 1024U, 4096U, std::numeric_limits<std::uint32_t>::max()}) {
        CAPTURE(bound);
        const auto loaded = image::load(request(path, bound, bound));
        REQUIRE_MESSAGE(loaded.failure == picture::Failure::none, loaded.detail);
        CHECK(loaded.image.width == 1024);
        CHECK(loaded.image.height == 512);
    }
    const auto one_bound = image::load(request(path, 30, 0));
    CHECK(one_bound.image.width == 30);
    CHECK(one_bound.image.height == 15);
}

TEST_CASE("SVG previews: malformed documents and bounded input") {
    const TempDir dir;
    for (const auto* content : {"<svg", "<html><svg width='8' height='8'/></html>",
                                "<svg width='8' height='8'><use href='#loop' id='loop'/></svg>"}) {
        const auto loaded = image::load(request(dir.write("broken.svg", content)));
        CHECK(loaded.failure == picture::Failure::damaged);
        CHECK(loaded.image.empty());
    }
    const auto large = dir.write("large.svg", kDrawing);
    std::filesystem::resize_file(large, 8U * 1024U * 1024U + 1U);
    CHECK(image::load(request(large)).failure == picture::Failure::too_large);
    CHECK(image::load(request(dir.path / "missing.svg")).failure == picture::Failure::unreadable);
}

TEST_CASE("SVG previews: actual sandbox renders fixtures and survives rejected input") {
    rummage::isolate::HelperOptions options;
    options.program = RUMMAGE_PREVIEW_HELPER;
    options.max_helpers = 1;
    rummage::isolate::Helpers helpers{options};
    for (const auto* name : {"solid.svg", "paths.svg", "strokes.svg", "linear-gradient.svg",
                             "group-opacity.svg", "adwaita-mask.svg", "blur.svg", "pattern.svg"}) {
        CAPTURE(name);
        const auto ask = request(std::filesystem::path{RUMMAGE_TEST_DATA} / "svg" / name);
        const auto there = helpers.load(ask);
        const auto here = image::load(ask);
        REQUIRE_MESSAGE(there.failure == picture::Failure::none, there.detail);
        CHECK(there.image.width == here.image.width);
        CHECK(there.image.height == here.image.height);
        CHECK(there.image.rgba == here.image.rgba);
    }
    const TempDir dir;
    CHECK(helpers.load(request(dir.write("broken.svg", "<svg"))).failure ==
          picture::Failure::damaged);
    const auto external =
        dir.write("external.svg",
                  "<svg width='80' height='40'><image width='80' height='40' "
                  "href='file:///etc/passwd'/><image href='https://example.invalid/image.png'/>"
                  "<rect width='40' height='40' fill='red' fill-opacity='.5'/></svg>");
    const auto survived = helpers.load(request(external));
    REQUIRE_MESSAGE(survived.failure == picture::Failure::none, survived.detail);
    CHECK(pixel(survived.image, 20, 20) == std::array<std::uint8_t, 4>{255, 0, 0, 128});
    CHECK(pixel(survived.image, 120, 20)[3] == 0);
    CHECK(helpers.running() == 1);
}

TEST_CASE("SVG previews: picture provider and reusable thumbnails") {
    const TempDir dir;
    const auto path = dir.write("drawing.svg", kDrawing);
    const auto mime = rummage::fs::MimeDatabase::load();
    const auto& provider = rummage::preview::provider_for(*mime, mime->find("image/svg+xml"));
    CHECK(provider.kind() == rummage::preview::Kind::image);
    const rummage::core::CancelToken cancel;
    const auto content =
        provider.load({.path = path, .mime = "image/svg+xml", .width = 160, .height = 120}, cancel);
    REQUIRE(std::holds_alternative<picture::Loaded>(content));
    const auto& loaded = std::get<picture::Loaded>(content);
    CHECK(loaded.failure == picture::Failure::none);
    CHECK(loaded.image.width == 160);
    CHECK(loaded.image.height == 80);

    const rummage::thumbs::Cache cache{dir.path / "thumbnails", "rummage-test"};
    const auto made = cache.get(path, rummage::thumbs::Size::normal);
    REQUIRE(made.source == rummage::thumbs::Source::decoded);
    CHECK(made.image.width == 128);
    CHECK(made.image.height == 64);
    const auto cached = cache.get(path, rummage::thumbs::Size::normal);
    CHECK(cached.source == rummage::thumbs::Source::cache);
    CHECK(cached.image.rgba == made.image.rgba);
}
