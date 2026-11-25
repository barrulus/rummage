#include "rummage/views/file_clipboard.hpp"

#include <algorithm>
#include <utility>

#include "rummage/core/file.hpp"
#include "rummage/thumbs/cache.hpp"

namespace rummage::views {
namespace {

constexpr std::array<std::pair<std::string_view, std::string_view>, 8> kImages{
    {{"image/png", ".png"},
     {"image/jpeg", ".jpg"},
     {"image/webp", ".webp"},
     {"image/gif", ".gif"},
     {"image/tiff", ".tiff"},
     {"image/bmp", ".bmp"},
     {"image/svg+xml", ".svg"},
     {"image/jxl", ".jxl"}}};

bool image_signature(std::string_view type, std::string_view bytes) {
    using namespace std::string_view_literals;
    if (type == "image/png")
        return bytes.starts_with("\x89PNG\r\n\x1a\n"sv);
    if (type == "image/jpeg")
        return bytes.starts_with("\xff\xd8\xff"sv);
    if (type == "image/gif")
        return bytes.starts_with("GIF87a") || bytes.starts_with("GIF89a");
    if (type == "image/webp")
        return bytes.size() >= 12 && bytes.starts_with("RIFF") && bytes.substr(8, 4) == "WEBP";
    if (type == "image/tiff")
        return bytes.starts_with("II\x2a\0"sv) || bytes.starts_with("MM\0\x2a"sv) ||
               bytes.starts_with("II\x2b\0"sv) || bytes.starts_with("MM\0\x2b"sv);
    if (type == "image/bmp")
        return bytes.size() >= 14 && bytes.starts_with("BM");
    if (type == "image/jxl")
        return bytes.starts_with("\xff\x0a"sv) || bytes.starts_with("\0\0\0\x0cJXL \r\n\x87\n"sv);
    if (type != "image/svg+xml")
        return false;
    // Inspect only a bounded XML preamble. No XML parsing, entities or resource loading.
    bytes = bytes.substr(0, 4096);
    if (bytes.starts_with("\xef\xbb\xbf"sv))
        bytes.remove_prefix(3);
    for (;;) {
        const auto first = bytes.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos)
            return false;
        bytes.remove_prefix(first);
        if (bytes.starts_with("<?xml")) {
            const auto end = bytes.find("?>");
            if (end == std::string_view::npos)
                return false;
            bytes.remove_prefix(end + 2);
        } else if (bytes.starts_with("<!--")) {
            const auto end = bytes.find("-->");
            if (end == std::string_view::npos)
                return false;
            bytes.remove_prefix(end + 3);
        } else {
            return bytes.size() > 4 && bytes.starts_with("<svg") &&
                   std::string_view{" \t\r\n/>"}.find(bytes[4]) != std::string_view::npos;
        }
    }
}

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

std::optional<std::string> content_offer(std::span<const std::string> types) {
    for (const auto& [type, extension] : kImages) {
        if (std::ranges::find(types, type) != types.end())
            return std::string{type};
    }
    for (const auto type : kTextTypes)
        if (std::ranges::find(types, type) != types.end())
            return std::string{type};
    return std::nullopt;
}

std::optional<std::string_view> content_extension(std::string_view type, std::string_view bytes) {
    if (bytes.size() > kMaxContentBytes)
        return std::nullopt;
    for (const auto& [image, extension] : kImages)
        if (type == image)
            return image_signature(type, bytes) ? std::optional{extension} : std::nullopt;
    if (std::ranges::find(kTextTypes, type) != kTextTypes.end())
        return ".txt";
    return std::nullopt;
}

std::optional<std::string> dropped_web_url(std::string_view bytes) {
    if (bytes.size() > kMaxContentBytes)
        return std::nullopt;
    std::optional<std::string> url;
    bool invalid = false;
    each_line(bytes, [&](std::string_view line) {
        if (line.empty() || line.starts_with('#'))
            return;
        const auto prefix = line.starts_with("https://")  ? 8U
                            : line.starts_with("http://") ? 7U
                                                          : 0U;
        if (url || prefix == 0 || line.size() <= prefix || line[prefix] == '/' ||
            line[prefix] == '?' || line[prefix] == '#' ||
            std::ranges::any_of(line, [](unsigned char c) { return c <= 32 || c == 127; })) {
            invalid = true;
            return;
        }
        url = line;
    });
    return invalid ? std::nullopt : url;
}

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
