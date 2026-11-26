#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <filesystem>
#include <fontconfig/fontconfig.h>
#include <memory>

#include <doctest/doctest.h>

#include "rummage/engines/engines.hpp"
#include "rummage/isolate/helpers.hpp"
#include "rummage/thumbs/cache.hpp"
#include "temp_dir.hpp"
namespace fs = std::filesystem;
namespace picture = rummage::picture;
namespace isolate = rummage::isolate;
namespace engines = rummage::engines;

TEST_CASE("font: isolated scalable specimen, scale, cache, damaged and oversized input") {
    // Fontconfig locates the test font; only the helper parses its contents.
    using Pattern = std::unique_ptr<FcPattern, decltype(&FcPatternDestroy)>;
    Pattern request{FcNameParse(reinterpret_cast<const FcChar8*>("sans")), FcPatternDestroy};
    REQUIRE(request);
    FcConfigSubstitute(nullptr, request.get(), FcMatchPattern);
    FcDefaultSubstitute(request.get());
    FcResult match_result;
    Pattern match{FcFontMatch(nullptr, request.get(), &match_result), FcPatternDestroy};
    REQUIRE(match);
    FcChar8* filename = nullptr;
    REQUIRE(FcPatternGetString(match.get(), FC_FILE, 0, &filename) == FcResultMatch);
    const fs::path source{reinterpret_cast<const char*>(filename)};
    rummage::test::TempDir dir;
    const auto sample = dir.path / "sample.ttf";
    fs::copy_file(source, sample);
    isolate::Helpers helpers{{.program = RUMMAGE_FONT_HELPER, .args = {}}};
    for (const auto size : {128U, 256U, 8000U}) {
        const auto result = helpers.load({.path = sample, .max_width = size, .max_height = size});
        INFO(result.detail);
        REQUIRE(result.failure == picture::Failure::none);
        CHECK(result.image.width == std::min(size, 1024U));
        CHECK(result.image.height <= 1024);
        std::size_t ink = 0;
        for (std::size_t i = 0; i < result.image.rgba.size(); i += 4) {
            ink += result.image.rgba[i] < 128 ? 1U : 0U;
            CHECK(result.image.rgba[i + 3] == 255);
        }
        CHECK(ink > 50);
    }
    for (const auto& path : {dir.write("broken.ttf", "not a font"), dir.write("empty.otf", ""),
                             dir.write("cut.ttf", std::string{"\0\1\0\0", 4})}) {
        CHECK(helpers.load({.path = path, .max_width = 128, .max_height = 128}).failure !=
              picture::Failure::none);
    }
    const auto large = dir.write("large.ttf", "");
    fs::resize_file(large, 32 * 1024 * 1024 + 1);
    CHECK(helpers.load({.path = large, .max_width = 128, .max_height = 128}).failure ==
          picture::Failure::too_large);
    CHECK(helpers.load({.path = sample, .max_width = 128, .max_height = 128, .page = 1}).failure !=
          picture::Failure::none);
    const auto registry = engines::Registry::load(RUMMAGE_ENGINE_DATA, [](std::string_view name) {
        return name == "rummage-font" ? fs::path{RUMMAGE_FONT_HELPER} : fs::path{};
    });
    isolate::use_engines(registry);
    const rummage::thumbs::Cache cache{dir.path / "cache", "test"};
    CHECK_FALSE(cache.get(sample, rummage::thumbs::Size::normal, "font/ttf").image.empty());
    CHECK(cache.get(sample, rummage::thumbs::Size::normal, "font/ttf").source ==
          rummage::thumbs::Source::cache);
}
