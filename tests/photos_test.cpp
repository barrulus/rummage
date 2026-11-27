#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include <doctest/doctest.h>

#include "rummage/photos/api.h"

namespace {
using Json = nlohmann::json;
namespace fs = std::filesystem;

struct Fixture {
    fs::path root;
    fs::path catalog;

    Fixture() {
        std::string pattern = "/tmp/rummage-photos-test-XXXXXX";
        auto* directory = ::mkdtemp(pattern.data());
        if (!directory)
            throw std::runtime_error("mkdtemp failed");
        root = directory;
        catalog = root / "data" / "catalog.db";
    }

    ~Fixture() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }

    Json call(Json request, bool success = true) const {
        auto* response = rummage_photos_request(catalog.c_str(), request.dump().c_str());
        REQUIRE(response != nullptr);
        const auto parsed = Json::parse(response);
        rummage_photos_free(response);
        INFO(parsed.dump());
        CHECK(parsed.at("ok") == success);
        return parsed;
    }

    fs::path file(const std::string& relative, const std::string& bytes = "image fixture") const {
        const auto path = root / relative;
        fs::create_directories(path.parent_path());
        std::ofstream(path) << bytes;
        return path;
    }

    Json add(const std::string& relative) const {
        return call({{"op", "add_source"}, {"path", (root / relative).string()}});
    }

    void drain(int maximum = 1000) const {
        for (int count = 0; count < maximum; ++count) {
            auto work = call({{"op", "work"}, {"budget", 2}});
            for (const auto& item : work.value("metadata", Json::array()))
                call({{"op", "metadata"},
                      {"id", item["id"]},
                      {"signature", item["signature"]},
                      {"width", 12},
                      {"height", 8},
                      {"capture_date", "2024-06-03T12:30:00"}});
            if (!work.value("worked", false) && work.value("metadata", Json::array()).empty())
                return;
        }
        FAIL("Jobs did not finish");
    }
};
}  // namespace

TEST_CASE("photo catalog ABI initializes lazily and rejects invalid requests") {
    Fixture fixture;
    CHECK(rummage_photos_version() == 1);
    CHECK_FALSE(fs::exists(fixture.catalog));
    CHECK(fixture.call({{"op", "sources"}})["sources"].empty());
    CHECK(fs::exists(fixture.catalog));
    fixture.call({{"op", "unknown"}}, false);
}

TEST_CASE("photo discovery combines roots, rejects overlap and persists distinct identities") {
    Fixture fixture;
    fixture.file("a/same.jpg");
    fixture.file("b/same.jpg");
    fixture.file("a/nested/another.jpg");
    fixture.file("a/video.mp4");
    fixture.add("a");
    fixture.add("b");
    fixture.call({{"op", "add_source"}, {"path", (fixture.root / "a/nested").string()}}, false);
    fixture.drain();
    auto result = fixture.call({{"op", "query"}});
    CHECK(result["total"] == 3);
    CHECK(result["photos"][0]["id"] != result["photos"][1]["id"]);
    CHECK(result["photos"][1]["path"] != result["photos"][2]["path"]);
    const auto ids = result["photos"];
    fixture.call({{"op", "refresh"}});
    fixture.drain();
    CHECK(fixture.call({{"op", "query"}})["photos"] == ids);
}

TEST_CASE("photo edits preserve unrelated tags and original dates across restart and refresh") {
    Fixture fixture;
    fixture.file("a/holiday.jpg");
    fixture.add("a");
    fixture.drain();
    const auto id = fixture.call({{"op", "query"}})["photos"][0]["id"];
    fixture.call({{"op", "edit"},
                  {"ids", Json::array({id})},
                  {"description", "京都の休日"},
                  {"rating", 4},
                  {"add_tags", Json::array({"旅行", "家族"})},
                  {"capture_date", "2020-02-29"}});
    fixture.call({{"op", "edit"},
                  {"ids", Json::array({id})},
                  {"add_tags", Json::array({"summer"})},
                  {"remove_tags", Json::array({"家族"})}});
    fixture.call({{"op", "refresh"}});
    fixture.drain();
    auto photo = fixture.call({{"op", "query"}, {"text", "旅行"}})["photos"][0];
    CHECK(photo["description"] == "京都の休日");
    CHECK(photo["tags"].size() == 2);
    CHECK(photo["capture_date"] == "2020-02-29");
    CHECK(photo["date_origin"] == "user");
    fixture.call({{"op", "edit"}, {"ids", Json::array({id})}, {"capture_date", nullptr}});
    photo = fixture.call({{"op", "query"}})["photos"][0];
    CHECK(photo["capture_date"] == "2024-06-03T12:30:00");
    CHECK(photo["date_origin"] == "embedded");
    fixture.call({{"op", "edit"}, {"ids", Json::array({id})}, {"capture_date", "2023-02-29"}},
                 false);
    CHECK(fixture.call({{"op", "query"}, {"text", "\" OR 1=1 --"}})["total"] == 0);
}

TEST_CASE("source disconnection preserves edits and relink requires established identity") {
    Fixture fixture;
    fixture.file("a/holiday.jpg");
    const auto source = fixture.add("a")["source"]["id"];
    fixture.drain();
    const auto id = fixture.call({{"op", "query"}})["photos"][0]["id"];
    fixture.call({{"op", "edit"}, {"ids", Json::array({id})}, {"rating", 5}});
    fs::rename(fixture.root / "a", fixture.root / "disconnected");
    fixture.call({{"op", "refresh"}});
    fixture.call({{"op", "work"}});
    CHECK(fixture.call({{"op", "query"}})["photos"][0]["availability"] == "offline");
    fixture.call({{"op", "relink_source"},
                  {"source_id", source},
                  {"path", (fixture.root / "disconnected").string()},
                  {"confirm", true}});
    const auto jobs = fixture.call({{"op", "jobs"}})["jobs"];
    fixture.call({{"op", "job_control"}, {"id", jobs[0]["id"]}, {"action", "retry"}});
    fixture.drain();
    auto photo = fixture.call({{"op", "query"}})["photos"][0];
    CHECK(photo["id"] == id);
    CHECK(photo["rating"] == 5);
    fixture.call({{"op", "remove_source"}, {"id", source}});
    CHECK(fixture.call({{"op", "query"}})["total"] == 0);
    CHECK(fixture.call({{"op", "query"}, {"include_inactive", true}})["total"] == 1);
    CHECK(fs::exists(fixture.root / "disconnected/holiday.jpg"));
}

