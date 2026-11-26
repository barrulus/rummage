#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Translations of the interface. The English text is the key: code keeps its English strings,
// wrapped in tr() or trf(), and a language's catalog maps them to its own words. No gettext and
// no files to install: every data/i18n/<code>.toml is compiled in, so a new language needs only
// its file (docs/translations.md). The language comes from the environment once (LANGUAGE,
// LC_ALL, LC_MESSAGES, LANG, the way glibc picks message catalogs); a text a catalog does not
// have stays English.
namespace rummage::i18n {

// English needs no catalog; its code is "en".
inline constexpr std::string_view kEnglish = "en";

struct Entry {
    std::string_view english;
    std::string_view translated;
    std::string_view context;
    std::string_view form;  // empty for a plain text; otherwise a plural category
};

// The code of the language the interface speaks ("de", "pt_BR", or "en").
[[nodiscard]] std::string_view language();

// Overrides the environment's choice (tests, a setting). A code without a catalog is English.
void set_language(std::string_view code);

// Apply a configured catalog at startup; "system" restores environment selection.
// Does not change locale environment variables or the process C locale.
void configure_language(std::string_view code);

// The language of a POSIX locale or LANGUAGE list ("de_DE.UTF-8", "de:en"): the first entry
// with a catalog, region first ("pt_BR", then "pt"). "en" when none has one; "C" and "POSIX"
// are English.
[[nodiscard]] std::string language_of(std::string_view locales);

// The codes with a catalog, sorted, and "en".
[[nodiscard]] std::vector<std::string_view> languages();

// A language's own name ("Deutsch"); the code when its catalog has none.
[[nodiscard]] std::string_view language_name(std::string_view code);

// A language's translated texts (none for English or an unknown code), for tests and tools.
[[nodiscard]] std::vector<Entry> entries(std::string_view code);

// Mark English stored in static tables for catalog extraction. This is not a lookup:
// translate with tr() when displaying it, after the session language has been selected.
[[nodiscard]] constexpr std::string_view mark(std::string_view english) { return english; }

// english in the interface's language.
[[nodiscard]] std::string_view tr(std::string_view english);

// Context is a stable meaning ("file-action"), never user data. A missing context entry
// falls back to English, not a possibly unrelated context-free translation.
[[nodiscard]] std::string_view trc(std::string_view context, std::string_view english);

// The catalog's integer cardinal category. Useful when checking a translation.
[[nodiscard]] std::string_view plural_category(std::uint64_t count);

namespace detail {
[[nodiscard]] std::string_view plural_text(std::string_view context, std::string_view singular,
                                           std::string_view plural, std::uint64_t count);

inline std::string translated_format(std::string_view english, std::string_view translated,
                                     std::format_args args) {
    try {
        return std::vformat(translated, args);
    } catch (const std::format_error&) {
        return std::vformat(english, args);
    }
}
}  // namespace detail

template <class... Args>
[[nodiscard]] std::string trcf(std::string_view context, std::string_view english,
                               const Args&... args) {
    return detail::translated_format(english, trc(context, english),
                                     std::make_format_args(args...));
}

// Whole, nonnegative counts. Count is always argument {0}; additional arguments start at {1}.
// Catalogs key plural messages by their English plural, with zero/one/two/few/many/other forms.
template <class... Args>
[[nodiscard]] std::string trnc(std::string_view context, std::string_view singular,
                               std::string_view plural, std::uint64_t count, const Args&... args) {
    return detail::translated_format(count == 1 ? singular : plural,
                                     detail::plural_text(context, singular, plural, count),
                                     std::make_format_args(count, args...));
}

template <class... Args>
[[nodiscard]] std::string trn(std::string_view singular, std::string_view plural,
                              std::uint64_t count, const Args&... args) {
    return trnc({}, singular, plural, count, args...);
}

// std::format with a translated format string. A translation may reorder its arguments with
// numbered fields ("{1} von {0}"); a translation whose fields do not fit falls back to English.
template <class... Args>
[[nodiscard]] std::string trf(std::string_view english, const Args&... args) {
    const std::string_view text = tr(english);
    if (text.data() != english.data()) {
        try {
            return std::vformat(text, std::make_format_args(args...));
        } catch (const std::format_error&) {
            // The catalog's fault: English is still right.
        }
    }
    return std::vformat(english, std::make_format_args(args...));
}

// Key names in a shortcut the way the language writes them on keyboards ("Ctrl+Shift+Z" ->
// "Strg+Umschalt+Z"); the key names are words between "+", spaces and commas.
[[nodiscard]] std::string keys(std::string_view text);

// A number written with a decimal point ("1.5 MB") the way the language writes it ("1,5 MB").
[[nodiscard]] std::string decimal(std::string text);

}  // namespace rummage::i18n
