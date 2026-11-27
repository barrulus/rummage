#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>

#include <doctest/doctest.h>

#include "rummage/photos/source_watcher.hpp"
#include "rummage/thumbs/cache.hpp"
#include "scoped_env.hpp"
#include "temp_dir.hpp"

namespace {
using Json = nlohmann::json;
using namespace std::chrono_literals;
namespace photos = rummage::photos;
namespace core = rummage::core;
namespace fs = std::filesystem;

bool run_until(core::EventLoop& loop, const std::function<bool()>& done) {
    bool ready = false;
    const auto poll = loop.add_timer(2ms, 2ms, [&] {
        if (done()) {
            ready = true;
            loop.quit();
        }
    });
    const auto timeout = loop.add_timer(5s, 0ms, [&] { loop.quit(); });
    loop.run();
    loop.remove(poll);
    loop.remove(timeout);
    return ready;
}

void run_for(core::EventLoop& loop, std::chrono::milliseconds time) {
    loop.add_timer(time, 0ms, [&] { loop.quit(); });
    loop.run();
}

struct Fixture {
    rummage::test::TempDir directory;
    rummage::test::ScopedEnv cache{"XDG_CACHE_HOME", (directory.path / "cache").string()};
    core::EventLoop loop;
    core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    std::shared_ptr<photos::Client> client{std::make_shared<photos::Client>(
        RUMMAGE_PHOTOS_TEST_MODULE, directory.path / "catalog/catalog.sqlite3")};
    std::unique_ptr<photos::SourceWatcher> observer;
    int changes{};
    std::vector<std::string> errors;

    Json call(Json request) {
        auto result = client->request(request);
        REQUIRE_MESSAGE(result.value("ok", false), result.dump());
        return result;
    }

    std::int64_t add(const fs::path& source) {
        fs::create_directories(source);
        return call({{"op", "add_source"}, {"path", source.string()}})
            .at("source")
            .at("id")
            .get<std::int64_t>();
    }

    void drain() {
        for (int iteration = 0; iteration < 2000; ++iteration) {
            auto result = call({{"op", "work"}, {"budget", 64}});
            // These tests exercise watch delivery and catalog identity, not image decoding.
            // Complete metadata explicitly when no image engine is installed in this test.
            for (const auto& item : result.value("metadata", Json::array())) {
                call({{"op", "metadata"},
                      {"id", item.at("id")},
                      {"signature", item.at("signature")},
                      {"width", 1},
                      {"height", 1}});
            }
            if (!result.value("worked", false) && result.value("metadata", Json::array()).empty())
                return;
        }
        FAIL("Photo jobs did not settle");
    }

    void observe(std::size_t count) {
        observer = std::make_unique<photos::SourceWatcher>(loop, pool, watcher, client,
                                                           [&](const std::string& error) {
                                                               if (error.empty())
                                                                   ++changes;
                                                               else
                                                                   errors.push_back(error);
                                                           });
        REQUIRE(run_until(loop, [&] {
            return observer->subscription_count() == count && !observer->refreshing() &&
                   changes > 0;
        }));
        drain();
        observer->refresh_subscriptions();
        REQUIRE(run_until(loop, [&] { return !observer->refreshing(); }));
        CHECK(errors.empty());
    }
};
}  // namespace

TEST_CASE("photo watcher discovers changes inside persisted empty directories") {
    Fixture fixture;
    const auto source = fixture.directory.path / "pictures";
    fs::create_directories(source / "empty/deep");
    fixture.add(source);
    fixture.drain();
    fixture.observe(3);
    const int before = fixture.changes;
    std::ofstream(source / "empty/deep/new.jpg") << "watch fixture";
    REQUIRE(run_until(fixture.loop, [&] { return fixture.changes > before; }));
    fixture.drain();
    CHECK(fixture.call({{"op", "query"}, {"text", "new"}}).at("total") == 1);
    const int directory_change = fixture.changes;
    fs::create_directory(source / "folder.with.dots");
    REQUIRE(run_until(fixture.loop, [&] { return fixture.changes > directory_change; }));
    fixture.drain();
    fixture.observer->refresh_subscriptions();
    REQUIRE(run_until(fixture.loop, [&] {
        return fixture.observer->subscription_count() == 4 && !fixture.observer->refreshing();
    }));
}