TEST_CASE("paused jobs do not discover or claim metadata and resume durable batches") {
    Fixture fixture;
    for (int i = 0; i < 9; ++i)
        fixture.file("a/" + std::to_string(i) + ".jpg");
    const auto job = fixture.add("a")["job_id"];
    auto batch = fixture.call({{"op", "work"}, {"budget", 2}});
    CHECK(batch["processed"] == 2);
    fixture.call({{"op", "job_control"}, {"id", job}, {"action", "pause"}});
    const auto total = fixture.call({{"op", "query"}})["total"];
    CHECK_FALSE(fixture.call({{"op", "work"}})["worked"].get<bool>());
    CHECK(fixture.call({{"op", "metadata_pending"}})["metadata"].empty());
    CHECK(fixture.call({{"op", "query"}})["total"] == total);
    fixture.call({{"op", "job_control"}, {"id", job}, {"action", "resume"}});
    fixture.drain();
    CHECK(fixture.call({{"op", "query"}})["total"] == 9);
    CHECK(fixture.call({{"op", "jobs"}})["jobs"][0]["state"] == "completed");
}

TEST_CASE("metadata claims serialize windows and stale results never replace derived fields") {
    Fixture fixture;
    fixture.file("a/test.jpg");
    fixture.add("a");
    const auto item = fixture.call({{"op", "work"}})["metadata"][0];
    auto claim = fixture.call({{"op", "metadata_claim"},
                               {"id", item["id"]},
                               {"signature", item["signature"]},
                               {"owner", "window-one"}});
    CHECK(claim["allowed"] == true);
    CHECK(fixture.call({{"op", "metadata_claim"},
                        {"id", item["id"]},
                        {"signature", item["signature"]},
                        {"owner", "window-two"}})["allowed"] == false);
    fixture.file("a/test.jpg", "changed original bytes");
    auto result = fixture.call({{"op", "metadata"},
                                {"id", item["id"]},
                                {"signature", item["signature"]},
                                {"claim_token", claim["claim_token"]},
                                {"width", 99}});
    CHECK(result["stale"] == true);
    CHECK(fixture.call({{"op", "query"}})["photos"][0]["width"] == 0);
}

TEST_CASE("scan progress distinguishes discovery from metadata and changed originals") {
    Fixture fixture;
    fixture.file("a/healthy.jpg");
    fixture.file("a/damaged.jpg");
    fixture.file("a/changed.jpg");
    fixture.add("a");
    const auto progress = [&] { return fixture.call({{"op", "jobs"}})["jobs"][0]; };
    auto job = progress();
    CHECK(job["phase"] == "discovering");
    CHECK(job["pending_directories"] == 1);
    CHECK(job["metadata_total"] == 0);

    const auto work = fixture.call({{"op", "work"}, {"budget", 64}});
    REQUIRE(work["metadata"].size() == 3);
    job = progress();
    CHECK(job["state"] == "running");
    CHECK(job["phase"] == "metadata");
    CHECK(job["pending_directories"] == 0);
    CHECK(job["metadata_completed"] == 0);
    CHECK(job["metadata_total"] == 3);
    CHECK(job["metadata_pending"] == 3);
    const auto discovery_completed = job["completed"];

    // A metadata attempt advances progress without advancing directory-entry counts.
    const auto first = work["metadata"][0];
    fixture.call({{"op", "metadata"},
                  {"id", first["id"]},
                  {"signature", first["signature"]},
                  {"width", 12},
                  {"height", 8}});
    job = progress();
    CHECK(job["completed"] == discovery_completed);
    CHECK(job["metadata_completed"] == 1);
    CHECK(job["metadata_pending"] == 2);
    CHECK(job["phase"] == "metadata");

    const auto second = work["metadata"][1];
    fixture.call({{"op", "metadata"},
                  {"id", second["id"]},
                  {"signature", second["signature"]},
                  {"error", "Damaged image"}});
    const auto third = work["metadata"][2];
    std::ofstream{third["path"].get<std::string>()} << "Changed since discovery";
    CHECK(fixture.call({{"op", "metadata"},
                        {"id", third["id"]},
                        {"signature", third["signature"]}})["stale"] == true);
    job = progress();
    CHECK(job["state"] == "running");
    CHECK(job["phase"] == "finishing");
    CHECK(job["metadata_completed"] == 2);
    CHECK(job["metadata_total"] == 2);
    CHECK(job["metadata_pending"] == 0);
    CHECK(job["metadata_unavailable"] == 1);
    CHECK(job["failures"].size() == 1);

    fixture.call({{"op", "work"}});
    job = progress();
    CHECK(job["state"] == "completed");
    CHECK(job["phase"] == "complete");
    CHECK(job["metadata_completed"] == 2);
    CHECK(job["metadata_unavailable"] == 1);
}

