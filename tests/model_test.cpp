#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "rummage/engines/engines.hpp"
#include "rummage/isolate/helpers.hpp"
#include "rummage/thumbs/cache.hpp"
#include "temp_dir.hpp"

TEST_CASE("model: optional routing thumbnail cache scale and page boundary") {
    namespace fs = std::filesystem;
    namespace isolate = rummage::isolate;
    namespace thumbs = rummage::thumbs;
    rummage::test::TempDir dir;
    const auto sample = dir.write("mesh.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    const auto registry =
        rummage::engines::Registry::load(RUMMAGE_ENGINE_DATA, [](std::string_view name) {
            return name == "rummage-model" ? fs::path{RUMMAGE_MODEL_HELPER} : fs::path{};
        });
    isolate::use_engines(registry);
    const thumbs::Cache cache{dir.path / "cache", "test"};
    for (const auto size : {thumbs::Size::normal, thumbs::Size::large}) {
        const auto first = cache.get(sample, size, "model/obj");
        REQUIRE_FALSE(first.image.empty());
        CHECK(cache.get(sample, size, "model/obj").source == thumbs::Source::cache);
    }
    isolate::Helpers helpers{{.program = RUMMAGE_MODEL_HELPER, .args = {}}};
    CHECK(helpers.load({.path = sample, .max_width = 128, .max_height = 128, .page = 1}).failure !=
          rummage::picture::Failure::none);
}
