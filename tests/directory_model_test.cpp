#include "rummage/fs/directory_model.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <sys/inotify.h>
#include <vector>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/fs/subfolders.hpp"
#include "temp_dir.hpp"

using rummage::core::EventLoop;
using rummage::core::WorkerPool;
using rummage::fs::DirectoryModel;
using rummage::fs::DirectoryWatcher;
using rummage::fs::Entry;
using rummage::fs::ModelChange;
using rummage::test::TempDir;
using namespace std::chrono_literals;

namespace {

// Runs the loop until done() holds (checked after every iteration's timer) or 5 s pass.
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

std::vector<std::string> names(const DirectoryModel& model) {
    std::vector<std::string> out;
    for (const Entry& entry : model.listing().entries()) {
        out.push_back(entry.name);
    }
    return out;
}

const Entry* find(const DirectoryModel& model, std::string_view name) {
    const auto& entries = model.listing().entries();
    const auto it = std::ranges::find(entries, name, &Entry::name);
    return it == entries.end() ? nullptr : &*it;
}

// A model on a temporary directory, with every change recorded.
struct Fixture {
    TempDir dir;
    EventLoop loop;
    WorkerPool pool{loop, 2};
    DirectoryWatcher watcher{loop};
    DirectoryModel model{loop, pool, watcher, rummage::fs::MimeDatabase::load(), 10ms};
    std::vector<ModelChange> changes;
    bool loaded = false;

    Fixture() {
        model.set_change_handler([this](const ModelChange& change) {
            changes.push_back(change);
            loaded = loaded || change.loaded;
        });
    }

    void open() {
        model.open(dir.path);
        REQUIRE(run_until(loop, [this] { return loaded; }));
        changes.clear();
    }

    // Waits until the listing satisfies done, then a little longer to catch extra notifications.
    void settle(const std::function<bool()>& done) {
        REQUIRE(run_until(loop, done));
        run_for(loop, 40ms);
    }

    [[nodiscard]] std::vector<std::string> all(
        std::vector<std::string> ModelChange::* field) const {
        std::vector<std::string> out;
        for (const ModelChange& change : changes) {
            out.insert(out.end(), (change.*field).begin(), (change.*field).end());
        }
        std::ranges::sort(out);
        return out;
    }
};

}  // namespace

TEST_CASE("model: initial load, sorted, then loaded") {
    Fixture f;
    f.dir.write("b.txt", "b");
    f.dir.write("a10.txt", "a");
    f.dir.write("a9.txt", "a");
    std::filesystem::create_directory(f.dir.path / "sub");
    f.model.open(f.dir.path);
    CHECK(f.model.loading());
    REQUIRE(run_until(f.loop, [&] { return f.loaded; }));
    CHECK_FALSE(f.model.loading());
    CHECK_FALSE(f.model.error());
    CHECK(f.changes.front().reset);
    CHECK(names(f.model) == std::vector<std::string>{"sub", "a9.txt", "a10.txt", "b.txt"});
    CHECK(f.model.listing().entries().front().mime == rummage::fs::MimeDatabase::kDirectory);
}

TEST_CASE("model: create, write, delete, mkdir") {
    Fixture f;
    f.dir.write("keep.txt", "1");
    f.open();

    f.dir.write("new.txt", "hello");
    std::filesystem::create_directory(f.dir.path / "folder");
    f.settle([&] { return find(f.model, "new.txt") != nullptr && find(f.model, "folder"); });
    CHECK(f.all(&ModelChange::added) == std::vector<std::string>{"folder", "new.txt"});
    CHECK(find(f.model, "new.txt")->size == 5);
    CHECK(find(f.model, "folder")->is_directory());

    f.changes.clear();
    f.dir.write("keep.txt", "now longer");
    f.settle([&] {
        const Entry* keep = find(f.model, "keep.txt");
        return keep != nullptr && keep->size == 10;
    });
    CHECK(f.all(&ModelChange::updated) == std::vector<std::string>{"keep.txt"});
    CHECK(f.all(&ModelChange::added).empty());

    f.changes.clear();
    std::filesystem::remove(f.dir.path / "new.txt");
    f.settle([&] { return find(f.model, "new.txt") == nullptr; });
    CHECK(f.all(&ModelChange::removed) == std::vector<std::string>{"new.txt"});
    CHECK(names(f.model) == std::vector<std::string>{"folder", "keep.txt"});
}

