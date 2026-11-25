#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Fuzzy matching for the command palette and finding files (ROADMAP 8a, 8b), on fzy's algorithm
// (third_party/fzy): the query's characters in order anywhere in the text, scored higher when
// they are together, at the start of a word or after a slash. Case does not matter (ASCII); bytes
// are compared as they are, so any UTF-8 text works, accented letters matching only themselves.
//
// Spaces split the query into terms, as fuzzel and Emacs' orderless do: every term must match,
// in any order ("ga st" finds "star … Games"; "monte rosa" finds "Monte Rosa" and "MonteRosa").
namespace rummage::views {

// UTF-8 filename prefix matching with simple Unicode case folding (no normalization).
// Malformed UTF-8 in the compared prefix never matches; no global locale is changed.
[[nodiscard]] bool filename_prefix_matches(std::string_view name, std::string_view prefix);

// fzy's limit: longer texts (or queries) never match.
inline constexpr std::size_t kFuzzyMaxLength = 1024;

// How well a relative path matches, for ordering found files (issue #25): first the tier, then
// the shallower path, then fzy's score.
struct PathRank {
    // How the name (the path's last part) holds the query, best first; with several terms the
    // weakest decides.
    enum class Tier : std::uint8_t {
        equal = 0,      // the name is the query ("SWAP" for "swap")
        prefix = 1,     // the name starts with it ("swapfile")
        contains = 2,   // the name holds it as typed ("swapfile" for "apfil")
        scattered = 3,  // its letters in order in the name ("app-filter.h" for "apfil")
        folders = 4,    // the folders are needed too
    };

    Tier tier{Tier::equal};
    std::size_t depth{0};  // the slashes in the path
    double score{0.0};     // fzy's, summed over the terms

    bool operator==(const PathRank&) const = default;
};

// a before b.
[[nodiscard]] bool better_rank(const PathRank& a, const PathRank& b) noexcept;

// A query prepared once for many texts. Not thread-safe: one per thread.
class FuzzyQuery {
public:
    explicit FuzzyQuery(std::string_view query);

    // Nothing to match: every text matches with 0.
    [[nodiscard]] bool empty() const noexcept { return terms_.empty(); }

    // The score of text (every term matching it), higher is better; nullopt when one does not.
    [[nodiscard]] std::optional<double> score(std::string_view text);

    // The byte offsets in text of the terms' characters as matched, ascending, each once; empty
    // when a term does not match or the query is empty.
    [[nodiscard]] std::vector<std::size_t> positions(std::string_view text);

    // For a relative path: its rank, each term matched in the name when it can be and across the
    // whole path when not; nullopt when a term matches neither.
    [[nodiscard]] std::optional<PathRank> path_rank(std::string_view path);

    // The positions shown for a path: each term's in the name when it matches there, else
    // across the path.
    [[nodiscard]] std::vector<std::size_t> path_positions(std::string_view path);

private:
    // text with a NUL after it, for fzy; false when term cannot be matched in it at all.
    bool prepare(const std::string& term, std::string_view text);
    std::optional<double> term_score(const std::string& term, std::string_view text);
    std::vector<std::size_t> term_positions(const std::string& term, std::string_view text);

    std::vector<std::string> terms_;  // lowercase, no spaces
    std::string haystack_;
    std::string lower_;  // the name in lowercase (ASCII), for the tiers
};

// One-off forms of FuzzyQuery::score and positions.
[[nodiscard]] std::optional<double> fuzzy_score(std::string_view query, std::string_view text);
[[nodiscard]] std::vector<std::size_t> fuzzy_positions(std::string_view query,
                                                       std::string_view text);

}  // namespace rummage::views
