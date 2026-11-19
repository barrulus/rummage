#include "rummage/theme/kde.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <fcntl.h>
#include <format>
#include <map>
#include <optional>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>

#include "rummage/core/config.hpp"
#include "rummage/theme/desktop.hpp"

namespace rummage::theme {
namespace {

constexpr std::size_t kMaxBytes = 1024 * 1024;
constexpr std::array<std::string_view, 4> kGroups{"Colors:View", "Colors:Window", "Colors:Button",
                                                  "Colors:Selection"};
constexpr std::array<std::string_view, 6> kKeys{"BackgroundNormal", "BackgroundAlternate",
                                                "ForegroundNormal", "ForegroundInactive",
                                                "ForegroundLink",   "ForegroundNegative"};

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r");
    return first == std::string_view::npos
               ? std::string_view{}
               : text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}

std::optional<Color> rgb(std::string_view value) {
    std::array<unsigned, 4> channels{0, 0, 0, 255};
    std::size_t count = 0;
    for (;;) {
        if (count == channels.size())
            return std::nullopt;
        const auto comma = value.find(',');
        const auto part = trim(value.substr(0, comma));
        if (part.empty())
            return std::nullopt;
        const auto [end, error] =
            std::from_chars(part.data(), part.data() + part.size(), channels[count]);
        if (error != std::errc{} || end != part.data() + part.size() || channels[count] > 255)
            return std::nullopt;
        ++count;
        if (comma == std::string_view::npos)
            break;
        value.remove_prefix(comma + 1);
    }
    if ((count != 3 && count != 4) || channels[3] != 255)
        return std::nullopt;
    return Color::rgb((channels[0] << 16U) | (channels[1] << 8U) | channels[2]);
}

Color mix(Color a, Color b, float amount) {
    return {a.r + (b.r - a.r) * amount, a.g + (b.g - a.g) * amount, a.b + (b.b - a.b) * amount,
            1.0F};
}

// Retain the requested ink where possible. Never accept a readable pair on just one of the
// surfaces that shares this role: a mixed light/dark scheme can require incompatible inks.
std::optional<Color> shared_ink(Color ink, const std::array<Color, 4>& surfaces) {
    const auto readable = [&](Color candidate) {
        return std::ranges::all_of(surfaces,
                                   [&](Color bg) { return contrast(candidate, bg) >= 4.5F; });
    };
    if (readable(ink))
        return ink;
    for (const auto surface : surfaces) {
        const auto candidate = readable_foreground(ink, surface);
        if (readable(candidate))
            return candidate;
    }
    for (const auto endpoint : {Color::rgb(0), Color::rgb(0xFFFFFF)})
        if (readable(endpoint))
            return endpoint;
    return std::nullopt;
}

}  // namespace

std::filesystem::path default_kde_colors_path() {
    const auto config = core::default_config_path();
    return config.empty() ? config : config.parent_path().parent_path() / "kdeglobals";
}

