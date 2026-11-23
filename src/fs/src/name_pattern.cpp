#include "rummage/fs/name_pattern.hpp"

#include <algorithm>
#include <re2/re2.h>

#include "rummage/core/i18n.hpp"

namespace rummage::fs {
NamePattern::Spec NamePattern::parse(const std::string& query) {
    Spec spec;
    std::string_view text = query;
    if (text.starts_with("case:")) {
        spec.case_sensitive = true;
        text.remove_prefix(5);
    }
    for (const auto& [prefix, mode] :
         {std::pair{"text:", Mode::text}, {"glob:", Mode::glob}, {"re:", Mode::regex}}) {
        if (text.starts_with(prefix)) {
            spec.mode = mode;
            text.remove_prefix(std::char_traits<char>::length(prefix));
            break;
        }
    }
    spec.text = text;
    return spec;
}

std::string NamePattern::encode(const Spec& spec) {
    std::string query = spec.case_sensitive ? "case:" : "";
    switch (spec.mode) {
        case Mode::automatic:
            break;
        case Mode::text:
            query += "text:";
            break;
        case Mode::glob:
            query += "glob:";
            break;
        case Mode::regex:
            query += "re:";
            break;
    }
    return query + spec.text;
}

NamePattern::NamePattern(const std::string& query) : spec_{parse(query)} {
    if (query.size() > 1024) {
        error_ = std::string{i18n::tr("Use a pattern of at most 1024 bytes")};
        return;
    }
    if (spec_.mode == Mode::automatic)
        spec_.mode = spec_.text.find_first_of("*?[") == std::string::npos ? Mode::text : Mode::glob;
    if (spec_.mode == Mode::text)
        return;
    std::string expression = spec_.text;
    if (spec_.mode == Mode::glob) {
        expression.clear();
        const auto incomplete = [this] {
            error_ = std::string{i18n::tr("Complete the glob escape or bracket expression")};
        };
        for (std::size_t i = 0; i < spec_.text.size(); ++i) {
            const char c = spec_.text[i];
            if (c == '*')
                expression += ".*";
            else if (c == '?')
                expression += '.';
            else if (c == '\\') {
                if (++i == spec_.text.size()) {
                    incomplete();
                    return;
                }
                expression += re2::RE2::QuoteMeta(spec_.text.substr(i, 1));
            } else if (c == '[') {
                expression += '[';
                ++i;
                if (i < spec_.text.size() && (spec_.text[i] == '!' || spec_.text[i] == '^')) {
                    expression += '^';
                    ++i;
                }
                if (i < spec_.text.size() && spec_.text[i] == ']') {
                    expression += "\\]";
                    ++i;
                }
                for (; i < spec_.text.size() && spec_.text[i] != ']'; ++i) {
                    if (spec_.text[i] == '[' && i + 1 < spec_.text.size() &&
                        spec_.text[i + 1] == ':') {
                        const auto end = spec_.text.find(":]", i + 2);
                        if (end == std::string::npos) {
                            incomplete();
                            return;
                        }
                        expression += spec_.text.substr(i, end + 2 - i);
                        i = end + 1;
                    } else if (spec_.text[i] == '\\') {
                        if (++i == spec_.text.size()) {
                            incomplete();
                            return;
                        }
                        expression += re2::RE2::QuoteMeta(spec_.text.substr(i, 1));
                    } else
                        expression += spec_.text[i];
                }
                if (i == spec_.text.size()) {
                    incomplete();
                    return;
                }
                expression += ']';
            } else
                expression += re2::RE2::QuoteMeta(spec_.text.substr(i, 1));
        }
    }
    re2::RE2::Options options;
    options.set_log_errors(false);
    options.set_case_sensitive(spec_.case_sensitive);
    options.set_dot_nl(true);
    options.set_max_mem(2 * 1024 * 1024);
    regex_ = std::make_shared<re2::RE2>(expression, options);
    if (!regex_->ok())
        error_ = i18n::trf("Invalid pattern: {}", regex_->error());
}

bool NamePattern::matches(const std::string& name) const {
    if (!error_.empty())
        return false;
    if (spec_.text.empty())
        return true;
    if (regex_)
        return spec_.mode == Mode::glob ? re2::RE2::FullMatch(name, *regex_)
                                        : re2::RE2::PartialMatch(name, *regex_);
    const auto fold = [sensitive = spec_.case_sensitive](unsigned char c) {
        return !sensitive && c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
    };
    return std::search(name.begin(), name.end(), spec_.text.begin(), spec_.text.end(),
                       [fold](unsigned char a, unsigned char b) { return fold(a) == fold(b); }) !=
           name.end();
}

bool matches_name(const std::string& name, const std::string& pattern) {
    return NamePattern{pattern}.matches(name);
}

std::string NamePattern::replace_all(std::string& text, const std::string& replacement) const {
    if (!error_.empty())
        return error_;
    if (spec_.mode != Mode::regex || !regex_)
        return std::string{i18n::tr("Choose a regular expression for regex replacement")};
    std::string error;
    if (!regex_->CheckRewriteString(replacement, &error))
        return i18n::trf("Invalid replacement: {}", error);
    re2::RE2::GlobalReplace(&text, *regex_, replacement);
    return {};
}
}  // namespace rummage::fs
