#include <filesystem>

#include <doctest/doctest.h>

#include "rummage/formats/image/photo.hpp"
#include "rummage/photos/client.hpp"
#include "rummage/picture/load.hpp"
#include "scoped_env.hpp"
#include "temp_dir.hpp"

TEST_CASE("photos client: absent or invalid components never create a catalog") {
    rummage::test::TempDir directory;
    const auto catalog = directory.path / "data/photos/catalog.sqlite3";
    for (const auto& module : {std::filesystem::path{}, directory.path / "missing.so",
                               std::filesystem::path{"relative.so"}}) {
        const rummage::photos::Client client{module, catalog};
        CHECK_FALSE(client.available());
        CHECK_FALSE(client.error().empty());
        const auto result = client.request({{"op", "sources"}});
        CHECK_FALSE(result.at("ok").get<bool>());
        CHECK_FALSE(result.at("error").get<std::string>().empty());
        CHECK_FALSE(std::filesystem::exists(catalog.parent_path()));
    }
}

#ifdef RUMMAGE_PHOTOS_TEST_MODULE
TEST_CASE("photos client: loading and copying a component is inert until an explicit request") {
    rummage::test::TempDir directory;
    const auto catalog = directory.path / "data/photos/catalog.sqlite3";
    const rummage::photos::Client client{RUMMAGE_PHOTOS_TEST_MODULE, catalog};
    REQUIRE_MESSAGE(client.available(), client.error());
    const auto copy = client;
    CHECK_FALSE(std::filesystem::exists(catalog.parent_path()));
    const auto result = copy.request({{"op", "sources"}});
    REQUIRE_MESSAGE(result.value("ok", false), result.dump());
    CHECK(result.at("sources").empty());
    CHECK(std::filesystem::exists(catalog));
    // A new client must access the same persisted catalog without any source discovery.
    const rummage::photos::Client reopened{RUMMAGE_PHOTOS_TEST_MODULE, catalog};
    CHECK(reopened.request({{"op", "jobs"}}).at("jobs").empty());
}

TEST_CASE("photos client: a missing catalog location is an actionable error") {
    const rummage::photos::Client client{RUMMAGE_PHOTOS_TEST_MODULE, {}};
    REQUIRE(client.available());
    const auto result = client.request({{"op", "sources"}});
    CHECK_FALSE(result.at("ok").get<bool>());
    CHECK(result.at("error").get<std::string>().find("XDG_DATA_HOME") != std::string::npos);
}

TEST_CASE(
    "photos client: malformed metadata and decoder exceptions complete claims and preserve the "
    "batch") {
    namespace fs = std::filesystem;
    namespace picture = rummage::picture;
    rummage::test::TempDir directory;
    rummage::test::ScopedEnv cache{"XDG_CACHE_HOME", (directory.path / "cache").string()};
    const auto source = directory.path / "source";
    fs::create_directories(source);
    for (const auto* name : {"throw.png", "invalid.png", "good.png"})
        fs::copy_file(fs::path{RUMMAGE_TEST_DATA} / "image/blocks.png", source / name);

    struct RestoreLoader {
        ~RestoreLoader() {
            picture::set_info_loader([](const fs::path& path, std::string_view) {
                return rummage::formats::image::read_photo_info(path);
            });
        }
    } restore;

    picture::set_info_loader([](const fs::path& path, std::string_view) {
        if (path.filename() == "throw.png")
            throw std::runtime_error("decoder failed");
        picture::PhotoInfo info;
        info.camera = path.filename() == "invalid.png" ? std::string{"\xff"} : "Good camera";
        return info;
    });
    const rummage::photos::Client client{RUMMAGE_PHOTOS_TEST_MODULE,
                                         directory.path / "catalog/catalog.sqlite3"};
    auto result = client.request({{"op", "add_source"}, {"path", source.string()}});
    REQUIRE_MESSAGE(result.value("ok", false), result.dump());
    std::size_t processed = 0;
    for (int iteration = 0; iteration < 20; ++iteration) {
        result = client.request({{"op", "work"}, {"budget", 32}});
        REQUIRE_MESSAGE(result.value("ok", false), result.dump());
        processed += result.value("metadata_processed", std::size_t{0});
        if (!result.value("worked", false))
            break;
    }
    CHECK(processed == 3);
    result = client.request({{"op", "query"}});
    REQUIRE_MESSAGE(result.value("ok", false), result.dump());
    REQUIRE(result.at("photos").size() == 3);
    for (const auto& photo : result.at("photos")) {
        const bool good = fs::path{photo.at("path").get<std::string>()}.filename() == "good.png";
        CHECK(photo.at("availability") == (good ? "online" : "unreadable"));
        if (good)
            CHECK(photo.at("camera") == "Good camera");
    }
    CHECK(client.request({{"op", "metadata_pending"}}).at("metadata").empty());
    const auto jobs = client.request({{"op", "jobs"}}).at("jobs");
    for (const auto& job : jobs)
        CHECK(job.at("state") == "completed");
}
#endif
