#include "rummage/core/event_loop.hpp"

#include <array>
#include <chrono>
#include <poll.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <doctest/doctest.h>

#include "../src/app/window_session.hpp"

using rummage::core::EventLoop;
using namespace std::chrono_literals;

namespace {

// Fails the test instead of hanging when a loop never quits.
void add_watchdog(EventLoop& loop) {
    loop.add_timer(5s, 0ms, [&loop] {
        FAIL_CHECK("event loop watchdog fired");
        loop.quit(-1);
    });
}

struct Pipe {
    std::array<int, 2> fds{-1, -1};

    Pipe() { REQUIRE(pipe(fds.data()) == 0); }

    ~Pipe() {
        close(fds[0]);
        close(fds[1]);
    }

    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;
    Pipe(Pipe&&) = delete;
    Pipe& operator=(Pipe&&) = delete;

    void write_byte() const { REQUIRE(write(fds[1], "x", 1) == 1); }

    void read_byte() const {
        char byte = 0;
        REQUIRE(read(fds[0], &byte, 1) == 1);
    }
};

}  // namespace

TEST_CASE("windows: closing resumes its scope after the handler and only once") {
    EventLoop loop;
    add_watchdog(loop);
    rummage::app::WindowLifetime lifetime;
    std::vector<int> events;
    const auto open = [&]() -> rummage::app::WindowSession {
        struct Guard {
            std::vector<int>& events;
            ~Guard() { events.push_back(3); }
        } guard{events};
        events.push_back(1);
        co_await lifetime;
        events.push_back(2);
    };
    auto window = open();
    CHECK_FALSE(window.done());
    const auto finish = [&] {
        CHECK(window.done());
        events.push_back(4);
        loop.quit();
    };
    lifetime.close(loop, finish);
    lifetime.close(loop, finish);
    CHECK(events == std::vector<int>{1});
    CHECK(loop.run() == 0);
    CHECK(events == std::vector<int>{1, 2, 3, 4});
    window.rethrow();
}

TEST_CASE("windows: application shutdown cancels a queued close without resuming dead locals") {
    EventLoop loop;
    add_watchdog(loop);
    rummage::app::WindowLifetime lifetime;
    bool destroyed = false;
    const auto open = [&]() -> rummage::app::WindowSession {
        struct Guard {
            bool& destroyed;
            ~Guard() { destroyed = true; }
        } guard{destroyed};
        co_await lifetime;
        FAIL_CHECK("destroyed window resumed");
    };
    {
        auto window = open();
        lifetime.close(loop, [] { FAIL_CHECK("destroyed application's callback ran"); });
    }
    CHECK(destroyed);
    loop.post([&] { loop.quit(); });
    CHECK(loop.run() == 0);
}

TEST_CASE("post from another thread runs on the loop thread, quit sets the exit code") {
    EventLoop loop;
    add_watchdog(loop);
    std::thread::id ran_on;
    std::thread poster{[&] {
        loop.post([&] {
            ran_on = std::this_thread::get_id();
            loop.quit(7);
        });
    }};
    CHECK(loop.run() == 7);
    poster.join();
    CHECK(ran_on == std::this_thread::get_id());
}

TEST_CASE("posted functions run in posting order") {
    EventLoop loop;
    add_watchdog(loop);
    std::vector<int> order;
    std::thread poster{[&] {
        for (int i = 0; i < 1000; ++i) {
            loop.post([&order, i] { order.push_back(i); });
        }
        loop.post([&loop] { loop.quit(0); });
    }};
    CHECK(loop.run() == 0);
    poster.join();
    REQUIRE(order.size() == 1000);
    for (int i = 0; i < 1000; ++i) {
        CHECK(order[static_cast<std::size_t>(i)] == i);
    }
}

