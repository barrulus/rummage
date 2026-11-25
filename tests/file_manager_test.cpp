#include "rummage/platform/file_manager.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "rummage/core/file.hpp"
#include "rummage/platform/browser_instance.hpp"
#include "rummage/platform/exec.hpp"
#include "rummage/platform/file_manager_config.hpp"
#include "temp_dir.hpp"

namespace platform = rummage::platform;
using Strings = std::vector<std::string>;
using rummage::test::TempDir;

TEST_CASE("file manager: local URIs are decoded without filesystem I/O") {
    using platform::Show;
    CHECK(platform::show_arguments(Show::items, {"file:///tmp/a%20b.zip"}) ==
          Strings{"--reveal", "--", "/tmp/a b.zip"});
    CHECK(platform::show_arguments(Show::properties, {"file:///a", "file://localhost/b/c"}) ==
          Strings{"--properties", "--", "/a", "/b/c"});
    CHECK(platform::show_arguments(Show::folders, {"file:///srv/photos/", "file:///tmp"}) ==
          Strings{"--open-folders", "--", "/srv/photos", "/tmp"});
    CHECK(platform::show_arguments(Show::items, {"https://x/y", "file:///-dash"}) ==
          Strings{"--reveal", "--", "/-dash"});
    CHECK(platform::show_arguments(Show::items, {"sftp://host/a", "file://host/a", "a.txt",
                                                 "file:///a%00b", "file:///a%ZZ"})
              .empty());
    CHECK(platform::show_arguments(Show::items, {}).empty());
}

TEST_CASE("file URI parsing rejects malformed, remote and ambiguous input") {
    using rummage::core::path_from_uri;
    CHECK(path_from_uri("file:///a%20b/c") == std::filesystem::path{"/a b/c"});
    CHECK(path_from_uri("FILE://localhost/x/./y/") == std::filesystem::path{"/x/y"});
    CHECK(path_from_uri("file:///") == std::filesystem::path{"/"});
    CHECK(path_from_uri("file:///a%23%3F%25%0A") == std::filesystem::path{"/a#?%\n"});
    for (const auto* uri :
         {"file://other/x", "file://user@localhost/x", "file://localhost:80/x", "http://x/y",
          "file:///a%00b", "file:///a%", "file:///a%1", "file:///a%ZZ", "file:///a?query",
          "file:///a#fragment", "file:///raw space", "file://localhost", "file:relative",
          "file:///raw\nnewline"}) {
        INFO(uri);
        CHECK_FALSE(path_from_uri(uri));
    }
}

TEST_CASE("file manager service: escaped executable, ownership and user data directory") {
    TempDir dir;
    platform::Environment environment;
    environment.data_dirs = {dir.path / "share", "/usr/share"};
    const auto service = platform::file_manager_service_path(environment);
    CHECK(service == dir.path / "share/dbus-1/services/org.freedesktop.FileManager1.service");
    const std::filesystem::path program = "/home/a b/quote\"slash\\dollar$back`/rummage";
    const auto text = platform::file_manager_service(program);
    CHECK(platform::is_ours(text));
    CHECK_FALSE(platform::is_ours("# Written by rummage --set-file-manager-elsewhere"));
    const auto exec = text.substr(text.find("Exec=") + 5);
    // The service keyfile unescapes first, then its Exec parser splits argv.
    CHECK(platform::split_exec(platform::unescape_value(exec.substr(0, exec.size() - 1))) ==
          Strings{program.string(), "--file-manager1"});
    auto change = platform::set_file_manager(environment, program, "thunar", "thunar.desktop");
    REQUIRE(change.ok);
    CHECK(rummage::core::read_text_file(service).text == text);
    CHECK(change.message.find("thunar holds org.freedesktop.FileManager1") != std::string::npos);
    CHECK(change.message.find("Folders open with thunar.desktop") != std::string::npos);
    change = platform::set_file_manager(environment, program, ".rummage-wrappe", "rummage.desktop");
    CHECK(change.ok);
    CHECK(change.message.find("holds org.freedesktop.FileManager1") == std::string::npos);
    CHECK(platform::unset_file_manager(environment).ok);
    CHECK_FALSE(std::filesystem::exists(service));
    CHECK_FALSE(platform::set_file_manager(environment, "/tmp/new\nline", "", "").ok);
    CHECK_FALSE(std::filesystem::exists(service));
    std::ofstream{service} << "[D-BUS Service]\nExec=/usr/bin/another\n";
    CHECK_FALSE(platform::set_file_manager(environment, "/tmp/rummage", "", "").ok);
    CHECK(platform::unset_file_manager(environment).ok);
    CHECK(std::filesystem::exists(service));
    std::filesystem::remove(service);
    const auto target = dir.write("owned", text);
    std::filesystem::create_symlink(target, service);
    CHECK_FALSE(platform::set_file_manager(environment, "/tmp/rummage", "", "").ok);
    CHECK(platform::unset_file_manager(environment).ok);
    CHECK(std::filesystem::is_symlink(service));
    CHECK(rummage::core::read_text_file(target).text == text);
    environment.data_dirs.clear();
    CHECK_FALSE(platform::set_file_manager(environment, "/tmp/rummage", "", "").ok);
}

TEST_CASE("file manager executable: preserve Nix public wrapper") {
    TempDir dir;
    const auto wrapped = dir.write(".rummage-wrapped", "binary");
    CHECK(platform::file_manager_program(wrapped) == wrapped);
    const auto launcher = dir.write("rummage", "wrapper");
    CHECK(platform::file_manager_program(wrapped) == launcher);
    CHECK(platform::file_manager_program(launcher) == launcher);
    CHECK_FALSE(platform::file_manager_program("relative/rummage"));
}

TEST_CASE("browser launch: bounds, absolute paths, and profile isolation") {
    platform::BrowserRequest request{
        platform::Show::folders, {"/unmounted/no-stat", "/tmp/a\nb"}, "token", false};
    CHECK(platform::valid_browser_request(request));
    request.paths = {"relative"};
    CHECK_FALSE(platform::valid_browser_request(request));
    request.paths = {std::string{"/nul\0suffix", 11}};
    CHECK_FALSE(platform::valid_browser_request(request));
    request.paths = {"/" + std::string(4096, 'x')};
    CHECK_FALSE(platform::valid_browser_request(request));
    request.paths.assign(257, "/tmp");
    CHECK_FALSE(platform::valid_browser_request(request));
    request.paths.assign(256, "/tmp");
    CHECK(platform::valid_browser_request(request));
    request.token = std::string(4097, 'x');
    CHECK_FALSE(platform::valid_browser_request(request));
    request.token.clear();
    request.paths.clear();
    CHECK_FALSE(platform::valid_browser_request(request));
    request.paths = {"trash:///"};
    CHECK(platform::valid_browser_request(request));
    request.show = static_cast<platform::Show>(255);
    CHECK_FALSE(platform::valid_browser_request(request));
    const auto name = platform::browser_instance_name("/config/a", "/run/user/1000/wayland-0");
    CHECK(name == platform::browser_instance_name("/config/a", "/run/user/1000/wayland-0"));
    CHECK(name != platform::browser_instance_name("/config/b", "/run/user/1000/wayland-0"));
    CHECK(name != platform::browser_instance_name("/config/a", "/run/user/1000/wayland-1"));
    CHECK(platform::browser_instance_name("ab", "c") != platform::browser_instance_name("a", "bc"));
}