TEST_CASE("model: renames inside the directory, and moves in and out") {
    Fixture f;
    f.dir.write("old.txt", "x");
    std::filesystem::create_directory(f.dir.path / "elsewhere");
    f.open();

    std::filesystem::rename(f.dir.path / "old.txt", f.dir.path / "new.txt");
    f.settle([&] { return find(f.model, "new.txt") != nullptr; });
    REQUIRE(f.changes.size() == 1);
    REQUIRE(f.changes[0].renamed.size() == 1);
    CHECK(f.changes[0].renamed[0] == std::pair<std::string, std::string>{"old.txt", "new.txt"});
    CHECK(f.changes[0].added.empty());
    CHECK(f.changes[0].removed.empty());
    CHECK(find(f.model, "old.txt") == nullptr);

    // Out of the watched directory: a removal. Back in: an addition.
    f.changes.clear();
    std::filesystem::rename(f.dir.path / "new.txt", f.dir.path / "elsewhere" / "moved.txt");
    f.settle([&] { return find(f.model, "new.txt") == nullptr; });
    CHECK(f.all(&ModelChange::removed) == std::vector<std::string>{"new.txt"});
    f.changes.clear();
    std::filesystem::rename(f.dir.path / "elsewhere" / "moved.txt", f.dir.path / "back.txt");
    f.settle([&] { return find(f.model, "back.txt") != nullptr; });
    CHECK(f.all(&ModelChange::added) == std::vector<std::string>{"back.txt"});
}

TEST_CASE("model: a burst of 500 files arrives in a few notifications") {
    Fixture f;
    f.open();
    for (int i = 0; i < 500; ++i) {
        f.dir.write("f" + std::to_string(i), "");
    }
    f.settle([&] { return f.model.listing().entries().size() == 500; });
    CHECK(f.all(&ModelChange::added).size() == 500);
    CHECK(f.changes.size() < 20);
    MESSAGE("500 files in " << f.changes.size() << " notifications");
}

TEST_CASE("model: changes during the load are neither lost nor doubled") {
    Fixture f;
    for (int i = 0; i < 300; ++i) {
        f.dir.write("f" + std::to_string(i), "");
    }
    f.model.open(f.dir.path);  // watching and loading
    f.dir.write("during.txt", "");
    std::filesystem::remove(f.dir.path / "f0");
    f.settle([&] { return f.loaded && find(f.model, "during.txt") && !find(f.model, "f0"); });
    CHECK(f.model.listing().entries().size() == 300);
    std::vector<std::string> sorted = names(f.model);
    std::ranges::sort(sorted);
    CHECK(std::ranges::adjacent_find(sorted) == sorted.end());  // no duplicates
}

TEST_CASE("model: the directory is deleted, or cannot be opened") {
    Fixture f;
    const std::filesystem::path sub = f.dir.path / "sub";
    std::filesystem::create_directory(sub);
    f.dir.write("sub/a", "");
    f.model.open(sub);
    REQUIRE(run_until(f.loop, [&] { return f.loaded; }));
    std::filesystem::remove_all(sub);
    f.settle([&] { return static_cast<bool>(f.model.error()); });
    CHECK(f.model.error() == std::errc::no_such_file_or_directory);
    CHECK(f.model.listing().entries().empty());

    f.model.open(f.dir.path / "absent");
    CHECK(f.model.loading());  // availability is checked on a worker, never inside open()
    CHECK_FALSE(f.model.error());
    REQUIRE(run_until(f.loop, [&] { return !f.model.loading(); }));
    CHECK(f.model.error() == std::errc::no_such_file_or_directory);
    CHECK(f.model.listing().entries().empty());
}

TEST_CASE("model: two models on one directory share the watch") {
    Fixture f;
    DirectoryModel second{f.loop, f.pool, f.watcher, nullptr, 10ms};
    bool second_loaded = false;
    second.set_change_handler([&](const ModelChange& c) { second_loaded |= c.loaded; });
    f.open();
    second.open(f.dir.path);
    REQUIRE(run_until(f.loop, [&] { return second_loaded; }));

    f.dir.write("x", "");
    f.settle([&] { return find(f.model, "x") && find(second, "x"); });

    second.open(f.dir.path / "absent");  // drops its watch; the first must keep working
    f.dir.write("y", "");
    f.settle([&] { return find(f.model, "y") != nullptr; });
}