TEST_CASE("one-shot and repeating timers") {
    EventLoop loop;
    add_watchdog(loop);
    int once = 0;
    int repeats = 0;
    loop.add_timer(0ms, 0ms, [&] { ++once; });
    EventLoop::SourceId repeating = 0;
    repeating = loop.add_timer(1ms, 1ms, [&] {
        if (++repeats == 5) {
            loop.remove(repeating);
            // Give a removed timer the chance to (wrongly) fire again before quitting.
            loop.add_timer(20ms, 0ms, [&] { loop.quit(0); });
        }
    });
    CHECK(loop.run() == 0);
    CHECK(once == 1);
    CHECK(repeats == 5);
}

TEST_CASE("fd sources: readable callback, removal from inside a callback") {
    EventLoop loop;
    add_watchdog(loop);
    const Pipe first;
    const Pipe second;
    int first_calls = 0;
    int second_calls = 0;
    EventLoop::SourceId second_id = 0;

    loop.add_fd(first.fds[0], POLLIN, [&](short revents) {
        CHECK((revents & POLLIN) != 0);
        first.read_byte();
        ++first_calls;
        loop.remove(second_id);  // both are readable in this iteration: second must not run
        loop.add_timer(20ms, 0ms, [&] { loop.quit(0); });
    });
    second_id = loop.add_fd(second.fds[0], POLLIN, [&](short /*revents*/) {
        second.read_byte();
        ++second_calls;
    });
    first.write_byte();
    second.write_byte();

    CHECK(loop.run() == 0);
    CHECK(first_calls == 1);
    CHECK(second_calls == 0);
}

TEST_CASE("sources added from a callback work, before_poll and notify_always are honoured") {
    EventLoop loop;
    add_watchdog(loop);
    const Pipe idle;  // never written: only notify_always gets its callback called
    int before = 0;
    int notified_idle = 0;
    loop.add_fd(idle.fds[0], POLLIN,
                [&](short revents) {
                    CHECK(revents == 0);
                    ++notified_idle;
                },
                {.before_poll = [&] { ++before; }, .notify_always = true});

    const Pipe late;
    bool late_called = false;
    loop.add_timer(1ms, 0ms, [&] {
        loop.add_fd(late.fds[0], POLLIN, [&](short /*revents*/) {
            late.read_byte();
            late_called = true;
            loop.quit(0);
        });
        late.write_byte();
    });

    CHECK(loop.run() == 0);
    CHECK(late_called);
    CHECK(before >= 2);
    CHECK(notified_idle == before);
}

TEST_CASE("prepared display reads finish before worker completions can redraw") {
    EventLoop loop;
    add_watchdog(loop);
    const Pipe display;
    bool prepared = false;
    bool completed = false;
    loop.add_fd(display.fds[0], POLLIN,
                [&](short revents) {
                    CHECK(revents == 0);
                    CHECK_FALSE(prepared);
                },
                {.before_poll = [&] { prepared = true; },
                 .after_poll =
                     [&](short revents) {
                         CHECK(revents == 0);
                         CHECK(prepared);
                         prepared = false;
                     },
                 .notify_always = true});
    // The eventfd for post() was registered before the display. A worker completion may
    // draw immediately, and EGL may then read from its own queue on the same display.
    loop.post([&] {
        CHECK_FALSE(prepared);
        completed = true;
        loop.quit();
    });
    CHECK(loop.run() == 0);
    CHECK(completed);
    CHECK_FALSE(prepared);
}

TEST_CASE("poll completion hooks finish all reads before ready fd callbacks") {
    EventLoop loop;
    add_watchdog(loop);
    const Pipe first;
    const Pipe second;
    bool first_prepared = false;
    bool second_prepared = false;
    int dispatched = 0;
    const auto dispatch = [&](short revents) {
        CHECK((revents & POLLIN) != 0);
        CHECK_FALSE(first_prepared);
        CHECK_FALSE(second_prepared);
        if (++dispatched == 2)
            loop.quit();
    };
    loop.add_fd(first.fds[0], POLLIN, dispatch,
                {.before_poll = [&] { first_prepared = true; },
                 .after_poll =
                     [&](short revents) {
                         CHECK((revents & POLLIN) != 0);
                         first.read_byte();
                         first_prepared = false;
                     }});
    loop.add_fd(second.fds[0], POLLIN, dispatch,
                {.before_poll = [&] { second_prepared = true; },
                 .after_poll =
                     [&](short revents) {
                         CHECK((revents & POLLIN) != 0);
                         second.read_byte();
                         second_prepared = false;
                     }});
    first.write_byte();
    second.write_byte();
    CHECK(loop.run() == 0);
    CHECK(dispatched == 2);
}

