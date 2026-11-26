#include "rummage/core/file_watcher.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"
#include "temp_dir.hpp"

using rummage::core::EventLoop;
using rummage::core::FileWatcher;
using rummage::test::TempDir;
using namespace std::chrono_literals;

namespace {

// Runs the loop for a fixed time: long enough for inotify and the settle timer.
void run_for(EventLoop& loop, std::chrono::milliseconds time) {
    loop.add_timer(time, 0ms, [&loop] { loop.quit(0); });
    loop.run();
}

}  // namespace

TEST_CASE("file watcher: write, other files, coalescing, rename, removal") {
    const TempDir dir;
    const std::filesystem::path file = dir.path / "colors.toml";
    EventLoop loop;
    int calls = 0;
    const FileWatcher watcher{loop, file, [&] { ++calls; }, 20ms};

    SUBCASE("several writes in a row are one change") {
        for (int i = 0; i < 5; ++i) {
            dir.write("colors.toml", "x");
        }
        run_for(loop, 150ms);
        CHECK(calls == 1);
    }
    SUBCASE("other files in the directory are ignored") {
        dir.write("config.toml", "x");
        dir.write("colors.toml.tmp", "x");
        run_for(loop, 150ms);
        CHECK(calls == 0);
    }
    SUBCASE("an atomic replacement by rename is seen") {
        const std::filesystem::path temp = dir.write("colors.toml.tmp", "x");
        run_for(loop, 100ms);
        std::filesystem::rename(temp, file);
        run_for(loop, 150ms);
        CHECK(calls == 1);
    }
    SUBCASE("removal is seen, and later changes still are") {
        dir.write("colors.toml", "x");
        run_for(loop, 150ms);
        std::filesystem::remove(file);
        run_for(loop, 150ms);
        dir.write("colors.toml", "y");
        run_for(loop, 150ms);
        CHECK(calls == 3);
    }
}

TEST_CASE("file watcher creates a missing directory") {
    const TempDir dir;
    const std::filesystem::path file = dir.path / "rummage" / "colors.toml";
    EventLoop loop;
    int calls = 0;
    const FileWatcher watcher{loop, file, [&] { ++calls; }, 20ms};
    CHECK(std::filesystem::is_directory(file.parent_path()));
    std::ofstream{file} << "x";
    run_for(loop, 150ms);
    CHECK(calls == 1);
}

namespace {

std::size_t inotify_instances() {
    std::size_t count = 0;
    for (const auto& fd : std::filesystem::directory_iterator{"/proc/self/fd"}) {
        std::error_code error;
        if (std::filesystem::read_symlink(fd.path(), error) == "anon_inode:inotify")
            ++count;
    }
    return count;
}

}  // namespace

TEST_CASE("file watcher: loop subscribers share one descriptor and release it") {
    const TempDir dir;
    EventLoop loop;
    const auto before = inotify_instances();
    int first_calls = 0;
    int second_calls = 0;
    int other_calls = 0;
    auto first = std::make_unique<FileWatcher>(loop, dir.path / "one", [&] { ++first_calls; }, 5ms);
    auto second =
        std::make_unique<FileWatcher>(loop, dir.path / "one", [&] { ++second_calls; }, 5ms);
    auto other =
        std::make_unique<FileWatcher>(loop, dir.path / "sub/one", [&] { ++other_calls; }, 5ms);
    CHECK(inotify_instances() == before + 1);
    dir.write("one", "x");
    run_for(loop, 50ms);
    CHECK(first_calls == 1);
    CHECK(second_calls == 1);
    CHECK(other_calls == 0);
    first.reset();
    dir.write("one", "y");
    dir.write("sub/one", "z");
    run_for(loop, 50ms);
    CHECK(first_calls == 1);
    CHECK(second_calls == 2);
    CHECK(other_calls == 1);
    second.reset();
    CHECK(inotify_instances() == before + 1);
    other.reset();
    CHECK(inotify_instances() == before);
    // Reuse the same loop after the final subscriber has released its service.
    first = std::make_unique<FileWatcher>(loop, dir.path / "one", [&] { ++first_calls; }, 5ms);
    dir.write("one", "again");
    run_for(loop, 50ms);
    CHECK(first_calls == 2);
    CHECK(inotify_instances() == before + 1);
}

