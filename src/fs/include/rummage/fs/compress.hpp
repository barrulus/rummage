#pragma once

// Making archives (ROADMAP 0.9.0, step 5): "Compress to .zip" and "Compress to .tar.gz". Blocking,
// long: TransferQueue runs it. The files are the user's own, read as a copy reads them; rummage
// writes the archive itself (zlib deflates), no engine needed.

#include <string>
#include <vector>

#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/transfer.hpp"

namespace rummage::fs {

// Compresses request.names from request.source into one archive in request.destination, named
// request.archive_name ("x 2.zip" when taken): a zip (deflate, UTF-8 names, ZIP64 when large,
// Unix permissions and times) or, with request.tar_gz, a pax tar in gzip.
//
// - Written into a hidden temporary file (".rummage-part-XXXXXXXX") renamed to its name only when
//   complete, never over anything; stopped or failed, the temporary file goes.
// - Folders are walked without following links; symbolic links are stored as links; devices,
//   fifos and sockets are left out and said; a hard link is stored as the file it is.
// - Each file is opened without following links and must still be the file first looked at, and
//   be as long once read: else the archive is not made ("changed while being compressed").
// - Cancellation (token) is checked between pieces of a file.
[[nodiscard]] TransferResult run_compress(const TransferRequest& request,
                                          const core::CancelToken& token,
                                          const TransferCallbacks& callbacks);

// The name "Compress" proposes for names: the one item's (a file without its extension), else
// "Archive"; then ".zip" or ".tar.gz".
[[nodiscard]] std::string archive_name_for(const std::vector<std::string>& names,
                                           bool first_is_folder, bool tar_gz);

}  // namespace rummage::fs
