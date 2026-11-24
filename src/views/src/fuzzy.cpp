#include "rummage/views/fuzzy.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fzy/src/match.h"  // NOLINT(misc-include-cleaner): has_match, match, match_positions

namespace rummage::views {
namespace {

// fzy gives +infinity to a whole-text match and -infinity when it cannot tell: kept finite so
// they still sort (and add up over the terms).
double finite(double score) {
    if (std::isinf(score)) {
        return score > 0.0 ? 1e9 : -1e9;
    }
    return score;
}

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

// The last part of a relative path, and where it starts.
std::size_t name_start(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? 0 : slash + 1;
}

}  // namespace

bool better_rank(const PathRank& a, const PathRank& b) noexcept {
    if (a.tier != b.tier) {
        return a.tier < b.tier;
    }
    if (a.depth != b.depth) {
        return a.depth < b.depth;
    }
    return a.score > b.score;
}

FuzzyQuery::FuzzyQuery(std::string_view query) {
    // fzy widens only lowercase query characters to either case: an uppercase one would miss a
    // lowercase letter. The text it lowercases itself.
    std::string term;
    for (const char c : query) {
        if (c == ' ' || c == '\0') {
            if (!term.empty()) {
                terms_.push_back(std::move(term));
                term.clear();
            }
        } else {
            term += lower(c);
        }
    }
    if (!term.empty()) {
        terms_.push_back(std::move(term));
    }
}

bool FuzzyQuery::prepare(const std::string& term, std::string_view text) {
    if (term.size() > text.size() || text.size() > kFuzzyMaxLength ||
        text.find('\0') != std::string_view::npos) {
        return false;
    }
    haystack_.assign(text);
    return has_match(term.c_str(), haystack_.c_str()) != 0;
}

std::optional<double> FuzzyQuery::term_score(const std::string& term, std::string_view text) {
    if (!prepare(term, text)) {
        return std::nullopt;
    }
    return finite(match(term.c_str(), haystack_.c_str()));
}

std::vector<std::size_t> FuzzyQuery::term_positions(const std::string& term,
                                                    std::string_view text) {
    if (!prepare(term, text)) {
        return {};
    }
    std::vector<std::size_t> out(term.size());
    match_positions(term.c_str(), haystack_.c_str(), out.data());
    return out;
}

std::optional<double> FuzzyQuery::score(std::string_view text) {
    double total = 0.0;
    for (const std::string& term : terms_) {
        const std::optional<double> found = term_score(term, text);
        if (!found) {
            return std::nullopt;
        }
        total += *found;
    }
    return total;
}

std::vector<std::size_t> FuzzyQuery::positions(std::string_view text) {
    std::vector<std::size_t> all;
    for (const std::string& term : terms_) {
        const std::vector<std::size_t> found = term_positions(term, text);
        if (found.empty()) {
            return {};
        }
        all.insert(all.end(), found.begin(), found.end());
    }
    std::ranges::sort(all);
    const auto [first, last] = std::ranges::unique(all);
    all.erase(first, last);
    return all;
}

std::optional<PathRank> FuzzyQuery::path_rank(std::string_view path) {
    PathRank rank{.depth = static_cast<std::size_t>(std::ranges::count(path, '/'))};
    const std::string_view name = path.substr(name_start(path));
    lower_.assign(name);
    std::ranges::transform(lower_, lower_.begin(), lower);
    for (const std::string& term : terms_) {
        PathRank::Tier tier = PathRank::Tier::folders;
        std::optional<double> found = term_score(term, name);
        if (found) {
            if (lower_ == term) {
                tier = PathRank::Tier::equal;
            } else if (lower_.starts_with(term)) {
                tier = PathRank::Tier::prefix;
            } else if (lower_.find(term) != std::string::npos) {
                tier = PathRank::Tier::contains;
            } else {
                tier = PathRank::Tier::scattered;
            }
        } else {
            found = term_score(term, path);
            if (!found) {
                return std::nullopt;
            }
        }
        rank.tier = std::max(rank.tier, tier);
        rank.score += *found;
    }
    return rank;
}

std::vector<std::size_t> FuzzyQuery::path_positions(std::string_view path) {
    const std::size_t start = name_start(path);
    std::vector<std::size_t> all;
    for (const std::string& term : terms_) {
        std::vector<std::size_t> found = term_positions(term, path.substr(start));
        if (found.empty()) {
            found = term_positions(term, path);
            if (found.empty()) {
                return {};
            }
        } else {
            for (std::size_t& at : found) {
                at += start;
            }
        }
        all.insert(all.end(), found.begin(), found.end());
    }
    std::ranges::sort(all);
    const auto [first, last] = std::ranges::unique(all);
    all.erase(first, last);
    return all;
}

std::optional<double> fuzzy_score(std::string_view query, std::string_view text) {
    return FuzzyQuery{query}.score(text);
}

std::vector<std::size_t> fuzzy_positions(std::string_view query, std::string_view text) {
    return FuzzyQuery{query}.positions(text);
}

}  // namespace rummage::views