TEST_CASE("byte duplicate review verifies changed candidates and records intent without deletion") {
    Fixture fixture;
    fixture.file("a/one.jpg", "identical");
    fixture.file("a/two.jpg", "identical");
    fixture.file("a/three.jpg", "different");
    fixture.add("a");
    fixture.drain();
    fixture.call({{"op", "find_duplicates"}});
    fixture.drain();
    auto groups = fixture.call({{"op", "duplicates"}})["groups"];
    REQUIRE(groups.size() == 1);
    auto first = groups[0]["photos"][0], second = groups[0]["photos"][1];
    fixture.call({{"op", "verify_duplicates"}, {"ids", Json::array({first["id"], second["id"]})}});
    auto operation = fixture.call({{"op", "operation_prepare"},
                                   {"kind", "trash"},
                                   {"ids", Json::array({second["id"]})},
                                   {"keeper_ids", Json::array({first["id"]})}});
    CHECK(fs::exists(second["path"].get<std::string>()));
    CHECK(fixture.call({{"op", "reconcile_operations"}})["operations"][0]["photos"][0]["state"] ==
          "original_present");
    std::ofstream(second["path"].get<std::string>()) << "changed";
    fixture.call({{"op", "verify_duplicates"}, {"ids", Json::array({first["id"], second["id"]})}},
                 false);
    fixture.call(
        {{"op", "operation_complete"},
         {"operation_id", operation["operation_id"]},
         {"results",
          Json::array({{{"id", second["id"]}, {"success", false}, {"error", "changed"}}})}});
    CHECK(fs::exists(first["path"].get<std::string>()));
}

TEST_CASE("annotation export restores only exact established identities") {
    Fixture fixture;
    fixture.file("a/one.jpg");
    fixture.file("a/two.jpg");
    fixture.add("a");
    fixture.drain();
    auto photos = fixture.call({{"op", "query"}})["photos"];
    auto album = fixture.call({{"op", "create_album"}, {"name", "Trip"}})["id"];
    fixture.call({{"op", "edit"},
                  {"ids", Json::array({photos[0]["id"]})},
                  {"description", "restored"},
                  {"add_tags", Json::array({"旅行"})},
                  {"add_albums", Json::array({album})},
                  {"capture_date", "2021-01-02"}});
    const auto exported = fixture.call({{"op", "export"}})["data"];
    const auto old_catalog = fixture.catalog;
    fixture.catalog = fixture.root / "new" / "catalog.db";
    fixture.add("a");
    fixture.drain();
    auto result = fixture.call({{"op", "restore"}, {"data", exported}});
    CHECK(result["restored"] == 2);
    CHECK(result["ambiguous"].empty());
    auto restored = fixture.call({{"op", "query"}, {"text", "restored"}})["photos"][0];
    CHECK(restored["capture_date"] == "2021-01-02");
    CHECK(restored["albums"].size() == 1);
    CHECK(fs::exists(old_catalog));
}

TEST_CASE("replacement at an old location never inherits its previous annotations") {
    Fixture fixture;
    fixture.file("a/one.jpg");
    fixture.add("a");
    fixture.drain();
    const auto id = fixture.call({{"op", "query"}})["photos"][0]["id"];
    fixture.call(
        {{"op", "edit"}, {"ids", Json::array({id})}, {"description", "private annotation"}});
    fixture.file("replacement.jpg", "different image");
    fs::rename(fixture.root / "replacement.jpg", fixture.root / "a/one.jpg");
    fixture.call({{"op", "refresh"}});
    fixture.drain();
    const auto photo = fixture.call({{"op", "query"}})["photos"][0];
    CHECK(photo["id"] != id);
    CHECK(photo["description"] == "");
    CHECK(fixture.call({{"op", "query"}, {"include_inactive", true}})["total"] == 2);
}

TEST_CASE("fresh-catalog restore is atomic and remaps saved filter identities") {
    Fixture fixture;
    fixture.file("a/one.jpg");
    fixture.file("a/two.jpg");
    fixture.add("a");
    fixture.drain();
    auto photos = fixture.call({{"op", "query"}})["photos"];
    const auto album = fixture.call({{"op", "create_album"}, {"name", "Empty album"}})["id"];
    fixture.call({{"op", "save_filter"},
                  {"name", "Trip"},
                  {"filter", {{"album_id", album}, {"source_id", 1}}}});
    fixture.call(
        {{"op", "edit"}, {"ids", Json::array({photos[0]["id"]})}, {"description", "imported"}});
    const auto data = fixture.call({{"op", "export"}})["data"];
    fixture.catalog = fixture.root / "fresh" / "catalog.db";
    fixture.call({{"op", "create_album"}, {"name", "Unrelated"}});
    auto broken = data;
    broken["photos"][1]["rating"] = 9;
    fixture.call({{"op", "restore"}, {"data", broken}}, false);
    CHECK(fixture.call({{"op", "query"}})["total"] == 0);
    CHECK(fixture.call({{"op", "sources"}})["sources"].empty());
    const auto restored = fixture.call({{"op", "restore"}, {"data", data}});
    CHECK(restored["restored"] == 2);
    CHECK(fixture.call({{"op", "jobs"}})["jobs"].empty());
    CHECK(fixture.call({{"op", "albums"}})["albums"].size() == 2);
    CHECK(fixture.call({{"op", "filters"}})["filters"][0]["filter"]["album_id"] != album);
    CHECK(fixture.call({{"op", "query"}, {"text", "imported"}})["total"] == 1);
}