PaletteResult parse_kde_palette(std::string_view text, std::string_view source) {
    PaletteResult result;
    const auto problem = [&](std::size_t line, std::string_view detail) {
        if (result.problems.size() < 32)
            result.problems.push_back(
                std::format("{}:{}: {}; KDE palette ignored", source, line, detail));
    };
    if (text.size() > kMaxBytes || text.find('\0') != std::string_view::npos) {
        problem(1, "file exceeds 1 MiB or contains NUL bytes");
        return result;
    }
    using Group = std::map<std::string, Color, std::less<>>;
    std::map<std::string, Group, std::less<>> groups;
    std::string group;
    std::size_t number = 0;
    while (!text.empty()) {
        ++number;
        const auto newline = text.find('\n');
        auto line = trim(text.substr(0, newline));
        text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
        if (line.empty() || line.starts_with('#') || line.starts_with(';'))
            continue;
        if (line.starts_with('[')) {
            group.clear();
            const auto close = line.find(']');
            if (close == std::string_view::npos) {
                problem(number, "unterminated group");
                continue;
            }
            const auto name = line.substr(1, close - 1);
            const auto suffix = trim(line.substr(close + 1));
            if (std::ranges::find(kGroups, name) == kGroups.end())
                continue;
            if (suffix.empty() || suffix == "[$i]")
                group = name;
            else if (suffix != "[Inactive]")
                problem(number, "unsupported modifier on an active colour group");
            continue;
        }
        if (group.empty())
            continue;
        const auto equal = line.find('=');
        if (equal == std::string_view::npos) {
            problem(number, "expected a colour key=value entry");
            continue;
        }
        auto key = trim(line.substr(0, equal));
        if (key.ends_with("[$i]"))
            key.remove_suffix(4);
        const auto modifier = key.find('[');
        const auto bare_key = key.substr(0, modifier);
        if (std::ranges::find(kKeys, bare_key) == kKeys.end())
            continue;
        const auto value = rgb(trim(line.substr(equal + 1)));
        if (modifier != std::string_view::npos || !value) {
            problem(number, "supported colours must be opaque R,G,B or R,G,B,255 (0–255)");
        } else if (!groups[group].emplace(std::string{key}, *value).second) {
            problem(number, "duplicate colour entry");
        }
    }
    for (const auto name : kGroups) {
        for (const auto key : {"BackgroundNormal", "ForegroundNormal"}) {
            if (!groups[std::string{name}].contains(key))
                problem(1,
                        std::format("missing [{}] {}; apply a KDE colour scheme first", name, key));
        }
    }
    if (!result.problems.empty())
        return result;
    const auto get = [&](std::string_view name, std::string_view key, Color fallback) {
        const auto& values = groups.find(name)->second;
        const auto found = values.find(key);
        return found == values.end() ? fallback : found->second;
    };
    const auto required = [&](std::string_view name, std::string_view key) {
        return groups.find(name)->second.find(key)->second;
    };
    const Color view = required("Colors:View", "BackgroundNormal");
    const Color window = required("Colors:Window", "BackgroundNormal");
    const Color button = required("Colors:Button", "BackgroundNormal");
    const Color alternate = get("Colors:View", "BackgroundAlternate", view);
    const Color text_color = required("Colors:View", "ForegroundNormal");
    const Color selection = required("Colors:Selection", "BackgroundNormal");
    const std::array surfaces{view, window, button, alternate};
    const auto ink = shared_ink(text_color, surfaces);
    if (!ink) {
        problem(1, "view, window and button backgrounds require incompatible text colours");
        return result;
    }
    const bool dark = contrast(Color::rgb(0), *ink) > contrast(Color::rgb(0), view);
    Palette p = desktop_palette({.dark = dark, .accent = selection});
    p.surface = view;
    p.surface_variant = alternate;
    p.surface_container_lowest = view;
    p.surface_container_low = window;
    p.surface_container = window;
    p.surface_container_high = window;
    p.surface_container_highest = button;
    const auto ordered = std::minmax_element(
        surfaces.begin(), surfaces.end(),
        [](Color a, Color b) { return contrast(Color::rgb(0), a) < contrast(Color::rgb(0), b); });
    p.surface_dim = *ordered.first;
    p.surface_bright = *ordered.second;
    p.on_surface = *ink;
    p.on_surface_variant =
        shared_ink(get("Colors:View", "ForegroundInactive", *ink), surfaces).value_or(*ink);
    p.primary = selection;
    p.on_primary = readable_foreground(required("Colors:Selection", "ForegroundNormal"), selection);
    p.primary_container = selection;
    p.on_primary_container = p.on_primary;
    p.hover = selection;
    p.on_hover = p.on_primary;
    p.secondary =
        shared_ink(get("Colors:View", "ForegroundLink", p.secondary), surfaces).value_or(*ink);
    p.on_secondary = readable_foreground(p.on_secondary, p.secondary);
    p.error =
        shared_ink(get("Colors:View", "ForegroundNegative", p.error), surfaces).value_or(*ink);
    p.on_error = readable_foreground(p.on_error, p.error);
    p.outline = p.on_surface_variant;
    p.outline_variant = mix(window, *ink, 0.3F);
    result.palette = p;
    result.from_file = true;
    return result;
}

PaletteResult load_kde_palette(const std::filesystem::path& path) {
    if (path.empty())
        return {};
    PaletteResult result;
    const auto problem = [&](std::string_view detail) {
        result.problems.push_back(
            std::format("{}: {}; KDE palette ignored", path.string(), detail));
    };
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        if (errno != ENOENT && errno != ENOTDIR)
            problem(std::error_code{errno, std::generic_category()}.message());
        return result;
    }

    struct Close {
        int fd;

        ~Close() { close(fd); }
    } closer{fd};
    struct stat info{};
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        std::cmp_greater(info.st_size, kMaxBytes)) {
        problem("expected a regular file no larger than 1 MiB");
        return result;
    }
    std::string text;
    std::array<char, 4096> buffer{};
    for (;;) {
        const auto size = read(fd, buffer.data(), buffer.size());
        if (size < 0 && errno == EINTR)
            continue;
        if (size < 0) {
            problem("read failed");
            return result;
        }
        if (size == 0)
            break;
        text.append(buffer.data(), static_cast<std::size_t>(size));
        if (text.size() > kMaxBytes) {
            problem("file exceeds 1 MiB");
            return result;
        }
    }
    return parse_kde_palette(text, path.string());
}

}  // namespace rummage::theme
