#include "rummage/core/i18n.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>
#include <toml++/toml.hpp>
#include <unordered_map>
#include <vector>

#include "i18n_catalogs.hpp"
#include "rummage/core/log.hpp"

namespace rummage::i18n {
namespace {

struct TextHash {
    using is_transparent = void;  // NOLINT(readability-identifier-naming): the standard's name

    std::size_t operator()(std::string_view text) const noexcept {
        return std::hash<std::string_view>{}(text);
    }
};

using Texts = std::unordered_map<std::string, std::string, TextHash, std::equal_to<>>;

// A catalog once parsed. Its strings never move again: tr() hands out views of them.
struct Language {
    std::string code;
    std::string name;
    std::string decimal_separator{"."};
    Texts keys;   // [keys]: key names on the language's keyboards
    Texts texts;  // [texts]: English text -> translation
};

// The string values of a TOML table; anything else is reported and left out.
Texts strings_of(const toml::table* table, std::string_view code, std::string_view section) {
    Texts out;
    if (table == nullptr) {
        return out;
    }
    for (const auto& [key, node] : *table) {
        if (const auto value = node.value<std::string>(); value && !value->empty()) {
            out.emplace(std::string{key.str()}, *value);
        } else {
            log::warn("translation {}: [{}] \"{}\" is not a text; left out", code, section,
                      key.str());
        }
    }
    return out;
}

Language parse(const detail::Catalog& catalog) {
    Language language;
    language.code = catalog.code;
    language.name = catalog.code;
    try {
        const toml::table root = toml::parse(catalog.toml, std::string{catalog.code} + ".toml");
        if (const auto* about = root["language"].as_table()) {
            if (const auto name = (*about)["name"].value<std::string>()) {
                language.name = *name;
            }
            if (const auto separator = (*about)["decimal_separator"].value<std::string>();
                separator && !separator->empty()) {
                language.decimal_separator = *separator;
            }
        }
        language.keys = strings_of(root["keys"].as_table(), catalog.code, "keys");
        language.texts = strings_of(root["texts"].as_table(), catalog.code, "texts");
    } catch (const toml::parse_error& e) {
        log::error("translation {}: {}; it is not used", catalog.code, e.description());
    }
    return language;
}

// Every compiled-in catalog, parsed once on first use, in code order.
const std::vector<Language>& all() {
    static const std::vector<Language> kParsed = [] {
        std::vector<Language> out;
        for (const detail::Catalog& catalog : detail::catalogs()) {
            if (catalog.code != kEnglish) {
                out.push_back(parse(catalog));
            }
        }
        return out;
    }();
    return kParsed;
}

const Language* find(std::string_view code) {
    const auto& languages = all();
    const auto it = std::ranges::find(languages, code, &Language::code);
    return it == languages.end() ? nullptr : &*it;
}

std::string from_environment() {
    // glibc's order for message catalogs: LANGUAGE (a list) unless the locale is C, then LC_ALL,
    // LC_MESSAGES, LANG.
    std::string_view locale;
    for (const char* name : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char* value = std::getenv(name);  // NOLINT(concurrency-mt-unsafe): read at start
        if (value != nullptr && *value != '\0') {
            locale = value;
            break;
        }
    }
    if (locale == "C" || locale == "POSIX" || locale.starts_with("C.")) {
        return std::string{kEnglish};
    }
    const char* list = std::getenv("LANGUAGE");  // NOLINT(concurrency-mt-unsafe)
    if (list != nullptr && *list != '\0' && !locale.empty()) {
        return language_of(list);
    }
    return language_of(locale);
}

// The interface's language; nullptr is English.
std::atomic<const Language*>& current() {
    static std::atomic<const Language*> language{find(from_environment())};
    return language;
}

const Language* speaking() { return current().load(std::memory_order_relaxed); }

}  // namespace

std::string_view language() {
    const Language* now = speaking();
    return now == nullptr ? kEnglish : std::string_view{now->code};
}

void set_language(std::string_view code) { current().store(find(code), std::memory_order_relaxed); }

std::string language_of(std::string_view locales) {
    while (!locales.empty()) {
        const auto colon = locales.find(':');
        std::string_view locale = locales.substr(0, colon);
        // "de_AT.UTF-8@euro" -> "de_AT": the encoding and modifier do not choose a language.
        locale = locale.substr(0, locale.find_first_of(".@"));
        const std::string_view base = locale.substr(0, locale.find('_'));
        if (locale == "C" || locale == "POSIX" || base == kEnglish) {
            return std::string{kEnglish};
        }
        for (const std::string_view candidate : {locale, base}) {
            if (!candidate.empty() && find(candidate) != nullptr) {
                return std::string{candidate};
            }
        }
        if (colon == std::string_view::npos) {
            break;
        }
        locales.remove_prefix(colon + 1);
    }
    return std::string{kEnglish};
}

std::vector<std::string_view> languages() {
    std::vector<std::string_view> out{kEnglish};
    for (const Language& language : all()) {
        out.emplace_back(language.code);
    }
    std::ranges::sort(out);
    return out;
}

std::string_view language_name(std::string_view code) {
    if (code == kEnglish) {
        return "English";
    }
    const Language* language = find(code);
    return language == nullptr ? code : std::string_view{language->name};
}

std::vector<Entry> entries(std::string_view code) {
    std::vector<Entry> out;
    if (const Language* language = find(code)) {
        out.reserve(language->texts.size());
        for (const auto& [english, translated] : language->texts) {
            out.push_back({english, translated});
        }
        std::ranges::sort(out, {}, &Entry::english);
    }
    return out;
}

std::string_view tr(std::string_view english) {
    if (const Language* now = speaking()) {
        if (const auto it = now->texts.find(english); it != now->texts.end()) {
            return it->second;
        }
    }
    return english;
}

std::string keys(std::string_view text) {
    const Language* now = speaking();
    if (now == nullptr || now->keys.empty()) {
        return std::string{text};
    }
    std::string out;
    out.reserve(text.size() + 8);
    std::size_t i = 0;
    while (i < text.size()) {
        const auto separator = text.find_first_of("+ ,", i);
        const std::size_t end = separator == std::string_view::npos ? text.size() : separator;
        const std::string_view word = text.substr(i, end - i);
        const auto found = now->keys.find(word);
        out += found != now->keys.end() ? std::string_view{found->second} : word;
        if (end < text.size()) {
            out += text[end];
        }
        i = end + 1;
    }
    return out;
}

std::string decimal(std::string text) {
    const Language* now = speaking();
    if (now == nullptr || now->decimal_separator == ".") {
        return text;
    }
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '.' && i > 0 && i + 1 < text.size() && digit(text[i - 1]) &&
            digit(text[i + 1])) {
            out += now->decimal_separator;
        } else {
            out += text[i];
        }
    }
    return out;
}

}  // namespace rummage::i18n
