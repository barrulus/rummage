#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include "rummage/core/event_loop.hpp"
#include "rummage/platform/browser_instance.hpp"
#include "rummage/platform/launcher.hpp"

namespace {
struct Scratch {
    std::filesystem::path path;

    Scratch() {
        std::string pattern{"/tmp/rummage-archive-cli-XXXXXX"};
        if (!mkdtemp(pattern.data()))
            throw std::runtime_error("Cannot make scratch directory");
        path = pattern;
    }

    ~Scratch() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    std::filesystem::path write(const std::string& name, const std::string& text) {
        auto file = path / name;
        std::ofstream{file} << text;
        return file;
    }
};
}  // namespace

int main() {
    using namespace rummage;
    namespace fs = std::filesystem;
    using namespace std::chrono_literals;
    Scratch dir;
    const auto config = dir.write(
        "config.toml",
        "[windows]\nreuse = true\n[startup]\nmode = \"folder\"\nfolder = \"/missing/default\"\n");
    setenv("XDG_CONFIG_HOME", (dir.path / "config").c_str(), 1);
    setenv("XDG_STATE_HOME", (dir.path / "state").c_str(), 1);
    setenv("XDG_DATA_HOME", (dir.path / "data").c_str(), 1);
    setenv("XDG_CACHE_HOME", (dir.path / "cache").c_str(), 1);
    setenv("XDG_RUNTIME_DIR", dir.path.c_str(), 1);
    setenv("WAYLAND_DISPLAY", "archive-cli-test", 1);
    const auto archive = dir.path / "naïve archive.zip";
    fs::copy_file(fs::path{RUMMAGE_TEST_DATA} / "archive/basic.zip", archive);
    fs::copy_file(archive, dir.path / "--literal.zip");
    const auto name =
        platform::browser_instance_name(config.string(), (dir.path / "archive-cli-test").string());
    core::EventLoop loop;
    std::vector<platform::BrowserRequest> received;
    platform::BrowserInstance instance{
        loop, name, [&](auto request) { received.push_back(std::move(request)); }};
    platform::Launcher launcher{loop};
    const std::vector<std::vector<std::string>> invocations{
        {archive.string()},
        {"--", "--literal.zip"},
        {"--", archive.string()},
        {dir.path.string()},
        {"--", "missing archive.zip"},
        {"--new-window", "--", archive.string()}};
    const std::vector<std::string> expected{
        archive.string(),  (dir.path / "--literal.zip").string(),       archive.string(),
        dir.path.string(), (dir.path / "missing archive.zip").string(), archive.string()};
    bool failed = false;
    for (std::size_t i = 0; i < invocations.size(); ++i) {
        std::vector<std::string> args{RUMMAGE_EXECUTABLE, "--config", config.string()};
        args.insert(args.end(), invocations[i].begin(), invocations[i].end());
        bool exited = false;
        const auto error = launcher.spawn(args, dir.path, {}, [&](auto result) {
            failed = failed || result.status != 0 || result.signal != 0;
            exited = true;
        });
        if (error) {
            std::cerr << error.message();
            return 1;
        }
        const auto timer = loop.add_timer(1ms, 1ms, [&] {
            if (exited && received.size() == i + 1)
                loop.quit();
        });
        const auto timeout = loop.add_timer(10s, 0ms, [&] {
            failed = true;
            loop.quit();
        });
        loop.run();
        loop.remove(timer);
        loop.remove(timeout);
        if (received.size() != i + 1 ||
            received.back().paths != std::vector<std::string>{expected[i]} ||
            received.back().show != platform::Show::folders ||
            received.back().new_window != (i == 5))
            failed = true;
        if (failed) {
            std::cerr << "Archive CLI/desktop invocation " << i << " failed\n";
            return 1;
        }
    }
    return 0;
}
