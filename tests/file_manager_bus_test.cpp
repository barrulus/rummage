#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sdbus-c++/sdbus-c++.h>
#include <thread>

#include "rummage/core/file.hpp"
#include "rummage/platform/file_manager.hpp"

namespace {
using namespace std::chrono_literals;
using Strings = std::vector<std::string>;

void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error{message};
}

void client(const std::filesystem::path& folder) {
    const auto connection = sdbus::createSessionBusConnection();
    const auto daemon = sdbus::createProxy(*connection, sdbus::ServiceName{"org.freedesktop.DBus"},
                                           sdbus::ObjectPath{"/org/freedesktop/DBus"});
    bool ready = false;
    for (int i = 0; i < 200 && !ready; ++i) {
        daemon->callMethod("NameHasOwner")
            .onInterface("org.freedesktop.DBus")
            .withArguments(std::string{"org.freedesktop.FileManager1"})
            .storeResultsTo(ready);
        if (!ready)
            std::this_thread::sleep_for(5ms);
    }
    require(ready, "FileManager1 did not acquire its name");
    const auto proxy =
        sdbus::createProxy(*connection, sdbus::ServiceName{"org.freedesktop.FileManager1"},
                           sdbus::ObjectPath{"/org/freedesktop/FileManager1"});
    const auto call = [&](const char* method, const Strings& uris, const std::string& token) {
        proxy->callMethod(method)
            .onInterface("org.freedesktop.FileManager1")
            .withTimeout(500ms)
            .withArguments(uris, token);
    };
    call("ShowItems", {"file:///tmp/a%20b.zip"}, "items");
    call("ShowFolders", {"file:///tmp/first/", "file:///tmp/second"}, "folders");
    call("ShowItemProperties", {"file://localhost/tmp/item"}, "properties");
    call("ShowItems", {"file:///tmp/no-token"}, "");
    call("ShowItems",
         {"https://example.org/a", "file://remote/tmp/a", "file:///bad%ZZ", "file:///bad%00",
          "file:///truncated%1"},
         "invalid");
    call("ShowFolders", {}, "invalid");
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while ((!std::filesystem::exists(folder / "items") ||
            !std::filesystem::exists(folder / "folders") ||
            !std::filesystem::exists(folder / "properties") ||
            !std::filesystem::exists(folder / "empty")) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    require(rummage::core::read_text_file(folder / "items").text ==
                "--reveal\n--\n/tmp/a b.zip\nitems\nitems\n",
            "ShowItems arguments/token changed");
    require(rummage::core::read_text_file(folder / "folders").text ==
                "--open-folders\n--\n/tmp/first\n/tmp/second\nfolders\nfolders\n",
            "ShowFolders lost a directory");
    require(rummage::core::read_text_file(folder / "properties").text ==
                "--properties\n--\n/tmp/item\nproperties\nproperties\n",
            "ShowItemProperties arguments/token changed");
    require(
        rummage::core::read_text_file(folder / "empty").text == "--reveal\n--\n/tmp/no-token\n\n\n",
        "Service reused inherited activation token");
    require(!std::filesystem::exists(folder / "invalid"), "Invalid URI launched a window");
}
}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 1) {
            const auto* token = std::getenv("XDG_ACTIVATION_TOKEN");
            const auto* startup = std::getenv("DESKTOP_STARTUP_ID");
            const auto* folder = std::getenv("RUMMAGE_FILE_MANAGER_RECORD");
            require(token && startup && folder, "Missing child environment");
            std::string text;
            for (int i = 1; i < argc; ++i)
                text += std::string{argv[i]} + '\n';
            text += std::string{token} + '\n' + startup + '\n';
            require(!rummage::core::write_file_atomically(
                        std::filesystem::path{folder} / (*token ? token : "empty"), text),
                    "Cannot record child invocation");
            return 0;
        }

        struct Scratch {
            std::string folder{"/tmp/rummage-file-manager-XXXXXX"};

            Scratch() { require(mkdtemp(folder.data()) != nullptr, "Cannot make test directory"); }

            ~Scratch() { std::filesystem::remove_all(folder); }
        } scratch;

        setenv("RUMMAGE_FILE_MANAGER_RECORD", scratch.folder.c_str(), 1);
        setenv("XDG_ACTIVATION_TOKEN", "inherited-must-not-reuse", 1);
        setenv("DESKTOP_STARTUP_ID", "inherited-must-not-reuse", 1);
        rummage::core::EventLoop loop;
        std::exception_ptr failure;
        std::jthread thread{[&] {
            try {
                client(scratch.folder);
            } catch (...) {
                failure = std::current_exception();
                loop.post([&] { loop.quit(1); });
            }
        }};
        loop.add_timer(8s, 0ms, [&] { loop.quit(2); });
        const auto code = rummage::platform::run_file_manager(
            loop,
            {.program = std::filesystem::read_symlink("/proc/self/exe"), .idle_timeout = 1500ms});
        thread.join();
        if (failure)
            std::rethrow_exception(failure);
        require(code == 0, "FileManager1 did not exit when idle");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
