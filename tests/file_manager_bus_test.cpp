#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sdbus-c++/sdbus-c++.h>
#include <thread>

#include "rummage/core/file.hpp"
#include "rummage/platform/browser_instance.hpp"
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

void browser_instances() {
    namespace platform = rummage::platform;
    const auto name = platform::browser_instance_name("/test/config", "test-display");
    const platform::BrowserRequest request{platform::Show::items,
                                           {"/missing/without/stat", "/tmp/space name"},
                                           "activation-token",
                                           true};
    require(
        platform::forward_browser_request(name, request) == platform::BrowserForward::unavailable,
        "Absent browser endpoint did not fall back");
    rummage::core::EventLoop loop;
    std::vector<platform::BrowserRequest> received;
    auto instance = std::make_unique<platform::BrowserInstance>(
        loop, name, [&](auto value) { received.push_back(std::move(value)); });
    bool duplicate_rejected = false;
    try {
        platform::BrowserInstance duplicate{loop, name, [](auto) {}};
    } catch (const std::exception&) {
        duplicate_rejected = true;
    }
    require(duplicate_rejected, "Two browser instances acquired the same profile");
    std::exception_ptr failure;
    std::jthread thread{[&] {
        try {
            require(platform::forward_browser_request(name, request) ==
                        platform::BrowserForward::accepted,
                    "Browser rejected a valid launch request");
            const auto connection = sdbus::createSessionBusConnection();
            const auto proxy = sdbus::createProxy(*connection, sdbus::ServiceName{name},
                                                  sdbus::ObjectPath{"/org/rummage/Browser"});
            const auto raw = [&](std::uint32_t show, const Strings& paths,
                                 const std::string& token) {
                bool accepted = true;
                proxy->callMethod("Open")
                    .onInterface("org.rummage.Browser1")
                    .withTimeout(500ms)
                    .withArguments(show, paths, token, false)
                    .storeResultsTo(accepted);
                return accepted;
            };
            require(!raw(256, {"/tmp"}, ""), "Wire enum wrapped to folders");
            require(!raw(0, Strings(257, "/tmp"), ""), "Unbounded paths accepted");
            require(!raw(0, {"relative"}, ""), "Relative IPC path accepted");
            require(!raw(0, {"/tmp"}, std::string(4097, 'x')), "Oversized token accepted");
            require(raw(2, {"/another/missing/item"}, ""), "Properties launch failed");
        } catch (...) {
            failure = std::current_exception();
        }
        loop.post([&] { loop.quit(); });
    }};
    loop.add_timer(5s, 0ms, [&] { loop.quit(2); });
    require(loop.run() == 0, "Browser endpoint timed out");
    thread.join();
    if (failure)
        std::rethrow_exception(failure);
    require(received.size() == 2,
            "Rejected requests reached the browser or a valid request was lost");
    require(received[0].show == request.show && received[0].paths == request.paths &&
                received[0].token == request.token && received[0].new_window,
            "Forwarding changed launch context");
    require(received[1].show == platform::Show::properties && received[1].token.empty() &&
                !received[1].new_window,
            "Properties reused a stale token or window policy");
    instance.reset();
    require(
        platform::forward_browser_request(name, request) == platform::BrowserForward::unavailable,
        "Closed browser left a stale endpoint");
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
        browser_instances();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
