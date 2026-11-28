#include "rummage/platform/program_output.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/wait.h>
#include <vector>

namespace {
using namespace std::chrono_literals;
namespace fs = std::filesystem;
constexpr std::string_view marker = "original executable image\n";

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error{message};
}

struct TemporaryDirectory {
    fs::path path;

    TemporaryDirectory() {
        auto pattern = (fs::temp_directory_path() / "rummage-self-exec-XXXXXX").string();
        require(::mkdtemp(pattern.data()) != nullptr, "Cannot create private executable fixture");
        path = pattern;
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

std::string identity(std::uintmax_t value) { return std::to_string(value); }

void launch_and_check(const fs::path& executable, const std::vector<std::string>& arguments,
                      bool self, std::string_view expected) {
    rummage::core::EventLoop loop;
    bool completed = false;
    int status = -1;
    std::string output;
    rummage::platform::ProgramOutput process{loop, [&](const std::string& text, int result) {
                                                 completed = true;
                                                 status = result;
                                                 output = text;
                                                 loop.quit();
                                             }};
    require(self ? process.start_self(arguments) : process.start(executable, arguments),
            "Cannot start executable fixture");
    loop.add_timer(10s, 0ms, [&] { loop.quit(1); });
    require(loop.run() == 0 && completed, "Executable fixture timed out");
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "Executable fixture did not exit successfully");
    require(output == expected, "Child did not execute the original image with preserved output");
    require(!process.truncated(), "Fixture output was truncated");
}

int exercise(bool replace) {
    const auto executable = fs::read_symlink("/proc/self/exe");
    struct stat original{};
    require(::stat("/proc/self/exe", &original) == 0, "Cannot identify running image");
    require(fs::remove(executable), "Cannot unlink running fixture");
    require(fs::read_symlink("/proc/self/exe").string().ends_with(" (deleted)"),
            "Fixture is not running from an unlinked executable");
    if (replace) {
        // A path-based relaunch must not silently run an installed replacement instead.
        std::ofstream replacement{executable};
        replacement << "#!/bin/sh\nprintf 'replacement executable\\n'\nexit 23\n";
        replacement.close();
        require(replacement.good(), "Cannot write replacement executable");
        fs::permissions(executable, fs::perms::owner_all);
    }
    launch_and_check(
        {},
        {"--child", identity(original.st_dev), identity(original.st_ino), "argument with spaces"},
        true, marker);
    std::cout << "self launch verified\n";
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 5 && std::string_view{argv[1]} == "--child") {
            struct stat current{};
            require(::stat("/proc/self/exe", &current) == 0, "Cannot identify child image");
            require(identity(current.st_dev) == argv[2] && identity(current.st_ino) == argv[3],
                    "Child ran a replacement image instead of the original inode");
            require(std::string_view{argv[4]} == "argument with spaces", "Child arguments changed");
            std::cout << marker;
            return 0;
        }
        if (argc == 3 && std::string_view{argv[1]} == "--exercise")
            return exercise(std::string_view{argv[2]} == "replace");
        const TemporaryDirectory directory;
        for (const auto* scenario : {"deleted", "replace"}) {
            const auto copy = directory.path / scenario;
            fs::copy_file("/proc/self/exe", copy);
            fs::permissions(copy, fs::perms::owner_all);
            launch_and_check(copy, {"--exercise", scenario}, false, "self launch verified\n");
        }
        std::cout << "Self launch after executable deletion and replacement passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
