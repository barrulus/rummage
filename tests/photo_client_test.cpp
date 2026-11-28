#include <algorithm>
#include <filesystem>

#include <doctest/doctest.h>

#include "rummage/formats/image/photo.hpp"
#include "rummage/photos/client.hpp"
#include "rummage/photos/work_diagnostics.hpp"
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
    std::size_t failures = 0;
    for (int iteration = 0; iteration < 20; ++iteration) {
        result = client.request({{"op", "work"}, {"budget", 32}});
        REQUIRE_MESSAGE(result.value("ok", false), result.dump());
        processed += result.value("metadata_processed", std::size_t{0});
        failures += result.value("metadata_failed", std::size_t{0});
        if (result.value("metadata_failed", std::size_t{0}) > 0) {
            CHECK_FALSE(result.value("metadata_failure_reason", std::string{}).empty());
            CHECK(result.at("metadata_failure_reason").get<std::string>().size() <= 240);
        }
        if (!result.value("worked", false))
            break;
    }
    CHECK(processed == 3);
    CHECK(failures == 2);
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

TEST_CASE("photos diagnostics: bounded work checkpoints, idle and pause transitions") {
    using Diagnostics = rummage::photos::detail::WorkDiagnostics;
    using Kind = Diagnostics::Kind;
    using Json = nlohmann::json;
    Diagnostics state;
    const auto origin = Diagnostics::Clock::time_point{};
    const Json work{{"op", "work"}};
    const auto observe = [&](Json response, int seconds) {
        return state.finish(work, response, state.begin(work),
                            origin + std::chrono::seconds{seconds});
    };
    CHECK(observe({{"ok", true}, {"worked", false}}, 0).empty());
    auto events =
        observe({{"ok", true}, {"worked", true}, {"processed", 64}, {"metadata_processed", 4}}, 1);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == Kind::start);
    for (int i = 2; i <= 30; ++i)
        CHECK(observe({{"ok", true}, {"worked", true}, {"processed", 1}}, i).empty());
    events = observe({{"ok", true}, {"worked", true}, {"metadata_processed", 2}}, 31);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == Kind::checkpoint);
    CHECK(events[0].message.find("entries processed=93") != std::string::npos);
    CHECK(events[0].message.find("metadata processed=6") != std::string::npos);
    CHECK(observe({{"ok", true}, {"worked", false}, {"busy", true}}, 32).empty());
    events = observe({{"ok", true}, {"worked", false}}, 33);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == Kind::stop);
    CHECK(events[0].message.find("idle") != std::string::npos);
    CHECK(observe({{"ok", true}, {"worked", false}}, 34).empty());

    REQUIRE(observe({{"ok", true}, {"worked", true}}, 35).size() == 1);
    const auto pending = state.begin(work);
    const Json pause{{"op", "background_control"}, {"paused", true}};
    events = state.finish(pause, {{"ok", true}, {"paused", true}}, state.begin(pause),
                          origin + std::chrono::seconds{36});
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == Kind::stop);
    CHECK(events[0].message.find("paused") != std::string::npos);
    CHECK(state
              .finish(work, {{"ok", true}, {"worked", true}}, pending,
                      origin + std::chrono::seconds{37})
              .empty());
    CHECK(observe({{"ok", true}, {"paused", true}, {"worked", false}}, 38).empty());
    const Json resume{{"op", "background_control"}, {"paused", false}};
    CHECK(state
              .finish(resume, {{"ok", true}, {"paused", false}}, state.begin(resume),
                      origin + std::chrono::seconds{39})
              .empty());
    events = observe({{"ok", true}, {"worked", true}, {"processed", 1}}, 40);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == Kind::start);
    events = state.shutdown(origin + std::chrono::seconds{41});
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == Kind::stop);
    CHECK(events[0].message.find("entries processed=1") != std::string::npos);
    CHECK(state.shutdown(origin + std::chrono::seconds{42}).empty());
}