TEST_CASE("partial trash receipts merge and restore preserves photo identity") {
    Fixture fixture;
    fixture.file("a/one.jpg");
    fixture.file("a/two.jpg");
    fixture.file("b/three.jpg");
    fixture.add("a");
    fixture.add("b");
    fixture.drain();
    const auto photos = fixture.call({{"op", "query"}})["photos"];
    const auto operation = fixture.call({{"op", "operation_prepare"},
                                         {"kind", "trash"},
                                         {"ids", Json::array({photos[1]["id"], photos[2]["id"]})},
                                         {"keeper_ids", Json::array({photos[0]["id"]})}});
    REQUIRE(operation["keepers"].size() == 1);
    fs::create_directory(fixture.root / "trash");
    for (std::size_t index = 1; index < 3; ++index) {
        const auto destination = fixture.root / "trash" / std::to_string(index);
        fs::rename(photos[index]["path"].get<std::string>(), destination);
        const auto result =
            fixture.call({{"op", "operation_complete"},
                          {"operation_id", operation["operation_id"]},
                          {"results", Json::array({{{"id", photos[index]["id"]},
                                                    {"success", true},
                                                    {"destination", destination.string()}}})}});
        CHECK(result["partial"] == (index == 1));
    }
    CHECK(fixture.call({{"op", "operations"}})["operations"][0]["results"].size() == 2);
    const auto restore = fixture.call(
        {{"op", "operation_restore_prepare"}, {"ids", Json::array({photos[1]["id"]})}});
    const auto& captured = restore["photos"][0];
    fs::rename(captured["path"].get<std::string>(), captured["original_path"].get<std::string>());
    fixture.call({{"op", "operation_complete"},
                  {"operation_id", restore["operation_id"]},
                  {"results", Json::array({{{"id", captured["id"]},
                                            {"success", true},
                                            {"destination", captured["original_path"]}}})}});
    const auto restored =
        fixture.call({{"op", "query"}, {"text", photos[1]["filename"]}})["photos"][0];
    CHECK(restored["id"] == photos[1]["id"]);
    CHECK(restored["availability"] == "online");
}

TEST_CASE("confirmed file and folder moves preserve annotations across source roots") {
    Fixture fixture;
    fixture.file("a/nested/one.jpg");
    fixture.file("b/two.jpg");
    fixture.add("a");
    fixture.add("b");
    fixture.drain();
    const auto id = fixture.call({{"op", "query"}, {"text", "one"}})["photos"][0]["id"];
    fixture.call({{"op", "edit"}, {"ids", Json::array({id})}, {"rating", 5}});
    fs::rename(fixture.root / "a/nested", fixture.root / "b/renamed");
    const auto result =
        fixture.call({{"op", "reconcile_locations"},
                      {"kind", "move"},
                      {"items", Json::array({{{"from", (fixture.root / "a/nested").string()},
                                              {"to", (fixture.root / "b/renamed").string()}}})}});
    CHECK(result["changed"] == 1);
    const auto photo = fixture.call({{"op", "query"}, {"text", "one"}})["photos"][0];
    CHECK(photo["id"] == id);
    CHECK(photo["rating"] == 5);
    CHECK(photo["relative_path"] == "renamed/one.jpg");
}

TEST_CASE("catalog backup is consistent and never overwrites an existing destination") {
    Fixture fixture;
    fixture.file("a/one.jpg");
    fixture.add("a");
    fixture.drain();
    const auto backup = fixture.root / "backup.db";
    fixture.call({{"op", "backup"}, {"path", backup.string()}});
    fixture.call({{"op", "backup"}, {"path", backup.string()}}, false);
    fixture.catalog = backup;
    CHECK(fixture.call({{"op", "query"}})["total"] == 1);
}

TEST_CASE("minute-precision embedded dates preserve precision when clearing an override") {
    Fixture fixture;
    fixture.file("a/one.jpg");
    fixture.add("a");
    const auto item = fixture.call({{"op", "work"}})["metadata"][0];
    fixture.call({{"op", "metadata"},
                  {"id", item["id"]},
                  {"signature", item["signature"]},
                  {"capture_date", "2024-06-03T12:30"}});
    fixture.call(
        {{"op", "edit"}, {"ids", Json::array({item["id"]})}, {"capture_date", "2020-02-29"}});
    fixture.call({{"op", "edit"}, {"ids", Json::array({item["id"]})}, {"capture_date", nullptr}});
    CHECK(fixture.call({{"op", "query"}})["photos"][0]["capture_date"] == "2024-06-03T12:30");
}

TEST_CASE("directory mutation between batches rewinds discovery without losing entries") {
    Fixture fixture;
    for (int i = 0; i < 8; ++i)
        fixture.file("a/" + std::to_string(i) + ".jpg");
    fixture.add("a");
    fixture.call({{"op", "work"}, {"budget", 2}});
    fixture.file("a/added-after-checkpoint.jpg");
    fixture.drain();
    CHECK(fixture.call({{"op", "query"}})["total"] == 9);
}

TEST_CASE("metadata failure does not hide deletion of unrelated healthy originals") {
    Fixture fixture;
    fixture.file("a/healthy.jpg");
    fixture.add("a");
    fixture.drain();
    fs::remove(fixture.root / "a/healthy.jpg");
    fixture.file("a/damaged.jpg", "damaged data");
    fixture.call({{"op", "refresh"}});
    auto work = fixture.call({{"op", "work"}});
    REQUIRE(work["metadata"].size() == 1);
    const auto item = work["metadata"][0];
    fixture.call({{"op", "metadata"},
                  {"id", item["id"]},
                  {"signature", item["signature"]},
                  {"error", "Cannot decode image"}});
    fixture.drain();
    CHECK(fixture.call({{"op", "query"}, {"text", "healthy"}})["photos"][0]["availability"] ==
          "missing");
    CHECK(fixture.call({{"op", "query"}, {"text", "damaged"}})["photos"][0]["availability"] ==
          "unreadable");
}

TEST_CASE("metadata claims from dead processes recover immediately on restart") {
    Fixture fixture;
    fixture.file("a/one.jpg");
    fixture.add("a");
    const auto item = fixture.call({{"op", "work"}})["metadata"][0];
    CHECK(fixture.call({{"op", "metadata_claim"},
                        {"id", item["id"]},
                        {"signature", item["signature"]},
                        {"owner", "2147483647"}})["allowed"] == true);
    CHECK(fixture.call({{"op", "metadata_claim"},
                        {"id", item["id"]},
                        {"signature", item["signature"]},
                        {"owner", std::to_string(::getpid())}})["allowed"] == true);
}

