#include "rummage/platform/file_manager_config.hpp"

#include <cstdint>
#include <exception>
#include <format>
#include <fstream>
#include <memory>
#include <sdbus-c++/sdbus-c++.h>
#include <sstream>
#include <system_error>

#include "rummage/core/file.hpp"

namespace rummage::platform {
namespace {

constexpr std::string_view kMarker = "# Written by rummage --set-file-manager";

std::string read_whole(const std::filesystem::path& path) {
    std::ifstream in{path};
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

}  // namespace

std::string file_manager_service(const std::filesystem::path& executable) {
    // First quote the argv word. D-Bus parses Exec with shell-style quoting,
    // without a shell. Newlines are refused by set_file_manager().
    std::string program = "\"";
    for (char c : executable.string()) {
        if (c == '\\' || c == '"' || c == '$' || c == '`')
            program += '\\';
        program += c;
    }
    program += '"';
    // The service keyfile unescapes the value before Exec is split into argv.
    // Protect each quoting backslash from that first decoding pass.
    std::string value;
    for (char c : program) {
        if (c == '\\')
            value += '\\';
        value += c;
    }
    return std::format(
        "{}: \"Show in Folder\" opens rummage.\n"
        "# `rummage --unset-file-manager` removes this file.\n"
        "[D-BUS Service]\n"
        "Name=org.freedesktop.FileManager1\n"
        "Exec={} --file-manager1\n",
        kMarker, value);
}

bool is_ours(std::string_view text) { return text.starts_with(std::string{kMarker} + ": "); }

std::filesystem::path file_manager_service_path(const Environment& environment) {
    const std::filesystem::path home =
        environment.data_dirs.empty() ? std::filesystem::path{} : environment.data_dirs.front();
    return home / "dbus-1" / "services" / std::string{kFileManagerService};
}

std::optional<std::filesystem::path> file_manager_program(const std::filesystem::path& executable) {
    std::error_code error;
    if (!executable.is_absolute())
        return std::nullopt;
    const auto public_program = executable.parent_path() / "rummage";
    if (executable.filename() == ".rummage-wrapped" &&
        std::filesystem::is_regular_file(public_program, error)) {
        return public_program;
    }
    if (std::filesystem::is_regular_file(executable, error))
        return executable;
    return std::nullopt;
}

ManagerChange set_file_manager(const Environment& environment,
                               const std::filesystem::path& executable, std::string_view owner,
                               std::string_view folder_app) {
    const std::filesystem::path path = file_manager_service_path(environment);
    if (path.is_relative()) {
        return {.ok = false, .message = "no data folder to write the service file in\n"};
    }
    if (!executable.is_absolute() ||
        executable.string().find_first_of("\r\n") != std::string::npos) {
        return {.ok = false,
                .message = "the rummage executable needs an absolute path without newlines\n"};
    }
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (status.type() != std::filesystem::file_type::not_found &&
        (!std::filesystem::is_regular_file(status) || !is_ours(read_whole(path)))) {
        return {.ok = false,
                .message = std::format("{} is another program's: left alone. Move it away first "
                                       "if rummage should answer \"Show in Folder\".\n",
                                       path.string())};
    }
    if (const std::error_code failed =
            core::write_file_atomically(path, file_manager_service(executable))) {
        return {.ok = false,
                .message = std::format("cannot write {}: {}\n", path.string(), failed.message())};
    }
    std::string message =
        std::format("Wrote {}: \"Show in Folder\" now opens rummage.\n", path.string());
    if (!owner.empty() && owner != "rummage" && owner != ".rummage-wrapped" &&
        owner != ".rummage-wrappe") {
        message += std::format(
            "{} holds org.freedesktop.FileManager1 right now: rummage answers once it exits, or "
            "after you log out and in again.\n",
            owner);
    }
    if (folder_app != "rummage.desktop") {
        message += std::format(
            "Folders open with {}: `xdg-mime default rummage.desktop inode/directory` "
            "opens them in rummage too.\n",
            folder_app.empty() ? std::string_view{"no application"} : folder_app);
    }
    return {.ok = true, .message = std::move(message)};
}

ManagerChange unset_file_manager(const Environment& environment) {
    const std::filesystem::path path = file_manager_service_path(environment);
    std::error_code error;
    if (path.is_relative() || !std::filesystem::exists(path, error)) {
        return {.ok = true, .message = "rummage was not set to answer \"Show in Folder\".\n"};
    }
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(path, error)) ||
        !is_ours(read_whole(path))) {
        return {.ok = true,
                .message = std::format("{} is another program's: left alone.\n", path.string())};
    }
    if (!std::filesystem::remove(path, error) || error) {
        return {.ok = false,
                .message = std::format("cannot remove {}: {}\n", path.string(), error.message())};
    }
    return {.ok = true,
            .message = std::format("Removed {}: \"Show in Folder\" goes back to the desktop's file "
                                   "manager once rummage's service exits (30 s at most).\n",
                                   path.string())};
}

std::string bus_name_owner(std::string_view name) {
    try {
        const std::unique_ptr<sdbus::IConnection> bus = sdbus::createSessionBusConnection();
        const std::unique_ptr<sdbus::IProxy> daemon =
            sdbus::createProxy(*bus, sdbus::ServiceName{"org.freedesktop.DBus"},
                               sdbus::ObjectPath{"/org/freedesktop/DBus"});
        bool has_owner = false;
        daemon->callMethod("NameHasOwner")
            .onInterface("org.freedesktop.DBus")
            .withArguments(std::string{name})
            .storeResultsTo(has_owner);
        if (!has_owner) {
            return {};
        }
        std::uint32_t pid = 0;
        daemon->callMethod("GetConnectionUnixProcessID")
            .onInterface("org.freedesktop.DBus")
            .withArguments(std::string{name})
            .storeResultsTo(pid);
        std::string program = read_whole(std::format("/proc/{}/comm", pid));
        while (!program.empty() && (program.back() == '\n' || program.back() == '\r')) {
            program.pop_back();
        }
        return program.empty() ? std::string{"another program"} : program;
    } catch (const std::exception& /*error*/) {
        return {};
    }
}

}  // namespace rummage::platform