TEST_CASE(
    "photos diagnostics: failures aggregate with bounded reasons and cancellation is normal") {
    using Diagnostics = rummage::photos::detail::WorkDiagnostics;
    using Kind = Diagnostics::Kind;
    Diagnostics state;
    const nlohmann::json request{{"op", "query"}};
    const auto origin = Diagnostics::Clock::time_point{};
    for (int seconds = 0; seconds <= 30; ++seconds) {
        const auto events =
            state.finish(request, {{"ok", false}, {"error", "database is busy\nretry"}},
                         state.begin(request), origin + std::chrono::seconds{seconds});
        if (seconds != 0 && seconds != 30) {
            CHECK(events.empty());
            continue;
        }
        REQUIRE(events.size() == 1);
        CHECK(events[0].kind == Kind::error);
        CHECK(events[0].message.find(seconds == 0 ? "request failures=1 "
                                                  : "request failures=30 ") != std::string::npos);
        CHECK(events[0].message.find("reason=database is busy retry") != std::string::npos);
    }
    const nlohmann::json work{{"op", "work"}};
    auto events = state.finish(work,
                               {{"ok", true},
                                {"worked", true},
                                {"metadata_processed", 4},
                                {"metadata_failed", 2},
                                {"metadata_results",
                                 {{{"ok", false}, {"error", "Invalid extracted capture date"}},
                                  {{"ok", true}, {"stale", true}}}}},
                               state.begin(work), origin + std::chrono::seconds{31});
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == Kind::start);
    events = state.finish(work, {{"ok", false}, {"error", "cancelled"}}, state.begin(work),
                          origin + std::chrono::seconds{32}, true);
    REQUIRE(events.size() == 2);
    CHECK(events[0].kind == Kind::error);
    CHECK(events[0].message.find("request failures=0") != std::string::npos);
    CHECK(events[0].message.find("metadata failures=3") != std::string::npos);
    CHECK(events[0].message.find("changed or unavailable originals=1") != std::string::npos);
    CHECK(events[0].message.find("Invalid extracted capture date") != std::string::npos);
    CHECK(events[1].kind == Kind::stop);

    Diagnostics bounded;
    events = bounded.finish({{"op", "/private/arbitrary/operation"}},
                            {{"ok", false}, {"error", std::string(10000, 'x')}}, 0, origin);
    REQUIRE(events.size() == 1);
    CHECK(events[0].message.size() < 500);
    CHECK(events[0].message.find("/private") == std::string::npos);
}

TEST_CASE(
    "photos diagnostics: shared catalogs and overlapping workers do not duplicate transitions") {
    using Shared = rummage::photos::detail::SharedWorkDiagnostics;
    using Diagnostics = rummage::photos::detail::WorkDiagnostics;
    auto first = Shared::for_catalog("/nonexistent/diagnostics/catalog.sqlite3");
    auto second = Shared::for_catalog("/nonexistent/diagnostics/./catalog.sqlite3");
    CHECK(first == second);
    CHECK(first != Shared::for_catalog("/nonexistent/other/catalog.sqlite3"));
    const nlohmann::json work{{"op", "work"}};
    const auto origin = Diagnostics::Clock::time_point{};
    const auto one = first->state.begin(work);
    const auto two = second->state.begin(work);
    auto events = first->state.finish(work, {{"ok", true}, {"worked", true}}, one, origin);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == Diagnostics::Kind::start);
    CHECK(second->state.finish(work, {{"ok", true}, {"worked", true}}, two, origin).empty());
    const auto slow = first->state.begin(work);
    const auto idle = second->state.begin(work);
    CHECK(second->state.finish(work, {{"ok", true}, {"worked", false}}, idle, origin).empty());
    events = first->state.finish(work, {{"ok", true}, {"worked", false}}, slow, origin);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == Diagnostics::Kind::stop);
    std::weak_ptr<Shared> expired = first;
    first.reset();
    second.reset();
    CHECK(expired.expired());
}

TEST_CASE("photos diagnostics: old cancelled work cannot stop a resumed run") {
    using Diagnostics = rummage::photos::detail::WorkDiagnostics;
    using Kind = Diagnostics::Kind;
    const auto now = Diagnostics::Clock::time_point{};
    const nlohmann::json work{{"op", "work"}};
    for (const bool cancelled : {false, true}) {
        Diagnostics state;
        const auto old = state.begin(work);
        const nlohmann::json resume{{"op", "background_control"}, {"paused", false}};
        CHECK(state.finish(resume, {{"ok", true}, {"paused", false}}, state.begin(resume), now)
                  .empty());
        auto events = state.finish(work, {{"ok", true}, {"worked", true}}, state.begin(work), now);
        REQUIRE(events.size() == 1);
        CHECK(events[0].kind == Kind::start);
        events = state.finish(work, {{"ok", false}, {"error", "request cancelled"}}, old, now,
                              cancelled);
        CHECK(std::none_of(events.begin(), events.end(),
                           [](const auto& event) { return event.kind == Kind::stop; }));
        events = state.finish(work, {{"ok", true}, {"worked", false}}, state.begin(work), now);
        REQUIRE(events.size() == 1);
        CHECK(events[0].kind == Kind::stop);
    }
}

