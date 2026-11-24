#include "rummage/fs/path_completion.hpp"

#include <algorithm>

#include "rummage/core/i18n.hpp"
#include "rummage/fs/archive_folder.hpp"
#include "rummage/fs/directory.hpp"
#include "rummage/fs/places.hpp"

namespace rummage::fs {
PathTarget resolve_path(const std::filesystem::path& path) {
    PathTarget result;
    std::error_code error;
    if (std::filesystem::is_directory(path, error)) {
        result.folder = path;
        return result;
    }
    if (const auto location = find_archive(path)) {
        const auto tree = archive_tree(location->archive, nullptr);
        if (tree->failure() == archive::Failure::none) {
            if (tree->folder(location->inner))
                result.folder = path;
            else if (tree->entry(location->inner)) {
                result.folder = path.parent_path();
                result.file = path.filename().string();
            } else
                result.error = std::string{i18n::tr("No such file or folder")};
            return result;
        }
    }
    error.clear();
    if (std::filesystem::exists(path, error)) {
        result.folder = path.parent_path();
        result.file = path.filename().string();
    } else
        result.error = error ? error.message() : std::string{i18n::tr("No such file or folder")};
    return result;
}

PathMatches complete_path(const std::string& text, const std::filesystem::path& current,
                          const std::filesystem::path& home, const core::CancelToken& token) {
    PathMatches result;
    if (text.find('\0') != std::string::npos) {
        result.error = std::string{i18n::tr("Invalid path")};
        return result;
    }
    auto location = parse_location(text, home);
    if (!location && !text.empty() && text.find("://") == std::string::npos && text.front() != '~')
        location = Location{.folder = current / text, .uri = {}};
    if (!location || !location->uri.empty()) {
        result.error = std::string{i18n::tr("Use a local path for completion")};
        return result;
    }
    auto path = location->folder;
    const bool slash = text.ends_with('/') || text == "~";
    const auto parent = slash ? path : path.parent_path();
    const auto prefix = slash ? std::string{} : path.filename().string();
    const auto add = [&](const Entry& entry) {
        if (token.cancelled() || !entry.name.starts_with(prefix) ||
            (entry.name.starts_with('.') && !prefix.starts_with('.')))
            return;
        // Return absolute decoded paths. Percent escapes remain URI syntax only at input.
        result.paths.push_back((parent / entry.name).string() + (entry.is_directory() ? "/" : ""));
    };
    if (const auto archive = find_archive(parent)) {
        const auto tree = archive_tree(archive->archive, nullptr);
        if (const auto* entries = tree->folder(archive->inner))
            for (const auto& entry : *entries)
                add(entry);
        else
            result.error =
                tree->detail().empty() ? std::string{i18n::tr("No such folder")} : tree->detail();
    } else {
        const auto read = read_directory(parent, token, {}, [&](auto entries) {
            for (const auto& entry : entries)
                add(entry);
        });
        if (read.error)
            result.error = read.error.message();
    }
    std::ranges::sort(result.paths);
    return result;
}
}  // namespace rummage::fs