TEST_CASE("directory watcher: slow asynchronous registration leaves other folders responsive") {
    TempDir directory;
    const auto slow = directory.path / "slow";
    const auto fast = directory.path / "fast";
    std::filesystem::create_directories(slow);
    std::filesystem::create_directories(fast);
    directory.write("fast/visible.txt", "ready");
    EventLoop loop;
    WorkerPool pool{loop, 2};
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

    DirectoryWatcher watcher{loop, [&](int fd, const char* path, std::uint32_t mask) {
                                 if (std::filesystem::path{path} == slow) {
                                     entered = true;
                                     gate.wait_for(2s);
                                     finished = true;
                                 }
                                 return ::inotify_add_watch(fd, path, mask);
                             }};
    auto first = std::make_unique<DirectoryModel>(loop, pool, watcher,
                                                  rummage::fs::MimeDatabase::load(), 10ms);
    DirectoryModel second{loop, pool, watcher, rummage::fs::MimeDatabase::load(), 10ms};
    bool slow_loaded = false, fast_loaded = false, heartbeat = false;
    first->set_change_handler([&](const ModelChange& change) { slow_loaded |= change.loaded; });
    second.set_change_handler([&](const ModelChange& change) { fast_loaded |= change.loaded; });
    first->open(slow);
    second.open(fast);
    loop.post([&] { heartbeat = true; });
    CHECK(run_until(loop, [&] { return entered && fast_loaded && heartbeat; }));
    CHECK_FALSE(finished.load());
    CHECK_FALSE(slow_loaded);
    CHECK(names(second) == std::vector<std::string>{"visible.txt"});
    first.reset();
    release.set_value();
    REQUIRE(run_until(loop, [&] { return finished.load(); }));
    run_for(loop, 20ms);
    CHECK_FALSE(slow_loaded);
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

TEST_CASE(
    "subfolder disclosures: slow watch registration is asynchronous and adoption closes the event "
    "gap") {
    TempDir directory;
    const auto slow = directory.path / "slow";
    const auto fast = directory.path / "fast";
    std::filesystem::create_directories(slow);
    std::filesystem::create_directories(fast / "child");
    EventLoop loop;
    WorkerPool pool{loop, 2};
    std::promise<void> release;
    const auto gate = release.get_future().share();
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

    DirectoryWatcher watcher{loop, [&](int fd, const char* path, std::uint32_t mask) {
                                 if (std::filesystem::path{path} == slow) {
                                     entered = true;
                                     gate.wait_for(2s);
                                     finished = true;
                                 }
                                 return ::inotify_add_watch(fd, path, mask);
                             }};
    int changes = 0;
    rummage::fs::Subfolders probes{pool, watcher, [&] { ++changes; }};
    const std::array paths{slow, fast};
    probes.retain(paths);
    REQUIRE(run_until(loop, [&] {
        return entered && probes.contains(slow, false).has_value() &&
               probes.contains(fast, false).value_or(false);
    }));
    CHECK_FALSE(finished.load());
    CHECK(probes.contains(slow, false) == false);
    SUBCASE("adoption rechecks changes made while the watch was pending") {
        std::filesystem::create_directory(slow / "new-child");
        release.set_value();
        REQUIRE(run_until(loop, [&] { return probes.contains(slow, false).value_or(false); }));
    }
    SUBCASE("removing a pending probe suppresses registration and read callbacks") {
        probes.retain({});
        const int before = changes;
        release.set_value();
        REQUIRE(run_until(loop, [&] { return finished.load(); }));
        run_for(loop, 20ms);
        CHECK(changes == before);
        CHECK_FALSE(probes.contains(slow, false).has_value());
        CHECK_FALSE(probes.loading());
    }
}

TEST_CASE(
    "model: arriving media metadata resorts, retains file fields, and rejects stale identity") {
    Fixture f;
    f.dir.write("a.png", "a");
    f.dir.write("b.mp4", "b");
    f.dir.write("c.txt", "c");
    f.open();
    f.model.set_order({.key = rummage::fs::SortKey::duration});
    Entry a = *find(f.model, "a.png"), b = *find(f.model, "b.mp4");
    a.media = std::make_shared<rummage::fs::MediaMetadata>(
        rummage::fs::MediaMetadata{.duration_ms = 2000});
    b.media = std::make_shared<rummage::fs::MediaMetadata>(
        rummage::fs::MediaMetadata{.duration_ms = 1000});
    // First response arrives while others are still pending.
    f.model.apply_media({a});
    CHECK(names(f.model) == std::vector<std::string>{"a.png", "b.mp4", "c.txt"});
    REQUIRE_FALSE(f.changes.empty());
    CHECK(f.changes.back().metadata_only);
    f.model.apply_media({b});
    CHECK(names(f.model) == std::vector<std::string>{"b.mp4", "a.png", "c.txt"});
    CHECK(find(f.model, "a.png")->permissions == a.permissions);
    f.model.set_order({.key = rummage::fs::SortKey::duration, .descending = true});
    CHECK(names(f.model) == std::vector<std::string>{"a.png", "b.mp4", "c.txt"});
    f.dir.write("a.png", "changed contents");
    f.settle([&] { return find(f.model, "a.png")->size != a.size; });
    CHECK_FALSE(find(f.model, "a.png")->media);
    f.model.apply_media({a});
    CHECK_FALSE(find(f.model, "a.png")->media);
}
