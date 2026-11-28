#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>
#include <doctest/doctest.h>
#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
using namespace std::chrono_literals;
using rummage::core::EventLoop;
using rummage::core::WorkerPool;
using rummage::core::CancelToken;
using rummage::core::Priority;

TEST_CASE("tsan stress: concurrent cancellation and queued streaming deliveries survive teardown") {
    for (int iteration = 0; iteration < 20; ++iteration) {
        EventLoop loop;
        loop.add_timer(20s, 0ms, [&] { FAIL_CHECK("stream cancellation watchdog"); loop.quit(1); });
        std::atomic<int> started{0}, stopped{0};
        std::atomic<unsigned> emitted{0};
        std::array<CancelToken, 4> tokens;
        int partials = 0, completed = 0;
        {
            WorkerPool pool{loop, 4};
            for (int job = 0; job < 4; ++job) {
                tokens[static_cast<std::size_t>(job)] = pool.submit_streaming(
                    Priority::visible,
                    [&](const CancelToken& token, auto& emit) {
                        ++started;
                        while (!token.cancelled()) {
                            emit(1);
                            ++emitted;
                            std::this_thread::yield();
                        }
                        ++stopped;
                        return 0;
                    },
                    [&](int value) { partials += value; },
                    [&](int) { ++completed; });
            }
            std::thread canceller([&] {
                while (started.load() != 4 || emitted.load() < 16) std::this_thread::yield();
                for (const auto& token : tokens) token.cancel();
                loop.post([&] { loop.quit(0); });
            });
            CHECK(loop.run() == 0);
            canceller.join();
        } // Join every worker before destroying captured state; suppress any posted delivery.
        CHECK(stopped.load() == 4);
        CHECK(completed == 0);
        const int before_drain = partials;
        loop.post([&] { loop.quit(0); });
        CHECK(loop.run() == 0);
        CHECK(partials == before_drain);
    }
}

TEST_CASE("tsan stress: eight concurrent producers post every callback exactly once") {
    for (int iteration = 0; iteration < 10; ++iteration) {
        EventLoop loop;
        loop.add_timer(20s, 0ms, [&] { FAIL_CHECK("multi-producer watchdog"); loop.quit(1); });
        constexpr int producers = 8, count = 512;
        std::vector<int> seen(producers * count, 0);
        int delivered = 0;
        std::vector<std::thread> threads;
        for (int producer = 0; producer < producers; ++producer) {
            threads.emplace_back([&, producer] {
                for (int item = 0; item < count; ++item) {
                    const auto index = static_cast<std::size_t>(producer * count + item);
                    loop.post([&, index] {
                        ++seen[index];
                        if (++delivered == producers * count) loop.quit(0);
                    });
                }
            });
        }
        CHECK(loop.run() == 0);
        for (auto& thread : threads) thread.join();
        for (int visits : seen) CHECK(visits == 1);
    }
}

TEST_CASE("tsan stress: pool destruction suppresses completed streaming callbacks holding dead owners") {
    for (int iteration = 0; iteration < 20; ++iteration) {
        EventLoop loop;
        int delivered = 0;
        auto owner = std::make_unique<int>(123);
        {
            WorkerPool pool{loop, 4};
            for (int job = 0; job < 64; ++job) {
                pool.submit_streaming(Priority::nearby,
                    [](const CancelToken& token, auto& emit) {
                        for (int part = 0; part < 32 && !token.cancelled(); ++part) emit(part);
                        return 32;
                    },
                    [&, raw = owner.get()](int) { delivered += *raw; },
                    [&, raw = owner.get()](int) { delivered += *raw; });
            }
        }
        owner.reset();
        loop.post([&] { loop.quit(0); });
        CHECK(loop.run() == 0);
        CHECK(delivered == 0);
    }
}
