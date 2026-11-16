#include "rummage/core/i18n.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <string_view>

#include <doctest/doctest.h>

#include "rummage/views/format.hpp"

namespace i18n = rummage::i18n;

namespace {

// Restores English, as every other test expects, when a test that switched language ends.
struct Speaking {
    explicit Speaking(std::string_view code) { i18n::set_language(code); }

    ~Speaking() { i18n::set_language(i18n::kEnglish); }

    Speaking(const Speaking&) = delete;
    Speaking& operator=(const Speaking&) = delete;
    Speaking(Speaking&&) = delete;
    Speaking& operator=(Speaking&&) = delete;
};

// The fields of a format string, "{}" and "{0}" alike, by number; automatic fields are numbered
// in order. A named one ("{file}", a placeholder for config.toml, not std::format) counts by name.
std::multiset<std::string> fields(std::string_view text) {
    std::multiset<std::string> out;
    int next = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '{' && i + 1 < text.size() && text[i + 1] == '{') {
            ++i;
            continue;
        }
        if (text[i] != '{') {
            continue;
        }
        const auto close = text.find('}', i);
        REQUIRE(close != std::string_view::npos);
        const std::string_view inside = text.substr(i + 1, close - i - 1);
        const std::string_view index = inside.substr(0, inside.find(':'));
        out.insert(index.empty() ? std::to_string(next++) : std::string{index});
        i = close;
    }
    return out;
}

}  // namespace

TEST_CASE("i18n: the language of a locale") {
    CHECK(i18n::language_of("de_DE.UTF-8") == "de");
    CHECK(i18n::language_of("de_AT@euro") == "de");
    CHECK(i18n::language_of("de_AT.UTF-8@euro") == "de");
    CHECK(i18n::language_of("de") == "de");
    CHECK(i18n::language_of("en_US.UTF-8") == "en");
    CHECK(i18n::language_of("C") == "en");
    CHECK(i18n::language_of("") == "en");
    CHECK(i18n::language_of("xx_YY.UTF-8") == "en");  // no catalog
    CHECK(i18n::language_of("xx:de:en") == "de");     // the first with a catalog
    CHECK(i18n::language_of("en:de") == "en");
    CHECK(i18n::language_of("de_XX") == "de");  // the language without the region
}

TEST_CASE("i18n: the compiled-in languages") {
    const auto codes = i18n::languages();
    CHECK(std::ranges::find(codes, i18n::kEnglish) != codes.end());
    CHECK(std::ranges::find(codes, "de") != codes.end());
    CHECK(std::ranges::is_sorted(codes));
    CHECK(i18n::language_name("en") == "English");
    CHECK(i18n::language_name("de") == "Deutsch");
    CHECK(i18n::entries("en").empty());
    CHECK(i18n::entries("xx").empty());
    const Speaking unknown{"xx"};
    CHECK(i18n::language() == i18n::kEnglish);
}

// Holds for every data/i18n/*.toml: a new language is checked without touching this test.
TEST_CASE("i18n: every catalog parses, names itself and keeps the fields of its texts") {
    for (const std::string_view code : i18n::languages()) {
        if (code == i18n::kEnglish) {
            continue;
        }
        CAPTURE(code);
        CHECK(i18n::language_name(code) != code);  // [language] name, so the file parsed
        const auto entries = i18n::entries(code);
        CHECK_FALSE(entries.empty());
        std::set<std::string_view> seen;
        for (const i18n::Entry& entry : entries) {
            CAPTURE(entry.english);
            CHECK(seen.insert(entry.english).second);
            CHECK_FALSE(entry.translated.empty());
            CHECK(fields(entry.english) == fields(entry.translated));
        }
    }
}

TEST_CASE("i18n: English stays English") {
    CHECK(i18n::tr("Empty folder") == "Empty folder");
    CHECK(i18n::trf("{} of {} selected", 2, 5) == "2 of 5 selected");
    CHECK(i18n::keys("Ctrl+Shift+Z") == "Ctrl+Shift+Z");
    CHECK(rummage::views::format_size(1234) == "1.2 kB");
}

TEST_CASE("i18n: German") {
    const Speaking german{"de"};
    CHECK(i18n::language() == "de");
    CHECK(i18n::tr("Empty folder") == "Leerer Ordner");
    CHECK(i18n::tr("not in any table") == "not in any table");
    CHECK(i18n::trf("{} of {} selected", 2, 5) == "2 von 5 ausgewählt");
    CHECK(i18n::trf("Delete “{}” permanently?", "a.txt") == "„a.txt“ endgültig löschen?");
    CHECK(i18n::keys("Ctrl+Shift+Z") == "Strg+Umschalt+Z");
    CHECK(i18n::keys("Ctrl++  Ctrl+−") == "Strg++  Strg+−");
    CHECK(i18n::keys("Alt+Left") == "Alt+←");
    CHECK(i18n::keys("Space  Escape") == "Leertaste  Esc");
    CHECK(i18n::decimal("1.2 kB · 3.4 MB") == "1,2 kB · 3,4 MB");
    CHECK(i18n::decimal("v1.") == "v1.");
    CHECK(rummage::views::format_size(1) == "1 Byte");
    CHECK(rummage::views::format_size(999) == "999 Bytes");
    CHECK(rummage::views::format_size(18'100'000) == "18,1 MB");
}