TEST_CASE("photos diagnostics: unavailable sources are visible without catalog history polling") {
    using Diagnostics = rummage::photos::detail::WorkDiagnostics;
    Diagnostics state;
    const nlohmann::json work{{"op", "work"}};
    const auto events = state.finish(work, {{"ok", true}, {"worked", true}, {"waiting", true}},
                                     state.begin(work), Diagnostics::Clock::time_point{});
    REQUIRE(events.size() == 2);
    CHECK(events[0].kind == Diagnostics::Kind::start);
    CHECK(events[1].kind == Diagnostics::Kind::error);
    CHECK(events[1].message.find("unavailable sources=1") != std::string::npos);
    CHECK(events[1].message.find("reconnect it and retry") != std::string::npos);
}

TEST_CASE("photos diagnostics: closing an idle client flushes suppressed request failures") {
    using Diagnostics = rummage::photos::detail::WorkDiagnostics;
    Diagnostics state;
    const nlohmann::json request{{"op", "query"}};
    const auto now = Diagnostics::Clock::time_point{};
    REQUIRE(
        state
            .finish(request, {{"ok", false}, {"error", "database busy"}}, state.begin(request), now)
            .size() == 1);
    CHECK(
        state
            .finish(request, {{"ok", false}, {"error", "database busy"}}, state.begin(request), now)
            .empty());
    const auto events = state.shutdown(now);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == Diagnostics::Kind::error);
    CHECK(events[0].message.find("request failures=1 ") != std::string::npos);
}

TEST_CASE("photos diagnostics: successful work replies retain bounded actionable failure reasons") {
    using Diagnostics = rummage::photos::detail::WorkDiagnostics;
    Diagnostics state;
    const nlohmann::json work{{"op", "work"}};
    const auto now = Diagnostics::Clock::time_point{};
    auto events = state.finish(work,
                               {{"ok", true},
                                {"worked", true},
                                {"work_errors", 3},
                                {"work_error", "Cannot read directory: Permission denied"}},
                               state.begin(work), now);
    REQUIRE(events.size() == 2);
    CHECK(events[0].kind == Diagnostics::Kind::start);
    CHECK(events[1].kind == Diagnostics::Kind::error);
    CHECK(events[1].message.find("work failures=3") != std::string::npos);
    CHECK(events[1].message.find("Permission denied") != std::string::npos);
    CHECK(state
              .finish(work,
                      {{"ok", true},
                       {"worked", true},
                       {"work_errors", 2},
                       {"work_error", "Original changed while fingerprinting"}},
                      state.begin(work), now + std::chrono::seconds{1})
              .empty());
    events =
        state.finish(work,
                     {{"ok", true},
                      {"worked", true},
                      {"work_errors", 1},
                      {"work_error", "Cannot read directory"},
                      {"metadata_failed", 2},
                      {"metadata_failure_reason", "Unsupported image encoding\nJPEG lossless"}},
                     state.begin(work), now + std::chrono::seconds{30});
    REQUIRE(events.size() == 2);
    CHECK(events[0].kind == Diagnostics::Kind::error);
    CHECK(events[0].message.find("work failures=3") != std::string::npos);
    CHECK(events[0].message.find("metadata failures=2") != std::string::npos);
    CHECK(events[0].message.find("reason=Unsupported image encoding JPEG lossless") !=
          std::string::npos);
    CHECK(events[1].kind == Diagnostics::Kind::checkpoint);

    const auto text =
        rummage::photos::detail::bounded_diagnostic_reason(std::string(239, 'a') + "é suffix");
    CHECK(text.size() == 239);
    CHECK_NOTHROW(nlohmann::json(text).dump());
}
