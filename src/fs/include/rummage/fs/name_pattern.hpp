#pragma once
#include <memory>
#include <string>

namespace re2 {
class RE2;
}

namespace rummage::fs {
// Explicit prefixes: text:, glob:, re:, optionally preceded by case:.
// Unprefixed queries preserve the historical substring/automatic-wildcard behavior.
class NamePattern {
public:
    enum class Mode { automatic, text, glob, regex };

    struct Spec {
        Mode mode{Mode::automatic};
        bool case_sensitive{false};
        std::string text;
    };

    explicit NamePattern(const std::string& query);
    [[nodiscard]] static Spec parse(const std::string& query);
    [[nodiscard]] static std::string encode(const Spec& spec);

    [[nodiscard]] const std::string& error() const { return error_; }

    [[nodiscard]] bool matches(const std::string& name) const;

private:
    Spec spec_;
    std::shared_ptr<const re2::RE2> regex_;
    std::string error_;
};

[[nodiscard]] bool matches_name(const std::string& name, const std::string& pattern);
}  // namespace rummage::fs
