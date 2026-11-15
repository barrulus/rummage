#include "rummage/views/file_clipboard.hpp"

#include <algorithm>
#include <utility>

#include "rummage/core/file.hpp"
#include "rummage/thumbs/cache.hpp"

namespace rummage::views {
namespace {

// Calls line for each line of text, CR before LF dropped.
template <class Line>
void each_line(std::string_view text, Line line) {
    while (!text.empty()) {
        const std::size_t end = std::min(text.find('\n'), text.size());
        std::string_view one = text.substr(0, end);
        text.remove_prefix(std::min(end + 1, text.size()));
        if (one.ends_with('\r')) {
            one.remove_suffix(1);
        }
        line(one);
    }
}

void add_uri(ClipboardFiles& files, std::string_view uri) {
    if (std::optional<std::filesystem::path> path = path_from_uri(uri)) {
        files.paths.push_back(std::move(*path));
    } else {
        ++files.refused;
    }
}

}  // namespace

std::string uri_list(std::span<const std::filesystem::path> paths) {
    std::string text;
    for (const std::filesystem::path& path : paths) {
        text += thumbs::file_uri(path);
        text += "\r\n";
    }
    return text;
}

std::string gnome_copied_files(bool cut, std::span<const std::filesystem::path> paths) {
    std::string text = cut ? "cut" : "copy";
    for (const std::filesystem::path& path : paths) {
        text += '\n';
        text += thumbs::file_uri(path);
    }
    return text;
}

std::string path_lines(std::span<const std::filesystem::path> paths) {
    std::string text;
    for (const std::filesystem::path& path : paths) {
        if (!text.empty()) {
            text += '\n';
        }
        text += path.string();
    }
    return text;
}

std::optional<std::filesystem::path> path_from_uri(std::string_view uri) {
    return core::path_from_uri(uri);
}

ClipboardFiles parse_uri_list(std::string_view text) {
    ClipboardFiles files;
    each_line(text, [&files](std::string_view line) {
        if (!line.empty() && !line.starts_with('#')) {
            add_uri(files, line);
        }
    });
    return files;
}

std::optional<ClipboardFiles> parse_gnome_copied_files(std::string_view text) {
    std::optional<ClipboardFiles> files;
    bool first = true;
    each_line(text, [&](std::string_view line) {
        if (std::exchange(first, false)) {
            if (line == "copy" || line == "cut") {
                files = ClipboardFiles{.cut = line == "cut", .paths = {}, .refused = 0};
            }
            return;
        }
        if (files && !line.empty()) {
            add_uri(*files, line);
        }
    });
    return files;
}

}  // namespace rummage::views
