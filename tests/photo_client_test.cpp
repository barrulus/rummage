#include <filesystem>

#include <doctest/doctest.h>

#include "rummage/photos/client.hpp"
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
#endif
