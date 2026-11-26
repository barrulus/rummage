#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <fstream>

#include <doctest/doctest.h>

#include "rummage/fs/trash.hpp"
#include "rummage/photos/client.hpp"
#include "scoped_env.hpp"
#include "temp_dir.hpp"

TEST_CASE("photos recovery: a crash after trash preserves receipts and never repeats deletion") {
    using Json = nlohmann::json;
    rummage::test::TempDir directory;
    rummage::test::ScopedEnv data{"XDG_DATA_HOME", (directory.path / "data").string()};
    const auto source = directory.path / "pictures";
    std::filesystem::create_directory(source);
    std::ofstream{source / "a.jpg"} << "same fixture bytes";
    std::ofstream{source / "b.jpg"} << "same fixture bytes";
    const auto catalog = directory.path / "catalog/catalog.sqlite3";
    const rummage::photos::Client client{RUMMAGE_PHOTOS_TEST_MODULE, catalog};
    const auto call = [&](Json request) {
        auto result = client.request(request);
        REQUIRE_MESSAGE(result.value("ok", false), result.dump());
        return result;
    };
    call({{"op", "add_source"}, {"path", source.string()}});
    call({{"op", "work"}});
    const auto rows = call({{"op", "query"}}).at("photos");
    REQUIRE(rows.size() == 2);
    const auto removed = rows[1].at("id");
    call(
        {{"op", "edit"}, {"ids", Json::array({removed})}, {"description", "Keep this annotation"}});
    const auto prepared = call({{"op", "operation_prepare"},
                                {"kind", "trash"},
                                {"ids", Json::array({removed})},
                                {"keeper_ids", Json::array({rows[0].at("id")})}});
    const auto original =
        std::filesystem::path{prepared.at("photos")[0].at("path").get<std::string>()};
    const auto result = rummage::fs::run_trash({.kind = rummage::fs::TransferKind::trash,
                                                .source = original.parent_path(),
                                                .names = {original.filename().string()},
                                                .destination = rummage::fs::home_trash()},
                                               {}, {});
    REQUIRE(result.errors.empty());
    REQUIRE(result.items.size() == 1);
    REQUIRE_FALSE(std::filesystem::exists(original));
    // Intentionally omit operation_complete, as if the process died after the filesystem move.
    const rummage::photos::Client restarted{RUMMAGE_PHOTOS_TEST_MODULE, catalog};
    const auto recovered = restarted.request({{"op", "recover_operations"}});
    REQUIRE_MESSAGE(recovered.value("ok", false), recovered.dump());
    const auto operations = call({{"op", "operations"}}).at("operations");
    REQUIRE(operations.size() == 1);
    CHECK(operations[0].at("state") == "completed");
    CHECK(std::filesystem::exists(source / "a.jpg"));
    CHECK(std::filesystem::exists(result.items[0].to));
    CHECK(call({{"op", "query"}, {"text", "annotation"}}).at("photos")[0].at("id") == removed);
    const auto restoring =
        call({{"op", "operation_restore_prepare"}, {"ids", Json::array({removed})}});
    const auto restored = rummage::fs::run_restore({.kind = rummage::fs::TransferKind::restore,
                                                    .source = result.items[0].to.parent_path(),
                                                    .names = {result.items[0].to.string()},
                                                    .destination = {}},
                                                   {}, {});
    REQUIRE(restored.errors.empty());
    REQUIRE(restored.items.size() == 1);
    REQUIRE(std::filesystem::exists(original));
    // The same receipt-based recovery works when restoring finished before the catalog update.
    const auto recovered_restore = restarted.request({{"op", "recover_operations"}});
    REQUIRE_MESSAGE(recovered_restore.value("ok", false), recovered_restore.dump());
    const auto after = call({{"op", "query"}, {"text", "annotation"}}).at("photos");
    REQUIRE(after.size() == 1);
    CHECK(after[0].at("availability") == "online");
    CHECK(after[0].at("id") == removed);
    CHECK(call({{"op", "operations"}}).at("operations")[0].at("id") ==
          restoring.at("operation_id"));
    CHECK(restarted.request({{"op", "recover_operations"}}).value("ok", false));
    CHECK(std::filesystem::exists(original));
}