TEST_CASE("protocol ownership changes dispatch before queued clipboard enrichment") {
    EventLoop loop;
    add_watchdog(loop);
    const Pipe display;
    const Pipe other_display;
    const Pipe removed;
    bool prepared = false;
    bool other_prepared = false;
    bool pending_selection = false;
    bool completion_ran = false;
    bool replaced_newer_copy = false;
    unsigned generation = 1;
    const auto copied_generation = generation;
    const auto removed_id = loop.add_fd(removed.fds[0], POLLIN, [](short) {
        FAIL_CHECK("protocol dispatch removed this source before ordinary dispatch");
    });
    loop.add_fd(display.fds[0], POLLIN,
                [&](short revents) {
                    CHECK((revents & POLLIN) != 0);
                    CHECK_FALSE(prepared);
                    CHECK_FALSE(other_prepared);
                    REQUIRE(pending_selection);
                    ++generation;  // A different application owns the clipboard now.
                    pending_selection = false;
                    loop.remove(removed_id);
                    // Reallocation must not invalidate the remaining snapshotted sources.
                    for (int i = 0; i < 100; ++i)
                        loop.add_timer(1s, 0ms, [] {});
                },
                {.before_poll = [&] { prepared = true; },
                 .after_poll =
                     [&](short revents) {
                         REQUIRE((revents & POLLIN) != 0);
                         display.read_byte();
                         pending_selection =
                             true;  // Like wl_display_read_events: not dispatched yet.
                         prepared = false;
                     }});
    loop.add_fd(other_display.fds[0], POLLIN,
                [&](short revents) {
                    CHECK(revents == 0);
                    CHECK_FALSE(prepared);
                    CHECK_FALSE(other_prepared);
                },
                {.before_poll = [&] { other_prepared = true; },
                 .after_poll =
                     [&](short revents) {
                         CHECK(revents == 0);
                         other_prepared = false;
                     },
                 .notify_always = true});
    // Wake fd was registered before either display. Both it and the protocol fd are ready
    // in the same poll; the worker must observe the dispatched ownership change first.
    loop.post([&] {
        CHECK_FALSE(prepared);
        CHECK_FALSE(other_prepared);
        CHECK_FALSE(pending_selection);
        replaced_newer_copy = generation == copied_generation;
        completion_ran = true;
        loop.quit();
    });
    display.write_byte();
    removed.write_byte();
    CHECK(loop.run() == 0);
    CHECK(completion_ran);
    CHECK_FALSE(replaced_newer_copy);
    CHECK(generation == 2);
}

TEST_CASE("quitting during poll preparation releases prepared reads") {
    EventLoop loop;
    const Pipe display;
    bool prepared = false;
    loop.add_fd(display.fds[0], POLLIN,
                [](short) { FAIL_CHECK("must not dispatch after preparation quits"); },
                {.before_poll =
                     [&] {
                         prepared = true;
                         // Reallocating sources while a preparation callback runs is safe.
                         for (int i = 0; i < 100; ++i)
                             loop.add_timer(1s, 0ms, [] {});
                         loop.quit(3);
                     },
                 .after_poll =
                     [&](short revents) {
                         CHECK(revents == 0);
                         CHECK(prepared);
                         prepared = false;
                     }});
    CHECK(loop.run() == 3);
    CHECK_FALSE(prepared);
}
