#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <sys/inotify.h>
#include "rummage/fs/directory_watcher.hpp"
#include "temp_dir.hpp"
using rummage::core::EventLoop;
using rummage::core::WorkerPool;
using rummage::fs::DirectoryWatcher;
using rummage::test::TempDir;
using namespace std::chrono_literals;
bool run_until(EventLoop& loop, const std::function<bool()>& done) {
    bool ok = false;
    const auto poll = loop.add_timer(1ms, 1ms, [&] {
        if (done()) {
            ok = true;
            loop.quit(0);
        }
    });
    const auto watchdog = loop.add_timer(5s, 0ms, [&loop] { loop.quit(1); });
    loop.run();
    loop.remove(poll);
    loop.remove(watchdog);
    return ok;
}

void run_for(EventLoop& loop, std::chrono::milliseconds time) {
    loop.add_timer(time, 0ms, [&loop] { loop.quit(0); });
    loop.run();
}

TEST_CASE(
    "directory watcher: cancelled pending registrations and shared descriptors have safe "
    "lifetimes") {
    TempDir directory;
    EventLoop loop;
    WorkerPool pool{loop, 2};
    DirectoryWatcher watcher{loop};
    int first_events = 0, second_events = 0, completed = 0;
    const auto first = watcher.watch_async(
        pool, directory.path, [&](const auto&) { ++first_events; },
        [&](std::error_code error) {
            CHECK_FALSE(error);
            ++completed;
        });
    const auto second = watcher.watch_async(
        pool, directory.path, [&](const auto&) { ++second_events; },
        [&](std::error_code error) {
            CHECK_FALSE(error);
            ++completed;
        });
    REQUIRE(run_until(loop, [&] { return completed == 2; }));
    watcher.unwatch(first);
    directory.write("new.txt", "event");
    REQUIRE(run_until(loop, [&] { return second_events > 0; }));
    CHECK(first_events == 0);
    watcher.unwatch(second);

    std::promise<void> release;
    auto gate = release.get_future().share();
    std::atomic<bool> entered{false}, finished{false};

    struct Release {
        std::promise<void>& promise;

        ~Release() {
            try {
                promise.set_value();
            } catch (const std::future_error&) {
            }
        }
    } release_on_exit{release};

    auto pending =
        std::make_unique<DirectoryWatcher>(loop, [&](int fd, const char* path, std::uint32_t mask) {
            entered = true;
            gate.wait_for(2s);
            const int result = ::inotify_add_watch(fd, path, mask);
            finished = true;
            return result;
        });
    int callbacks = 0;
    pending->watch_async(
        pool, directory.path, [&](const auto&) { ++callbacks; },
        [&](std::error_code) { ++callbacks; });
    REQUIRE(run_until(loop, [&] { return entered.load(); }));
    pending.reset();  // must not wait for the blocked syscall or close/reuse its fd
    CHECK_FALSE(finished.load());
    release.set_value();
    REQUIRE(run_until(loop, [&] { return finished.load(); }));
    run_for(loop, 20ms);
    CHECK(callbacks == 0);
}

TEST_CASE(
    "directory watcher: registration backlog is bounded and dropped worker tasks release "
    "reservations") {
    TempDir directory;
    EventLoop loop;
    auto pool = std::make_unique<WorkerPool>(loop, 1);
    auto release = std::make_shared<std::promise<void>>();
    const auto gate = release->get_future().share();
    auto entered = std::make_shared<std::atomic<bool>>(false);
    pool->submit(
        rummage::core::Priority::visible,
        [gate, entered](const auto&) {
            *entered = true;
            gate.wait_for(5s);
            return 0;
        },
        [](int) {});
    REQUIRE(run_until(loop, [&] { return entered->load(); }));

    // Destruction of this queued task releases the running worker. Therefore pool teardown
    // deterministically drops every queued registration, without a timing-dependent sleep.
    struct Unblock {
        std::shared_ptr<std::promise<void>> promise;

        ~Unblock() {
            try {
                promise->set_value();
            } catch (const std::future_error&) {
            }
        }
    };

    auto unblock = std::make_shared<Unblock>(release);
    pool->submit(
        rummage::core::Priority::visible, [unblock](const auto&) { return 0; }, [](int) {});
    unblock.reset();
    DirectoryWatcher watcher{loop};
    int callbacks = 0;
    std::vector<DirectoryWatcher::WatchId> ids;
    for (int i = 0; i < 8192; ++i) {
        const auto id = watcher.watch_async(
            *pool, directory.path, [&](const auto&) { ++callbacks; },
            [&](std::error_code) { ++callbacks; });
        REQUIRE(id != 0);
        ids.push_back(id);
    }
    CHECK(watcher.watch_async(*pool, directory.path, [](const auto&) {}) == 0);
    for (const auto id : ids)
        watcher.unwatch(id);
    pool.reset();
    run_for(loop, 10ms);
    CHECK(callbacks == 0);
    // A surviving watcher can use a new pool; the abandoned tasks did not leave its adding
    // counter permanently nonzero or retain the entire backlog's subscription capacity.
    WorkerPool replacement{loop, 1};
    bool completed = false;
    const auto renewed = watcher.watch_async(
        replacement, directory.path, [](const auto&) {},
        [&](std::error_code error) {
            CHECK_FALSE(error);
            completed = true;
        });
    REQUIRE(renewed != 0);
    REQUIRE(run_until(loop, [&] { return completed; }));
    watcher.unwatch(renewed);
}
