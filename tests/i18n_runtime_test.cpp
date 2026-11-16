#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <array>
#include <limits>
#include <string>
#include <string_view>

#include <doctest/doctest.h>

#include "i18n_catalogs.hpp"
#include "i18n_rules.hpp"
#include "rummage/core/i18n.hpp"

// Test-only catalogs: exercise complete runtime selection without shipping unfinished languages.
namespace rummage::i18n::detail {
std::span<const Catalog> catalogs() {
    static constexpr std::array kCatalogs{Catalog{"pl", R"(
[language]
name = "Polski"
[language.plural_rules]
one = "n = 1"
few = "n % 10 = 2..4 and n % 100 != 12..14"
many = "n != 1 and n % 10 = 0..1 or n % 10 = 5..9 or n % 100 = 12..14"
[texts]
"{} files" = { one = "{} plik", few = "{} pliki", many = "{} plików", other = "{} pliku" }
"Open" = "generic"
[contexts.action]
"Open" = "Otwórz"
"{} files in {}" = { one = "{1}: {0} plik", few = "{1}: {0} pliki", many = "{1}: {0} plików", other = "{1}: {0} pliku" }
[contexts.state]
"Open" = "Otwarty"
)"},
                                          Catalog{"ru", R"(
[language]
name = "Russian fixture"
[language.plural_rules]
one = "n % 10 = 1 and n % 100 != 11"
few = "n % 10 = 2..4 and n % 100 != 12..14"
many = "n % 10 = 0 or n % 10 = 5..9 or n % 100 = 11..14"
[texts]
"{} files" = {one = "one {}", few = "few {}", many = "many {}", other = "other {}"}
)"},
                                          Catalog{"ar", R"(
[language]
name = "Arabic fixture"
[language.plural_rules]
zero = "n = 0"
one = "n = 1"
two = "n = 2"
few = "n % 100 = 3..10"
many = "n % 100 = 11..99"
[texts]
"{} files" = {zero = "zero {}", one = "one {}", two = "two {}", few = "few {}", many = "many {}", other = "other {}"}
)"},
                                          Catalog{"ja", R"(
[language]
name = "Japanese fixture"
[language.plural_rules]
[texts]
"{} files" = {other = "{} files-ja"}
)"},
                                          Catalog{"fr", R"(
[language]
name = "French fixture"
[language.plural_rules]
one = "n = 0..1"
many = "n != 0 and n % 1000000 = 0"
[texts]
"{} files" = {one = "one {}", many = "many {}", other = "other {}"}
)"},
                                          Catalog{"xx", R"(
[language]
name = "Invalid rule"
[language.plural_rules]
one = "n % 0 = 1"
)"},
                                          Catalog{"yy", R"(
[language]
name = "Missing other"
[texts]
"{} files" = {one = "{} file"}
)"},
                                          Catalog{"zz", R"(
[language]
name = "Bad format"
[texts]
"{} files" = {one = "{7} file", other = "{7} files"}
)"}};
    return kCatalogs;
}
}  // namespace rummage::i18n::detail

namespace i18n = rummage::i18n;

TEST_CASE("integer plurals: Polish and Russian distinguish teens and decades") {
    i18n::set_language("pl");
    CHECK(i18n::trn("{} file", "{} files", 1) == "1 plik");
    CHECK(i18n::trn("{} file", "{} files", 2) == "2 pliki");
    CHECK(i18n::trn("{} file", "{} files", 5) == "5 plików");
    for (auto n : {0U, 11U, 12U, 14U, 21U, 101U, 112U})
        CHECK(i18n::plural_category(n) == "many");
    for (auto n : {2U, 4U, 22U, 24U, 102U})
        CHECK(i18n::plural_category(n) == "few");
    i18n::set_language("ru");
    for (auto n : {1U, 21U, 101U})
        CHECK(i18n::plural_category(n) == "one");
    for (auto n : {2U, 4U, 22U})
        CHECK(i18n::plural_category(n) == "few");
    for (auto n : {0U, 5U, 11U, 12U, 14U, 111U})
        CHECK(i18n::plural_category(n) == "many");
    CHECK(i18n::plural_category(std::numeric_limits<std::uint64_t>::max()) == "many");
}

TEST_CASE("integer plurals: six Arabic categories and single-form languages") {
    i18n::set_language("ar");
    for (const auto& [n, form] :
         std::array<std::pair<unsigned, const char*>, 10>{{{0U, "zero"},
                                                           {1U, "one"},
                                                           {2U, "two"},
                                                           {3U, "few"},
                                                           {10U, "few"},
                                                           {11U, "many"},
                                                           {99U, "many"},
                                                           {100U, "other"},
                                                           {102U, "other"},
                                                           {103U, "few"}}}) {
        CHECK(i18n::plural_category(n) == form);
        CHECK(i18n::trn("{} file", "{} files", n) == std::string{form} + " " + std::to_string(n));
    }
    i18n::set_language("ja");
    for (auto n : {0U, 1U, 2U, 200U})
        CHECK(i18n::trn("{} file", "{} files", n) == std::to_string(n) + " files-ja");
    i18n::set_language("fr");
    CHECK(i18n::plural_category(0) == "one");
    CHECK(i18n::plural_category(1) == "one");
    CHECK(i18n::plural_category(2) == "other");
    CHECK(i18n::plural_category(1'000'000) == "many");
}

TEST_CASE("contexts: separate meanings, reordered fields and English fallbacks") {
    i18n::set_language("pl");
    CHECK(i18n::tr("Open") == "generic");
    CHECK(i18n::trc("action", "Open") == "Otwórz");
    CHECK(i18n::trc("state", "Open") == "Otwarty");
    CHECK(i18n::trc("unknown", "Open") == "Open");
    CHECK(i18n::trnc("action", "{} file in {}", "{} files in {}", 22, "photos") ==
          "photos: 22 pliki");
    CHECK(i18n::trnc("unknown", "{} file", "{} files", 1) == "1 file");
    CHECK(i18n::trnc("unknown", "{} file", "{} files", 2) == "2 files");
    CHECK(i18n::trn("{} unknown file", "{} unknown files", 1) == "1 unknown file");
    i18n::set_language("xx");
    CHECK(i18n::language() == "en");
    i18n::set_language("yy");
    CHECK(i18n::language() == "en");
    i18n::set_language("zz");
    CHECK(i18n::trn("{} file", "{} files", 1) == "1 file");
    CHECK(i18n::trn("{} file", "{} files", 5) == "5 files");
}

TEST_CASE("plural rule parser: bounded grammar and precedence") {
    using i18n::detail::parse_rule;
    for (const auto* text :
         {"", "n % 0 = 1", "n = -1", "n = 4..2", "n = 1 or", "n = 1 garbage", "n / 2 = 1",
          "n = 1.5", "n = 18446744073709551616", "n = 1,,2", "n == 1", "n = ", "n = 1 and n"})
        CHECK_FALSE(parse_rule(text));
    CHECK_FALSE(parse_rule(std::string(1025, ' ')));
    std::string too_many_relations = "n = 1";
    std::string too_many_ranges = "n = 1";
    for (int i = 0; i < 32; ++i) {
        too_many_relations += " or n = 1";
        too_many_ranges += ",1";
    }
    CHECK_FALSE(parse_rule(too_many_relations));
    CHECK_FALSE(parse_rule(too_many_ranges));
    const auto rule = parse_rule("n = 0 or n % 10 = 2,4 and n % 100 != 12..14");
    REQUIRE(rule);
    for (auto n : {0U, 2U, 4U, 22U})
        CHECK(rule->matches(n));
    for (auto n : {1U, 12U, 14U, 112U})
        CHECK_FALSE(rule->matches(n));
}
