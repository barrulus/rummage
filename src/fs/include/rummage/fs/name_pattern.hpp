#pragma once
#include <algorithm>
#include <fnmatch.h>
#include <string>

namespace rummage::fs {
// Empty matches everything. Without wildcards, match literal text anywhere (including backslashes).
// Wildcard expressions use the same case-insensitive fnmatch rules as the file chooser.
[[nodiscard]] inline bool matches_name(const std::string& name, const std::string& pattern) {
    if (pattern.find_first_of("*?[") != std::string::npos)
        return fnmatch(pattern.c_str(), name.c_str(), FNM_CASEFOLD) == 0;
    const auto fold = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
    return std::search(name.begin(), name.end(), pattern.begin(), pattern.end(),
                       [fold](unsigned char a, unsigned char b) { return fold(a) == fold(b); }) !=
               name.end() ||
           pattern.empty();
}
}  // namespace rummage::fs
