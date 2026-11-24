#include "rummage/platform/code_stats.hpp"

#include <chrono>
#include <fstream>
#include <functional>
#include <string>

#include <doctest/doctest.h>

#include "temp_dir.hpp"

namespace {
using namespace rummage::platform;
using namespace std::chrono_literals;
constexpr std::string_view example =
    R"({"Markdown":{"blanks":1,"code":0,"comments":3,"reports":[{"stats":{"blanks":1,"code":0,"comments":3,"blobs":{"Python":{"blanks":0,"code":1,"comments":1,"blobs":{}}}},"name":"./guide.md"}],"children":{"Python":[]},"inaccurate":false},"Rust":{"blanks":1,"code":1,"comments":1,"reports":[{"stats":{"blanks":1,"code":1,"comments":1,"blobs":{}},"name":"./sub/main.rs"}],"children":{},"inaccurate":false},"Total":{"blanks":2,"code":2,"comments":5,"reports":[],"children":{},"inaccurate":false}})";

bool until(rummage::core::EventLoop& loop, const std::function<bool()>& done) {
    bool success = false;
    const auto poll = loop.add_timer(1ms, 1ms, [&] {
        if (done()) {
            success = true;
            loop.quit(0);
        }
    });
    const auto timeout = loop.add_timer(5s, 0ms, [&] { loop.quit(1); });
    loop.run();
    loop.remove(poll);
    loop.remove(timeout);
    return success;
}
}  // namespace

TEST_CASE(
    "source stats: subprocess uses requested scope and returns errors without stale results") {
    rummage::test::TempDir dir;
    dir.write("report.json", std::string{example});
    rummage::core::EventLoop loop;
    rummage::core::WorkerPool pool{loop, 1};
    CodeAnalysis analysis{loop, pool, RUMMAGE_CODE_FIXTURE, 250ms};
    std::shared_ptr<const CodeReport> result;
    const auto run = [&](bool recursive = true, bool hidden = false) {
        result.reset();
        analysis.start(dir.path, recursive, hidden,
                       [&](auto report) { result = std::move(report); });
        REQUIRE(until(loop, [&] { return result != nullptr; }));
        CHECK_FALSE(analysis.running());
    };
    run();
    REQUIRE(result->error == CodeError::none);
    CHECK(result->files == 2);
    dir.write("hidden", "");
    dir.write("shallow", "");
    run(false, true);
    CHECK(result->error == CodeError::none);
    run();  // The fixture rejects incorrect flags.
    CHECK(result->error == CodeError::failed);
    std::filesystem::remove(dir.path / "hidden");
    std::filesystem::remove(dir.path / "shallow");
    dir.write("report.json", "{}");
    run();
    CHECK(result->error == CodeError::invalid_output);
    dir.write("sleep", "");
    run();
    CHECK(result->error == CodeError::timed_out);
    result.reset();
    analysis.start(dir.path, true, false, [&](auto report) { result = std::move(report); });
    analysis.stop();
    REQUIRE(until(loop, [&] { return result != nullptr; }));
    CHECK(result->error == CodeError::stopped);
    result.reset();
    analysis.start(dir.path, true, false, [&](auto report) { result = std::move(report); });
    analysis.cancel();
    CHECK_FALSE(analysis.running());
    CHECK(result == nullptr);
    std::filesystem::remove(dir.path / "sleep");
    dir.write("oversize", "");
    CodeAnalysis large{loop, pool, RUMMAGE_CODE_FIXTURE};
    large.start(dir.path, true, false, [&](auto report) { result = std::move(report); });
    REQUIRE(until(loop, [&] { return result != nullptr; }));
    CHECK(result->error == CodeError::output_limit);
    analysis.start(dir.path / "missing", true, false,
                   [&](auto report) { result = std::move(report); });
    CHECK(result->error == CodeError::invalid_root);
    CHECK_FALSE(analysis.running());
}