TEST_CASE("invalid UTF-8 filenames are reported without hiding healthy photographs") {
    Fixture fixture;
    fixture.file("a/healthy.jpg");
    fixture.file(std::string("a/bad-") + static_cast<char>(0xff) + ".jpg");
    fixture.add("a");
    fixture.drain();
    CHECK(fixture.call({{"op", "query"}})["total"] == 1);
    const auto jobs = fixture.call({{"op", "jobs"}})["jobs"];
    REQUIRE(jobs[0]["failures"].size() == 1);
    CHECK(jobs[0]["failures"][0]["stage"] == "filename");
}

TEST_CASE("unfinished directory replays safely after an owning process exits") {
    Fixture fixture;
    for (int i = 0; i < 9; ++i)
        fixture.file("a/" + std::to_string(i) + ".jpg");
    fixture.add("a");
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        auto* response =
            rummage_photos_request(fixture.catalog.c_str(), "{\"op\":\"work\",\"budget\":2}");
        bool success = false;
        if (response) {
            const auto result = Json::parse(response);
            success = result.value("ok", false) && result.value("processed", 0) == 2;
            rummage_photos_free(response);
        }
        ::_exit(success ? 0 : 1);
    }
    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 0);
    const auto first = fixture.call({{"op", "query"}})["photos"];
    REQUIRE(first.size() == 2);
    fixture.drain();
    CHECK(fixture.call({{"op", "query"}})["total"] == 9);
    for (const auto& item : first)
        CHECK(fixture.call({{"op", "query"}, {"text", item["filename"]}})["photos"][0]["id"] ==
              item["id"]);
}

TEST_CASE("confirmed source-root rename updates its mapping and preserves annotations") {
    Fixture fixture;
    fixture.file("a/one.jpg");
    fixture.add("a");
    fixture.drain();
    const auto photo = fixture.call({{"op", "query"}})["photos"][0];
    fixture.call({{"op", "edit"}, {"ids", Json::array({photo["id"]})}, {"rating", 5}});
    fs::rename(fixture.root / "a", fixture.root / "renamed");
    fixture.call({{"op", "reconcile_locations"},
                  {"kind", "rename"},
                  {"items", Json::array({{{"from", (fixture.root / "a").string()},
                                          {"to", (fixture.root / "renamed").string()}}})}});
    const auto result = fixture.call({{"op", "query"}})["photos"][0];
    CHECK(result["id"] == photo["id"]);
    CHECK(result["rating"] == 5);
    CHECK(result["availability"] == "online");
    CHECK(result["path"] == (fixture.root / "renamed/one.jpg").string());
    fixture.call({{"op", "refresh"}});
    fixture.drain();
    CHECK(fixture.call({{"op", "query"}})["total"] == 1);
}

TEST_CASE("folder filters treat wildcard characters as literal path characters") {
    Fixture fixture;
    fixture.file("a/trip[1]/one.jpg");
    fixture.file("a/trip1/two.jpg");
    fixture.add("a");
    fixture.drain();
    const auto result = fixture.call({{"op", "query"}, {"folder", "trip[1]"}});
    CHECK(result["total"] == 1);
    CHECK(result["photos"][0]["filename"] == "one.jpg");
}

TEST_CASE("watch inventory includes empty directories and excludes internal data") {
    Fixture fixture;
    fs::create_directories(fixture.root / "pictures/empty/deep");
    fixture.add("pictures");
    fixture.drain();
    const auto paths = fixture.call({{"op", "watch_paths"}, {"limit", 1}});
    REQUIRE(paths["paths"].size() == 1);
    CHECK(paths["more"] == true);
    const auto next =
        fixture.call({{"op", "watch_paths"}, {"after_id", paths["next_after_id"]}, {"limit", 10}});
    CHECK(next["paths"].size() == 2);
    CHECK(next["more"] == false);
    fs::remove_all(fixture.root / "pictures/empty");
    fixture.call({{"op", "refresh"}});
    fixture.drain();
    CHECK(fixture.call({{"op", "watch_paths"}})["paths"].size() == 1);
}

TEST_CASE("global pause and gentle pace persist across clients without changing named states") {
    Fixture fixture;
    for (int index = 0; index < 20; ++index)
        fixture.file("a/" + std::to_string(index) + ".jpg");
    fixture.add("a");
    fixture.call({{"op", "background_control"}, {"paused", true}, {"pace", "gentle"}});
    CHECK(fixture.call({{"op", "work"}})["worked"] == false);
    CHECK(fixture.call({{"op", "query"}})["total"] == 0);
    CHECK(fixture.call({{"op", "jobs"}})["jobs"][0]["state"] == "queued");
    fixture.call({{"op", "background_control"}, {"paused", false}});
    const auto work = fixture.call({{"op", "work"}, {"budget", 64}});
    CHECK(work["processed"].get<int>() <= 8);
    CHECK(work["metadata_limit"] == 1);
    CHECK(work["delay_ms"] == 1000);
    fixture.call({{"op", "background_control"}, {"paused", true}});
    const auto item = work["metadata"][0];
    CHECK(fixture.call({{"op", "metadata_claim"},
                        {"id", item["id"]},
                        {"signature", item["signature"]}})["allowed"] == false);
    fixture.call({{"op", "background_control"}, {"paused", false}, {"pace", "normal"}});
    fixture.drain();
    CHECK(fixture.call({{"op", "query"}})["total"] == 20);
}

