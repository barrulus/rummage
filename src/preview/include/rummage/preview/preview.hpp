#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "rummage/archive/archive.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/archive_folder.hpp"
#include "rummage/fs/entry.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/picture/load.hpp"
#include "rummage/picture/photo_info.hpp"
#include "rummage/text/text.hpp"

// What Quick Look shows for a file, made on a worker. One provider per kind of file (DESIGN,
// "Preview provider"): adding a kind does not touch the others, and the view draws what the
// provider made.
namespace rummage::preview {

// The beginning of a text file, ready to draw line by line (read by the base engine).
using Text = text::Text;

// What the information card shows beyond the directory entry itself.
struct Info {
    std::string permissions;  // as ls shows them: "-rw-r--r--", "drwxr-xr-x"
    std::string owner;        // user and group names (numbers when unknown)
    std::string group;
    std::string link_target;                 // for a symbolic link, where it points; else empty
    std::optional<std::uint64_t> items;      // a directory's entries, hidden ones included
    std::uint64_t hidden_items{0};           // of items, the hidden ones (dotfiles)
    std::optional<std::int64_t> created_ns;  // birth time, where the file system keeps it
    std::string error;                       // the file could not be examined
};

// What an archive holds (0.9.0), from the tree the engine's listing makes: Quick Look's page and
// the info pane show it.
struct Archive {
    archive::Format format{archive::Format::unknown};
    archive::Failure failure{archive::Failure::none};
    std::string detail;  // why it cannot be read, or only in part
    fs::ArchiveTotals totals;
    std::uint64_t left_out{0};   // names leading outside it, or a file where a folder is
    std::vector<fs::Entry> top;  // the first entries at its top: folders first, then by name
    std::uint64_t top_count{0};  // all the entries at its top
};

using Content = std::variant<picture::Loaded, Text, Info, Archive>;

// What a provider makes: the alternative of Content it returns (Text may become Info: a "text"
// file with NUL bytes in it).
enum class Kind : std::uint8_t { image, text, info, archive };

struct Request {
    std::filesystem::path path;
    std::string mime;  // the type rummage found (text: for the engine to choose a language by)
    // The area an image is shown in, in physical pixels.
    std::uint32_t width{0};
    std::uint32_t height{0};
    // Types an archive's entries by name (their icons); none: untyped.
    std::shared_ptr<const fs::MimeDatabase> types{};
};

// The most entries Archive::top holds for Quick Look.
inline constexpr std::size_t kArchiveTopEntries = 12;

class Provider {
public:
    Provider() = default;
    virtual ~Provider() = default;
    Provider(const Provider&) = delete;
    Provider& operator=(const Provider&) = delete;
    Provider(Provider&&) = delete;
    Provider& operator=(Provider&&) = delete;

    [[nodiscard]] virtual bool can_handle(const fs::MimeDatabase& mime, fs::MimeId type) const = 0;

    [[nodiscard]] virtual Kind kind() const = 0;

    // Blocking: call it on a worker. Never throws; problems end up in the content.
    [[nodiscard]] virtual Content load(const Request& request,
                                       const core::CancelToken& cancel) const = 0;
};

// The first provider that handles type: images and camera raw files, then text, then archives,
// then the information card, which handles everything.
[[nodiscard]] const Provider& provider_for(const fs::MimeDatabase& mime, fs::MimeId type);

// The building blocks of the providers.

// Permissions, owner, link target, birth time, and a directory's number of entries (stops
// counting when cancelled).
[[nodiscard]] Info read_info(const std::filesystem::path& path, const core::CancelToken& cancel);

// What the archive at path holds, with at most top of its top entries (types: as for Request).
// Blocking: the engine lists it (the tree is kept, as for browsing it).
[[nodiscard]] Archive read_archive(const std::filesystem::path& path,
                                   const std::shared_ptr<const fs::MimeDatabase>& types,
                                   std::size_t top);

// As ls -l shows mode: "-rw-r--r--", "drwxr-xr-x" (its type bits decide the first letter).
[[nodiscard]] std::string permissions_text(std::uint32_t mode);

// What the info pane shows about an entry, beyond the directory entry itself.
struct Summary {
    Info info;
    std::optional<picture::PhotoInfo> photo;  // images and camera raw files
    std::optional<Text> text;  // a text file's beginning (a log's end), as in Quick Look
    std::optional<Archive> archive;
};

// Reads it for an entry of type mime whose provider makes kind. Blocking: call it on a worker.
[[nodiscard]] Summary summarize(const std::filesystem::path& path, std::string_view mime, Kind kind,
                                const core::CancelToken& cancel,
                                const std::shared_ptr<const fs::MimeDatabase>& types = {});

}  // namespace rummage::preview