TEST_CASE("photo watcher coalesces overflow and releases inactive or disabled sources") {
    Fixture fixture;
    const auto source = fixture.directory.path / "pictures";
    const auto id = fixture.add(source);
    fixture.drain();
    fixture.observe(1);
    const auto jobs = fixture.call({{"op", "jobs"}}).at("jobs").size();
    const int before = fixture.changes;
    for (int event = 0; event < 20; ++event)
        fixture.observer->notify_change(id, rummage::fs::DirectoryEvent::Kind::overflow);
    REQUIRE(run_until(fixture.loop, [&] { return fixture.changes > before; }));
    CHECK(fixture.call({{"op", "jobs"}}).at("jobs").size() == jobs + 1);
    fixture.drain();
    fixture.call({{"op", "remove_source"}, {"source_id", id}});
    fixture.observer->refresh_subscriptions();
    REQUIRE(run_until(fixture.loop, [&] {
        return fixture.observer->subscription_count() == 0 && !fixture.observer->refreshing();
    }));
    const int removed = fixture.changes;
    std::ofstream(source / "after-removal.jpg") << "untracked";
    run_for(fixture.loop, 350ms);
    CHECK(fixture.changes == removed);
    fixture.add(source);
    fixture.drain();
    fixture.observer->refresh_subscriptions();
    REQUIRE(run_until(fixture.loop, [&] {
        return fixture.observer->subscription_count() == 1 && !fixture.observer->refreshing();
    }));
    fixture.observer->stop();
    const int stopped = fixture.changes;
    std::ofstream(source / "after-disable.jpg") << "untracked";
    run_for(fixture.loop, 350ms);
    CHECK(fixture.observer->subscription_count() == 0);
    CHECK(fixture.changes == stopped);
}

TEST_CASE("photo watcher ignores catalog, thumbnail cache and ordinary config writes") {
    Fixture fixture;
    const auto cache = rummage::thumbs::default_root();
    fs::create_directories(cache / "large");
    fs::create_directories(fixture.directory.path / "catalog");
    std::ofstream(cache / "large/cached.png") << "not an original";
    std::ofstream(fixture.directory.path / "catalog/internal.png") << "not an original";
    fixture.add(fixture.directory.path);
    fixture.drain();
    CHECK(fixture.call({{"op", "query"}}).at("total") == 0);
    const auto paths = fixture.call({{"op", "watch_paths"}}).at("paths");
    fixture.observe(paths.size());
    const auto jobs = fixture.call({{"op", "jobs"}}).at("jobs").size();
    const int before = fixture.changes;
    fixture.call({{"op", "create_album"}, {"name", "Catalog write"}});
    std::ofstream(cache / "large/another.png") << "not an original";
    std::ofstream(fixture.directory.path / "catalog/another.png") << "not an original";
    std::ofstream(fixture.directory.path / "config.toml") << "unrelated config";
    run_for(fixture.loop, 600ms);
    CHECK(fixture.changes == before);
    CHECK(fixture.call({{"op", "jobs"}}).at("jobs").size() == jobs);
    CHECK(fixture.call({{"op", "query"}}).at("total") == 0);
}

TEST_CASE("destroying a photo watcher cancels queued subscription delivery") {
    Fixture fixture;
    fixture.add(fixture.directory.path / "pictures");
    fixture.drain();
    fixture.observer = std::make_unique<photos::SourceWatcher>(
        fixture.loop, fixture.pool, fixture.watcher, fixture.client,
        [&](const std::string&) { ++fixture.changes; });
    fixture.observer.reset();
    run_for(fixture.loop, 300ms);
    CHECK(fixture.changes == 0);
}

TEST_CASE("photo watcher: restored sources remain read-only until an explicit refresh") {
    Fixture fixture;
    const auto source = fixture.directory.path / "pictures";
    fixture.add(source);
    std::ofstream{source / "existing.jpg"} << "fixture image";
    fixture.drain();
    const auto exported = fixture.call({{"op", "export"}}).at("data");
    fixture.client = std::make_shared<photos::Client>(
        RUMMAGE_PHOTOS_TEST_MODULE, fixture.directory.path / "restored/catalog.sqlite3");
    const auto restored = fixture.call({{"op", "restore"}, {"data", exported}});
    REQUIRE(restored.at("restored") == 1);
    const auto sources = fixture.call({{"op", "sources"}}).at("sources");
    REQUIRE(sources.size() == 1);
    CHECK_FALSE(sources[0].at("watch_enabled").get<bool>());
    fixture.observer = std::make_unique<photos::SourceWatcher>(
        fixture.loop, fixture.pool, fixture.watcher, fixture.client, [&](const std::string& error) {
            ++fixture.changes;
            fixture.errors.push_back(error);
        });
    REQUIRE(run_until(fixture.loop, [&] { return !fixture.observer->refreshing(); }));
    std::ofstream{source / "new.jpg"} << "new image";
    fixture.observer->refresh_subscriptions();  // same path as periodic availability checks
    run_for(fixture.loop, 250ms);
    CHECK(fixture.observer->subscription_count() == 0);
    CHECK(fixture.changes == 0);
    CHECK(fixture.call({{"op", "jobs"}}).at("jobs").empty());
    CHECK(fixture.call({{"op", "query"}}).at("total") == 1);
    fixture.call({{"op", "refresh"}, {"source_id", sources[0].at("id")}});
    fixture.observer->refresh_subscriptions();
    REQUIRE(run_until(fixture.loop, [&] {
        return fixture.observer->subscription_count() == 1 && !fixture.observer->refreshing();
    }));
    fixture.drain();
    CHECK(fixture.call({{"op", "query"}}).at("total") == 2);
}