TEST_CASE("watch refresh coalesces one follow-up when current scan already started") {
    Fixture fixture;
    for (int index = 0; index < 10; ++index)
        fixture.file("a/" + std::to_string(index) + ".jpg");
    const auto source = fixture.add("a")["source"]["id"];
    fixture.call({{"op", "work"}, {"budget", 2}});
    for (int event = 0; event < 10; ++event)
        fixture.call({{"op", "refresh"}, {"source_id", source}, {"reconcile", true}});
    fixture.drain();
    CHECK(fixture.call({{"op", "jobs"}})["jobs"].size() == 2);
    CHECK(fixture.call({{"op", "query"}})["total"] == 10);
}

TEST_CASE("forget source prunes captured annotations while retaining unrelated receipts") {
    Fixture fixture;
    fixture.file("a/one.jpg");
    fixture.file("a/two.jpg");
    fixture.file("b/three.jpg");
    fixture.file("b/four.jpg");
    const auto source = fixture.add("a")["source"]["id"];
    fixture.add("b");
    fixture.drain();
    const auto photos = fixture.call({{"op", "query"}})["photos"];
    Json forgotten = Json::array(), retained = Json::array();
    for (const auto& photo : photos)
        (photo["source_id"] == source ? forgotten : retained).push_back(photo["id"]);
    fixture.call({{"op", "edit"}, {"ids", forgotten}, {"description", "must be forgotten"}});
    fixture.call({{"op", "operation_prepare"},
                  {"kind", "trash"},
                  {"ids", Json::array({forgotten[1], retained[1]})},
                  {"keeper_ids", Json::array({forgotten[0], retained[0]})}});
    fixture.call({{"op", "remove_source"}, {"source_id", source}});
    fixture.call({{"op", "forget_source"}, {"source_id", source}});
    const auto operations = fixture.call({{"op", "operations"}})["operations"];
    REQUIRE(operations.size() == 1);
    CHECK(operations[0]["photos"].size() == 1);
    CHECK(operations[0]["keepers"].size() == 1);
    CHECK(operations.dump().find("must be forgotten") == std::string::npos);
    CHECK(fs::exists(fixture.root / "a/one.jpg"));
    CHECK(fs::exists(fixture.root / "a/two.jpg"));
}

TEST_CASE("duplicate trash rejects originals changed after the displayed review") {
    Fixture fixture;
    fixture.file("a/one.jpg", "original");
    fixture.file("a/two.jpg", "original");
    fixture.add("a");
    fixture.drain();
    const auto reviewed = fixture.call({{"op", "query"}})["photos"];
    fixture.file("a/one.jpg", "changed identical bytes");
    fixture.file("a/two.jpg", "changed identical bytes");
    fixture.call({{"op", "refresh"}});
    fixture.drain();
    const Json request{{"op", "operation_prepare"},
                       {"kind", "trash"},
                       {"ids", Json::array({reviewed[1]["id"]})},
                       {"keeper_ids", Json::array({reviewed[0]["id"]})},
                       {"reviewed", reviewed}};
    fixture.call(request, false);
    CHECK(fixture.call({{"op", "operations"}})["operations"].empty());
    auto current = request;
    current["reviewed"] = fixture.call({{"op", "query"}})["photos"];
    fixture.call(current);
    CHECK(fs::exists(fixture.root / "a/one.jpg"));
    CHECK(fs::exists(fixture.root / "a/two.jpg"));
}

TEST_CASE("unresolved location identities remain in Activity after restart") {
    Fixture fixture;
    fixture.file("a/one.jpg");
    fixture.add("a");
    fixture.drain();
    const auto photo = fixture.call({{"op", "query"}})["photos"][0];
    fixture.call({{"op", "edit"},
                  {"ids", Json::array({photo["id"]})},
                  {"description", "retained annotation"}});
    fixture.file("elsewhere.jpg", "different inode and bytes");
    const auto result = fixture.call(
        {{"op", "reconcile_locations"},
         {"kind", "move"},
         {"items", Json::array({{{"from", photo["path"]},
                                 {"to", (fixture.root / "elsewhere.jpg").string()}}})}});
    REQUIRE(result["review"].size() == 1);
    const auto operations = fixture.call({{"op", "operations"}})["operations"];
    REQUIRE(operations.size() == 1);
    CHECK(operations[0]["kind"] == "location");
    CHECK(operations[0]["state"] == "needs_review");
    CHECK(operations[0]["photos"][0]["description"] == "retained annotation");
    CHECK(operations[0]["results"][0]["success"] == false);
    CHECK(fixture.call({{"op", "query"}})["photos"][0]["path"] == photo["path"]);
}

TEST_CASE("photo query sorts real file columns globally with safe explicit directions") {
    Fixture fixture;
    const auto first = fixture.file("a/z.jpg", "1");
    const auto second = fixture.file("b/a.jpg", "222");
    const auto third = fixture.file("a/nested/m.jpg", "22");
    const auto time = fs::last_write_time(first);
    fs::last_write_time(second, time + std::chrono::seconds(1));
    fs::last_write_time(third, time + std::chrono::nanoseconds(1));
    fixture.add("a");
    fixture.add("b");
    fixture.drain();
    const auto names = [&](const std::string& sort, bool descending) {
        const auto query =
            fixture.call({{"op", "query"}, {"sort", sort}, {"descending", descending}});
        std::vector<std::string> result;
        for (const auto& row : query["photos"])
            result.push_back(row["filename"]);
        return result;
    };
    const std::vector<std::string> ascending{"z.jpg", "m.jpg", "a.jpg"};
    const std::vector<std::string> descending{"a.jpg", "m.jpg", "z.jpg"};
    for (const std::string sort : {"size", "modified", "location"}) {
        INFO(sort);
        CHECK(names(sort, false) == ascending);
        CHECK(names(sort, true) == descending);
        const auto page = fixture.call(
            {{"op", "query"}, {"sort", sort}, {"descending", false}, {"offset", 1}, {"limit", 1}});
        CHECK(page["photos"][0]["filename"] == "m.jpg");
        CHECK(page["total"] == 3);
    }
    CHECK(names("name", false) == std::vector<std::string>{"a.jpg", "m.jpg", "z.jpg"});
    CHECK(names("name", true) == std::vector<std::string>{"z.jpg", "m.jpg", "a.jpg"});
    const auto rows = fixture.call({{"op", "query"}})["photos"];
    fixture.call({{"op", "edit"},
                  {"ids", Json::array({rows[0]["id"]})},
                  {"capture_date", "2000-01-01"},
                  {"rating", 5}});
    CHECK(names("date", false).front() == "a.jpg");
    CHECK(names("date", true).back() == "a.jpg");
    CHECK(names("rating", false).back() == "a.jpg");
    CHECK(names("rating", true).front() == "a.jpg");
    // Capture-date edits must not change the ordering of filesystem modification times.
    CHECK(names("modified", false) == ascending);
    fixture.call({{"op", "query"}, {"sort", "name; DROP TABLE photos"}}, false);
    CHECK(fixture.call({{"op", "query"}})["total"] == 3);
}