TEST_CASE("file watcher: independent loops and many subscriptions have bounded descriptors") {
    const TempDir dir;
    const auto before = inotify_instances();
    {
        EventLoop first;
        EventLoop second;
        int calls = 0;
        std::vector<std::unique_ptr<FileWatcher>> watchers;
        for (int i = 0; i < 128; ++i)
            watchers.push_back(
                std::make_unique<FileWatcher>(first, dir.path / "one", [&] { ++calls; }, 5ms));
        const FileWatcher independent{second, dir.path / "one", [&] { ++calls; }, 5ms};
        CHECK(inotify_instances() == before + 2);
        dir.write("one", "x");
        run_for(first, 50ms);
        CHECK(calls == 128);
        run_for(second, 50ms);
        CHECK(calls == 129);
    }
    CHECK(inotify_instances() == before);
}

TEST_CASE("file watcher: failed subscription and pending subscriber destruction are safe") {
    const TempDir dir;
    EventLoop loop;
    const auto before = inotify_instances();
    dir.write("not-a-directory", "x");
    CHECK_THROWS_AS(FileWatcher(loop, dir.path / "not-a-directory/file", [] {}), std::system_error);
    CHECK(inotify_instances() == before);
    int calls = 0;
    auto watcher = std::make_unique<FileWatcher>(loop, dir.path / "one", [&] { ++calls; }, 100ms);
    dir.write("one", "x");
    run_for(loop, 20ms);
    watcher.reset();
    run_for(loop, 150ms);
    CHECK(calls == 0);
    CHECK(inotify_instances() == before);
}

TEST_CASE("file watcher: settling follows the last write rather than the first") {
    const TempDir dir;
    EventLoop loop;
    int calls = 0;
    const FileWatcher watcher{loop, dir.path / "one", [&] { ++calls; }, 100ms};
    dir.write("one", "x");
    loop.add_timer(60ms, 0ms, [&] { dir.write("one", "y"); });
    run_for(loop, 130ms);
    CHECK(calls == 0);
    run_for(loop, 100ms);
    CHECK(calls == 1);
}

TEST_CASE("file watcher: queue overflow notifies subscriptions in every directory") {
    const TempDir dir;
    std::size_t capacity = 0;
    std::ifstream{"/proc/sys/fs/inotify/max_queued_events"} >> capacity;
    if (capacity == 0 || capacity > 65536) {
        MESSAGE("Overflow test requires an inotify queue of at most 65536 events");
        return;
    }
    EventLoop loop;
    int calls = 0;
    const FileWatcher first{loop, dir.path / "one", [&] { ++calls; }, 5ms};
    const FileWatcher second{loop, dir.path / "sub/two", [&] { ++calls; }, 5ms};
    // Alternate CREATE, CLOSE_WRITE and DELETE so identical events cannot coalesce.
    for (std::size_t i = 0; i <= capacity / 3 + 1; ++i) {
        dir.write("unrelated", "x");
        std::filesystem::remove(dir.path / "unrelated");
    }
    run_for(loop, 100ms);
    CHECK(calls == 2);
}

TEST_CASE("file watcher: existing loop can subscribe when descriptors are exhausted") {
    const TempDir dir;
    const auto child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        try {
            EventLoop first;
            EventLoop second;
            int calls = 0;
            const FileWatcher initial{first, dir.path / "one", [&] { ++calls; }, 5ms};
            rlimit limit{};
            if (getrlimit(RLIMIT_NOFILE, &limit) != 0)
                _exit(2);
            limit.rlim_cur = std::min<rlim_t>(limit.rlim_cur, 64);
            if (setrlimit(RLIMIT_NOFILE, &limit) != 0)
                _exit(3);
            std::vector<int> fillers;
            for (;;) {
                const int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
                if (fd < 0)
                    break;
                fillers.push_back(fd);
            }
            // The existing service needs no new descriptor, including for another directory.
            const FileWatcher shared{first, dir.path / "sub/two", [&] { ++calls; }, 5ms};
            bool rejected = false;
            try {
                const FileWatcher independent{second, dir.path / "one", [] {}};
            } catch (const std::system_error& error) {
                rejected = error.code().value() == EMFILE;
            }
            for (const int fd : fillers)
                close(fd);
            if (!rejected)
                _exit(4);
            dir.write("one", "x");
            dir.write("sub/two", "y");
            run_for(first, 50ms);
            _exit(calls == 2 ? 0 : 5);
        } catch (...) {
            _exit(6);
        }
    }
    int status = 0;
    REQUIRE(waitpid(child, &status, 0) == child);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}
