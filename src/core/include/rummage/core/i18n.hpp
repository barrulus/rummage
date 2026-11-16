#pragma once

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
};

// The code of the language the interface speaks ("de", "pt_BR", or "en").
[[nodiscard]] std::string_view language();

// Overrides the environment's choice (tests, a setting). A code without a catalog is English.
void set_language(std::string_view code);

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

// english in the interface's language.
[[nodiscard]] std::string_view tr(std::string_view english);

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