TEST_CASE("photo album creation atomically includes the selected photos") {
    Fixture fixture;
    for (int i = 0; i < 15; ++i)
        fixture.file("source/photo-" + std::to_string(i) + ".jpg");
    fixture.add("source");
    fixture.drain();
    const auto photos = fixture.call({{"op", "query"}})["photos"];
    Json ids = Json::array();
    for (std::size_t i = 0; i < 13; ++i)
        ids.push_back(photos[i]["id"]);
    const auto created =
        fixture.call({{"op", "create_album"}, {"name", "Selected thirteen"}, {"ids", ids}});
    CHECK(created["count"] == 13);
    const auto members = fixture.call({{"op", "query"}, {"album_id", created["id"]}});
    CHECK(members["total"] == 13);
    Json actual = Json::array();
    for (const auto& photo : members["photos"])
        actual.push_back(photo["id"]);
    CHECK(actual == ids);
    const auto before = fixture.call({{"op", "albums"}})["albums"];
    fixture.call({{"op", "create_album"},
                  {"name", "Selected thirteen"},
                  {"ids", Json::array({photos[14]["id"]})}},
                 false);
    CHECK(fixture.call({{"op", "albums"}})["albums"] == before);
    CHECK(fixture.call({{"op", "query"}, {"album_id", created["id"]}})["photos"] ==
          members["photos"]);

    // An invalid ID after a valid membership must roll back both the new album and membership.
    fixture.call({{"op", "create_album"},
                  {"name", "Retry safely"},
                  {"ids", Json::array({photos[0]["id"], 999999999})}},
                 false);
    CHECK(fixture.call({{"op", "albums"}})["albums"] == before);
    CHECK(fixture.call({{"op", "query"}, {"album_id", created["id"]}})["photos"] ==
          members["photos"]);
    const auto retried = fixture.call({{"op", "create_album"},
                                       {"name", "Retry safely"},
                                       {"ids", Json::array({photos[0]["id"], photos[0]["id"]})}});
    CHECK(retried["count"] == 1);
    CHECK(fixture.call({{"op", "query"}, {"album_id", retried["id"]}})["total"] == 1);
    CHECK(fixture.call({{"op", "create_album"}, {"name", "Empty"}})["count"] == 0);
    CHECK(fixture.call({{"op", "create_album"},
                        {"name", "Explicit empty"},
                        {"ids", Json::array()}})["count"] == 0);
    for (const auto& invalid : {Json(nullptr), Json("one"), Json::array({-1}), Json::array({1.5})})
        fixture.call({{"op", "create_album"}, {"name", "Invalid"}, {"ids", invalid}}, false);
    CHECK(fixture.call({{"op", "albums"}})["albums"].size() == 4);
}

TEST_CASE("metadata batches isolate invalid results and report changed photo IDs") {
    Fixture fixture;
    fixture.file("source/first.jpg");
    fixture.file("source/second.jpg");
    fixture.add("source");
    const auto work = fixture.call({{"op", "work"}, {"budget", 64}});
    CHECK(work["discovery_changed"] == true);
    REQUIRE(work["metadata"].size() == 2);
    Json updates = Json::array();
    for (const auto& row : work["metadata"]) {
        const auto claim = fixture.call(
            {{"op", "metadata_claim"}, {"id", row["id"]}, {"signature", row["signature"]}});
        REQUIRE(claim["allowed"] == true);
        updates.push_back({{"id", row["id"]},
                           {"signature", row["signature"]},
                           {"claim_token", claim["claim_token"]},
                           {"width", 12}});
    }
    updates[0]["capture_date"] = "invalid extracted date";
    const auto result = fixture.call({{"op", "metadata_batch"}, {"items", updates}});
    REQUIRE(result["results"].size() == 2);
    CHECK(result["results"][0]["ok"] == false);
    CHECK(result["results"][1]["ok"] == true);
    CHECK(result["changed_ids"].size() == 2);
    CHECK(fixture.call({{"op", "metadata_pending"}})["metadata"].empty());
    const auto patched = fixture.call({{"op", "query_ids"}, {"ids", result["changed_ids"]}});
    CHECK(patched["photos"][0]["availability"] == "unreadable");
    CHECK(patched["photos"][1]["availability"] == "online");
    const auto progress = fixture.call({{"op", "progress"}});
    REQUIRE(progress["jobs"].size() == 1);
    CHECK(progress["jobs"][0]["failure_count"] == 1);
    CHECK(progress["jobs"][0]["failures"].empty());
    CHECK(progress["jobs"][0]["in_flight"] == 0);
    CHECK(progress["job_counts"]["paused"] == 0);
}

