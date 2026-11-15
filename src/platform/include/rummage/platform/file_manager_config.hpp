#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "rummage/platform/desktop_entry.hpp"

// `rummage --set-file-manager` / `--unset-file-manager` (issue #16): rummage as the program
// "Show in Folder" reaches, by a D-Bus service file for org.freedesktop.FileManager1 in the
// user's own dbus-1/services, which the session bus reads before the system's (where Thunar,
// Nautilus or Dolphin put theirs). Never installed for everyone: two system files for one name
// would be a conflict.
namespace rummage::platform {

inline constexpr std::string_view kFileManagerBusName = "org.freedesktop.FileManager1";
inline constexpr std::string_view kFileManagerService = "org.freedesktop.FileManager1.service";

// The service file's text: the name, and rummage --file-manager1 at executable (quoted with D-Bus
// Exec escaping), with a first line saying who wrote it.
[[nodiscard]] std::string file_manager_service(const std::filesystem::path& executable);

// Whether text is a service file set_file_manager() wrote.
[[nodiscard]] bool is_ours(std::string_view text);

// Where the user's service file goes: the first data dir (XDG_DATA_HOME)'s dbus-1/services.
[[nodiscard]] std::filesystem::path file_manager_service_path(const Environment& environment);

// Resolve the public launcher beside executable, including Nix's .rummage-wrapped binary.
// Falls back to executable for build trees and ordinary installations.
[[nodiscard]] std::optional<std::filesystem::path> file_manager_program(
    const std::filesystem::path& executable);

struct ManagerChange {
    bool ok{true};        // false: the file could not be written or removed
    std::string message;  // what was done, lines for the terminal
};

// Writes the service file naming executable; then says who holds the bus name now (owner: the
// program's name, empty when nobody does) and whether folders open in rummage (folder_app: the
// desktop file ID that opens inode/directory, empty when none).
[[nodiscard]] ManagerChange set_file_manager(const Environment& environment,
                                             const std::filesystem::path& executable,
                                             std::string_view owner, std::string_view folder_app);

// Removes the service file when it is ours; another one is left alone, and said.
[[nodiscard]] ManagerChange unset_file_manager(const Environment& environment);

// The program holding bus name on the session bus right now ("thunar"), from its process; empty
// when nobody does or the bus cannot be asked.
[[nodiscard]] std::string bus_name_owner(std::string_view name);

}  // namespace rummage::platform
