#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace rummage::i18n::detail {
inline constexpr std::array<std::string_view, 6> kPluralForms{"zero", "one",  "two",
                                                              "few",  "many", "other"};

// Integer cardinal rules only. No evaluation of code from a catalog.
struct Relation {
    std::uint64_t modulus{};
    bool negate{};
    bool disjunction{};  // starts a new OR group; AND binds more tightly
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
};

struct Rule {
    std::vector<Relation> relations;
    [[nodiscard]] bool matches(std::uint64_t count) const;
};

// n [% integer] (= | !=) integer[..integer][, ...], joined by and/or.
// Rejects overflow, zero divisors, reversed ranges and excessive input.
[[nodiscard]] std::optional<Rule> parse_rule(std::string_view source);
}  // namespace rummage::i18n::detail