TEST_CASE("restore rejects invalid source settings atomically and leaves new sources inert") {
    Fixture fixture;
    fixture.file("source/photo.jpg");
    fixture.add("source");
    fixture.drain();
    auto backup = fixture.call({{"op", "export"}})["data"];
    const auto encoded = fixture.call({{"op", "export"}, {"encoded", true}});
    CHECK(Json::parse(encoded["json"].get<std::string>()) == backup);
    CHECK(encoded["bytes"] == encoded["json"].get<std::string>().size());
    Fixture destination;
    for (const auto& exclusions :
         {Json::array({7}), Json::array({"../escape"}), Json::array({"/absolute"})}) {
        auto invalid = backup;
        invalid["sources"][0]["exclusions"] = exclusions;
        destination.call({{"op", "restore"}, {"data", invalid}}, false);
        CHECK(destination.call({{"op", "sources"}})["sources"].empty());
        CHECK(destination.call({{"op", "query"}})["photos"].empty());
    }
    const auto restored = destination.call({{"op", "restore"}, {"data", backup}});
    CHECK(restored["restored"] == 1);
    CHECK(restored["sources_requiring_refresh"].size() == 1);
    CHECK(restored["merge_policy"] == "replace_scalar_annotations_merge_tags_and_albums");
    CHECK(destination.call({{"op", "sources"}})["sources"][0]["watch_enabled"] == false);
    CHECK(destination.call({{"op", "watch_paths"}})["paths"].empty());
    CHECK(destination.call({{"op", "refresh"}, {"reconcile", true}})["job_ids"].empty());
    CHECK(destination.call({{"op", "refresh"}})["job_ids"].size() == 1);
    CHECK(destination.call({{"op", "sources"}})["sources"][0]["watch_enabled"] == true);
}

TEST_CASE("retained associations remain reviewable and album counts match visible photos") {
    Fixture fixture;
    fixture.file("source/photo.jpg");
    const auto source = fixture.add("source")["source"]["id"];
    fixture.drain();
    const auto id = fixture.call({{"op", "query"}})["photos"][0]["id"];
    fixture.call({{"op", "edit"}, {"ids", Json::array({id})}, {"add_tags", Json::array({"kept"})}});
    const auto album =
        fixture.call({{"op", "create_album"}, {"name", "Keep"}, {"ids", Json::array({id})}});
    fixture.call({{"op", "remove_source"}, {"id", source}});
    const auto albums = fixture.call({{"op", "albums"}})["albums"];
    CHECK(albums[0]["count"] == 0);
    CHECK(albums[0]["retained_count"] == 1);
    CHECK(fixture.call({{"op", "query"}, {"album_id", album["id"]}})["total"] == 0);
    const auto retained =
        fixture.call({{"op", "query"}, {"album_id", album["id"]}, {"retained_only", true}});
    CHECK(retained["total"] == 1);
    CHECK(retained["photos"][0]["tags"] == Json::array({"kept"}));
    CHECK(retained["photos"][0]["availability"] == "inactive");
    const auto hidden = fixture.call({{"op", "query_ids"}, {"ids", Json::array({id})}});
    CHECK(hidden["photos"].empty());
    CHECK(hidden["missing_ids"] == Json::array({id}));
    CHECK(fixture
              .call({{"op", "query_ids"},
                     {"ids", Json::array({id})},
                     {"include_inactive", true}})["photos"]
              .size() == 1);
}

TEST_CASE("duplicate groups expose stable pagination and explicit verification bounds") {
    Fixture fixture;
    for (int group = 0; group < 3; ++group)
        for (int copy = 0; copy < 2; ++copy)
            fixture.file("source/" + std::to_string(group) + "-" + std::to_string(copy) + ".jpg",
                         "group-" + std::to_string(group));
    fixture.add("source");
    fixture.drain();
    fixture.call({{"op", "find_duplicates"}});
    fixture.drain();
    std::set<std::string> keys;
    for (int offset = 0; offset < 3; ++offset) {
        const auto page = fixture.call({{"op", "duplicates"}, {"limit", 1}, {"offset", offset}});
        CHECK(page["total"] == 3);
        REQUIRE(page["groups"].size() == 1);
        CHECK(keys.insert(page["groups"][0]["key"].get<std::string>()).second);
        CHECK(page["more"] == (offset < 2));
    }
    CHECK(fixture.call({{"op", "duplicates"}, {"offset", 3}})["groups"].empty());
    Json ids = Json::array();
    ids.get_ref<Json::array_t&>().resize(1001, Json(1));
    CHECK(fixture.call({{"op", "verify_duplicates"}, {"ids", ids}}, false)["error"]
              .get<std::string>()
              .find("1000") != std::string::npos);
}

TEST_CASE("source availability transitions invalidate visible rows and subscriptions") {
    Fixture fixture;
    fixture.file("source/photo.jpg");
    fixture.add("source");
    fixture.drain();
    fs::rename(fixture.root / "source", fixture.root / "disconnected");
    const auto job = fixture.call({{"op", "refresh"}})["job_ids"][0];
    const auto offline = fixture.call({{"op", "work"}});
    CHECK(offline["waiting"] == true);
    CHECK(offline["discovery_changed"] == true);
    CHECK(offline["topology_changed"] == true);
    fs::rename(fixture.root / "disconnected", fixture.root / "source");
    fixture.call({{"op", "job_control"}, {"id", job}, {"action", "retry"}});
    const auto online = fixture.call({{"op", "work"}, {"budget", 64}});
    CHECK(online["discovery_changed"] == true);
    CHECK(online["topology_changed"] == true);
    CHECK(online["changed_ids"].empty());
    CHECK(fixture.call({{"op", "query"}})["photos"][0]["availability"] == "online");
}
