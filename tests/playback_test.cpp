#include "rummage/isolate/playback.hpp"

#include <chrono>
#include <filesystem>
#include <thread>

#include <doctest/doctest.h>

#include "temp_dir.hpp"

namespace {
using rummage::isolate::Playback;
using namespace std::chrono_literals;
const std::filesystem::path data{RUMMAGE_MEDIA_DATA};

bool until(const auto& condition) {
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition())
            return true;
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

Playback::Programs helpers() {
    return {.decoder = RUMMAGE_PLAYBACK_HELPER, .audio = "/nonexistent/rummage-audio"};
}
}  // namespace

TEST_CASE("playback: paused first frame, playback, pause, seek and replay") {
    Playback value{data / "red.mp4", helpers()};
    REQUIRE(until([&] { return !value.snapshot().loading; }));
    auto state = value.snapshot();
    REQUIRE(state.error.empty());
    REQUIRE(state.image);
    CHECK(state.image->width == 96);
    CHECK(state.paused);
    CHECK(state.position_ms == 0);
    value.pause(false);
    REQUIRE(until([&] { return value.snapshot().position_ms >= 100; }));
    value.pause(true);
    std::this_thread::sleep_for(80ms);
    const auto paused = value.snapshot().position_ms;
    std::this_thread::sleep_for(80ms);
    CHECK(value.snapshot().position_ms == paused);
    value.seek(0);
    REQUIRE(until([&] { return !value.snapshot().loading && value.snapshot().position_ms == 0; }));
    CHECK(value.snapshot().paused);
    value.pause(false);
    REQUIRE(until([&] { return value.snapshot().ended; }));
    REQUIRE(until([&] { return value.snapshot().paused; }));
    value.pause(false);
    REQUIRE(until([&] { return !value.snapshot().ended && value.snapshot().position_ms < 200; }));
}

TEST_CASE("playback: volume clamps and absent audio output keeps video running") {
    Playback value{data / "metadata.mp4", helpers()};
    value.volume(-100);
    CHECK(value.volume() == 0);
    value.volume(200);
    CHECK(value.volume() == 100);
    value.volume(0);
    REQUIRE(until([&] { return !value.snapshot().loading; }));
    CHECK(value.snapshot().error.empty());
    value.pause(false);
    REQUIRE(until([&] { return value.snapshot().error == "Audio output unavailable"; }));
    CHECK_FALSE(value.snapshot().failed);
    REQUIRE(value.snapshot().image);
    REQUIRE(until([&] { return value.snapshot().ended; }));
}

TEST_CASE("playback: missing and damaged input or missing backend fail gracefully") {
    rummage::test::TempDir temporary;
    for (const auto& path :
         {temporary.path / "missing.mp4", temporary.write("bad.mp4", "broken")}) {
        Playback value{path, helpers()};
        REQUIRE(until([&] { return !value.snapshot().loading; }));
        CHECK(value.snapshot().failed);
        CHECK_FALSE(value.snapshot().error.empty());
        value.pause(false);
        CHECK(value.snapshot().paused);
    }
    Playback missing{data / "red.mp4", {.decoder = "/nonexistent/rummage-playback", .audio = {}}};
    REQUIRE(until([&] { return !missing.snapshot().loading; }));
    CHECK(missing.snapshot().error == "Playback backend unavailable");
}

TEST_CASE("playback: repeated close and rapid pending seek release the session promptly") {
    for (int attempt = 0; attempt < 8; ++attempt) {
        auto value = std::make_unique<Playback>(data / "fade.mp4", helpers());
        value->pause(false);
        value->seek(2000);
        value->seek(0);
        value->seek(1000);
        const auto start = std::chrono::steady_clock::now();
        value.reset();
        CHECK(std::chrono::steady_clock::now() - start < 500ms);
    }
}

TEST_CASE("playback: untrusted response bounds and seals are checked before pixels are used") {
    rummage::test::TempDir dir;
    for (const auto* mode : {"oversized", "unknown", "unsealed"}) {
        const auto program = dir.path / mode;
        std::filesystem::create_symlink(RUMMAGE_FAKE_PLAYBACK, program);
        Playback value{data / "red.mp4", {.decoder = program, .audio = {}}};
        REQUIRE(until([&] { return !value.snapshot().loading; }));
        CHECK(value.snapshot().failed);
        CHECK_FALSE(value.snapshot().image);
    }
}

TEST_CASE("playback: closing interrupts a decoder that never responds") {
    rummage::test::TempDir dir;
    const auto program = dir.path / "hang";
    std::filesystem::create_symlink(RUMMAGE_FAKE_PLAYBACK, program);
    auto value = std::make_unique<Playback>(data / "red.mp4",
                                            Playback::Programs{.decoder = program, .audio = {}});
    std::this_thread::sleep_for(100ms);
    CHECK(value->snapshot().loading);
    const auto start = std::chrono::steady_clock::now();
    value.reset();
    CHECK(std::chrono::steady_clock::now() - start < 300ms);
}

TEST_CASE("playback: rapid seeks display the latest requested position and frame") {
    Playback value{data / "fade.mp4", helpers()};
    REQUIRE(until([&] { return !value.snapshot().loading; }));
    value.seek(1000);
    value.seek(0);
    value.seek(2000);
    REQUIRE(until([&] {
        const auto state = value.snapshot();
        return !state.loading && state.position_ms == 2000;
    }));
    const auto state = value.snapshot();
    CHECK(state.error.empty());
    REQUIRE(state.image);
    CHECK(state.image->rgba[0] > 220);
}
