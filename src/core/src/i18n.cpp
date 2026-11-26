#include "rummage/core/i18n.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <toml++/toml.hpp>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "i18n_catalogs.hpp"
#include "i18n_rules.hpp"
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

struct Message {
    std::string text;
    std::array<std::string, 6> forms;
};

using Messages = std::unordered_map<std::string, Message, TextHash, std::equal_to<>>;
using Contexts = std::unordered_map<std::string, Messages, TextHash, std::equal_to<>>;

Messages messages_of(const toml::table* table) {
    Messages out;
    if (!table)
        return out;
    for (const auto& [key, node] : *table) {
        Message message;
        if (const auto value = node.value<std::string>(); value && !value->empty()) {
            message.text = *value;
        } else if (const auto* forms = node.as_table()) {
            for (const auto& [form, text] : *forms) {
                const auto it = std::ranges::find(detail::kPluralForms, form.str());
                const auto form_value = text.value<std::string>();
                if (it == detail::kPluralForms.end() || !form_value || form_value->empty())
                    throw std::runtime_error("invalid plural form");
                message.forms[static_cast<std::size_t>(it - detail::kPluralForms.begin())] =
                    *form_value;
            }
            if (message.forms.back().empty())
                throw std::runtime_error("plural message needs other");
        } else {
            throw std::runtime_error("message must be text or a plural table");
        }
        out.emplace(std::string{key.str()}, std::move(message));
    }
    return out;
}

// A catalog once parsed. Its strings never move again: tr() hands out views of them.
struct Language {
    std::string code;
    std::string name;
    std::string decimal_separator{"."};
    Texts keys;  // [keys]: key names on the language's keyboards
    Messages texts;
    Contexts contexts;
    std::array<std::optional<detail::Rule>, 5> rules;
    bool valid{true};
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
        for (const auto* section : {"language", "texts", "contexts"}) {
            if (root[section] && !root[section].is_table())
                throw std::runtime_error("language, texts and contexts must be tables");
        }
        if (const auto* about = root["language"].as_table()) {
            if (const auto name = (*about)["name"].value<std::string>()) {
                language.name = *name;
            }
            if (const auto separator = (*about)["decimal_separator"].value<std::string>();
                separator && !separator->empty()) {
                language.decimal_separator = *separator;
            }
        }
        language.rules[1] = detail::parse_rule("n = 1");  // existing two-form catalogs
        if (const auto* rules = root["language"]["plural_rules"].as_table()) {
            language.rules = {};
            for (const auto& [form, value] : *rules) {
                const auto it = std::ranges::find(detail::kPluralForms, form.str());
                const auto text = value.value<std::string>();
                if (it >= detail::kPluralForms.end() - 1 || !text)
                    throw std::runtime_error("invalid plural category/rule");
                auto rule = detail::parse_rule(*text);
                if (!rule)
                    throw std::runtime_error("invalid integer plural rule");
                language.rules[static_cast<std::size_t>(it - detail::kPluralForms.begin())] =
                    std::move(rule);
            }
        } else if (root["language"]["plural_rules"]) {
            throw std::runtime_error("plural_rules must be a table");
        }
        language.keys = strings_of(root["keys"].as_table(), catalog.code, "keys");
        language.texts = messages_of(root["texts"].as_table());
        if (const auto* contexts = root["contexts"].as_table()) {
            for (const auto& [context, messages] : *contexts) {
                if (context.str().empty() || !messages.is_table())
                    throw std::runtime_error("context must be a named table");
                language.contexts.emplace(std::string{context.str()},
                                          messages_of(messages.as_table()));
            }
        }
    } catch (const toml::parse_error& e) {
        log::error("translation {}: {}; it is not used", catalog.code, e.description());
        language.valid = false;
    } catch (const std::runtime_error& e) {
        log::error("translation {}: {}; it is not used", catalog.code, e.what());
        language.valid = false;
    }
    return language;
}

// Every compiled-in catalog, parsed once on first use, in code order.
const std::vector<Language>& all() {
    static const std::vector<Language> kParsed = [] {
        std::vector<Language> out;
        for (const detail::Catalog& catalog : detail::catalogs()) {
            if (catalog.code != kEnglish) {
                auto language = parse(catalog);
                if (language.valid)
                    out.push_back(std::move(language));
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

void configure_language(std::string_view code) {
    set_language(code == "system" ? from_environment() : code);
}

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
        const auto append = [&out](const Messages& messages, std::string_view context) {
            for (const auto& [english, message] : messages) {
                if (!message.text.empty())
                    out.push_back({english, message.text, context, {}});
                for (std::size_t i = 0; i < message.forms.size(); ++i)
                    if (!message.forms[i].empty())
                        out.push_back(
                            {english, message.forms[i], context, detail::kPluralForms[i]});
            }
        };
        append(language->texts, {});
        for (const auto& [context, messages] : language->contexts)
            append(messages, context);
        std::ranges::sort(out, [](const Entry& a, const Entry& b) {
            return std::tie(a.context, a.english, a.form) < std::tie(b.context, b.english, b.form);
        });
    }
    return out;
}

namespace {
const Message* message_of(const Language* language, std::string_view context,
                          std::string_view english) {
    if (!language)
        return nullptr;
    const Messages* messages = &language->texts;
    if (!context.empty()) {
        const auto found = language->contexts.find(context);
        if (found == language->contexts.end())
            return nullptr;
        messages = &found->second;
    }
    const auto it = messages->find(english);
    return it == messages->end() ? nullptr : &it->second;
}

std::size_t category(const Language* language, std::uint64_t count) {
    if (!language)
        return count == 1 ? 1 : 5;
    for (std::size_t i = 0; i < language->rules.size(); ++i)
        if (language->rules[i] && language->rules[i]->matches(count))
            return i;
    return 5;
}
}  // namespace

std::string_view trc(std::string_view context, std::string_view english) {
    const auto* message = message_of(speaking(), context, english);
    return message && !message->text.empty() ? std::string_view{message->text} : english;
}

std::string_view tr(std::string_view english) { return trc({}, english); }

std::string_view plural_category(std::uint64_t count) {
    return detail::kPluralForms[category(speaking(), count)];
}

std::string_view detail::plural_text(std::string_view context, std::string_view singular,
                                     std::string_view plural, std::uint64_t count) {
    const auto* language = speaking();
    const auto* message = message_of(language, context, plural);
    if (message && !message->forms.back().empty()) {
        const auto& chosen = message->forms[category(language, count)];
        return chosen.empty() ? message->forms.back() : chosen;
    }
    return count == 1 ? singular : plural;
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
