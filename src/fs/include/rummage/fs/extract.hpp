#pragma once

// Extracting from an archive (ROADMAP 0.9.0, step 4): what a copy does when its source folder is
// inside an archive ("…/photos.zip/docs"). Blocking, long: TransferQueue runs it.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/transfer.hpp"

namespace rummage::fs {

// Above this, an archive that unpacks to more than kAskRatio times its own size is asked about
// before anything is written (a "zip bomb" is mostly that).
inline constexpr std::uint64_t kAskAbove = std::uint64_t{1} << 30;
inline constexpr std::uint64_t kAskRatio = 100;

// Extracts request.names from the archive folder request.source into request.destination (into a
// new folder request.extract_into there, when set).
//
// - The engine reads the bytes; rummage alone names and makes every file and folder, all inside
//   a hidden folder of the destination (".rummage-extract-XXXXXX"), each opened with openat2
//   (RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS: nothing can lead out of it, not even a link made
//   earlier) and O_EXCL; only once everything is written are the items renamed into the
//   destination, never over anything: a name taken gets "x 2". Cancelled, nothing is published.
// - Permissions are the archive's, without setuid, setgid or sticky bits (and readable by the
//   owner); modification times are the archive's.
// - Symbolic links are made only when they point inside what is extracted (relative, never
//   above its top); hard links only to a file extracted in the same run. Encrypted entries, those
//   compressed in a way not read yet, devices and fifos are skipped and said in errors.
// - Refused before anything is written: more bytes than the destination has free; asked through
//   callbacks.resolve (a Conflict with a question): more than kAskAbove that is more than
//   kAskRatio times the archive's size. An entry never yields more bytes than the archive says.
[[nodiscard]] TransferResult run_extract(const TransferRequest& request,
                                         const core::CancelToken& token,
                                         const TransferCallbacks& callbacks);

// Files of an archive folder extracted for another program to open (Enter on a file inside an
// archive): into a new private folder (0700) under $XDG_CACHE_HOME/rummage/opened, where folders
// older than a day are removed first. No question is asked: an archive that would be asked about
// is refused.
struct ExtractedToOpen {
    std::filesystem::path folder;  // where they are (empty: nothing could be made)
    TransferResult result;
};

[[nodiscard]] ExtractedToOpen extract_to_open(const std::filesystem::path& source,
                                              const std::vector<std::string>& names,
                                              const core::CancelToken& token);

// Where extract_to_open puts them.
[[nodiscard]] std::filesystem::path opened_files_folder();

}  // namespace rummage::fs
