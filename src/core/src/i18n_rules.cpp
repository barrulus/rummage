#include "i18n_rules.hpp"

#include <algorithm>
#include <charconv>
#include <system_error>

namespace rummage::i18n::detail {
namespace {
class Reader {
public:
    explicit Reader(std::string_view source) : rest_{source} {}

    void spaces() {
        while (!rest_.empty() && (rest_.front() == ' ' || rest_.front() == '\t' ||
                                  rest_.front() == '\n' || rest_.front() == '\r'))
            rest_.remove_prefix(1);
    }

    bool take(std::string_view token) {
        spaces();
        if (!rest_.starts_with(token))
            return false;
        rest_.remove_prefix(token.size());
        return true;
    }

    std::optional<std::uint64_t> number() {
        spaces();
        std::uint64_t n{};
        const auto [end, error] = std::from_chars(rest_.data(), rest_.data() + rest_.size(), n);
        if (error != std::errc{})
            return std::nullopt;
        rest_.remove_prefix(static_cast<std::size_t>(end - rest_.data()));
        return n;
    }

    bool done() {
        spaces();
        return rest_.empty();
    }

private:
    std::string_view rest_;
};
}  // namespace

std::optional<Rule> parse_rule(std::string_view source) {
    if (source.empty() || source.size() > 1024)
        return std::nullopt;
    Reader in{source};
    Rule rule;
    bool disjunction = false;
    do {
        if (rule.relations.size() >= 32 || !in.take("n"))
            return std::nullopt;
        Relation relation;
        relation.disjunction = disjunction;
        if (in.take("%")) {
            const auto modulus = in.number();
            if (!modulus || *modulus == 0)
                return std::nullopt;
            relation.modulus = *modulus;
        }
        if (in.take("!="))
            relation.negate = true;
        else if (!in.take("="))
            return std::nullopt;
        do {
            const auto first = in.number();
            if (!first || relation.ranges.size() >= 32)
                return std::nullopt;
            const auto last = in.take("..") ? in.number() : first;
            if (!last || *last < *first)
                return std::nullopt;
            relation.ranges.emplace_back(*first, *last);
        } while (in.take(","));
        rule.relations.push_back(std::move(relation));
        if (in.done())
            return rule;
        if (in.take("or"))
            disjunction = true;
        else if (in.take("and"))
            disjunction = false;
        else
            return std::nullopt;
    } while (!in.done());
    return std::nullopt;  // trailing and/or
}

bool Rule::matches(std::uint64_t count) const {
    bool group = true;
    for (const Relation& relation : relations) {
        if (relation.disjunction) {
            if (group)
                return true;
            group = true;
        }
        const auto value = relation.modulus == 0 ? count : count % relation.modulus;
        const bool found = std::ranges::any_of(relation.ranges, [value](const auto& range) {
            return value >= range.first && value <= range.second;
        });
        group = group && (found != relation.negate);
    }
    return !relations.empty() && group;
}
}  // namespace rummage::i18n::detail
