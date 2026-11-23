#include <chrono>
#include <fstream>
#include <functional>
#include <string>

#include <doctest/doctest.h>

#include "rummage/platform/code_stats.hpp"
#include "temp_dir.hpp"

namespace {
using namespace rummage::platform;
using namespace std::chrono_literals;
constexpr std::string_view example =
    R"({"Markdown":{"blanks":1,"code":0,"comments":3,"reports":[{"stats":{"blanks":1,"code":0,"comments":3,"blobs":{"Python":{"blanks":0,"code":1,"comments":1,"blobs":{}}}},"name":"./guide.md"}],"children":{"Python":[]},"inaccurate":false},"Rust":{"blanks":1,"code":1,"comments":1,"reports":[{"stats":{"blanks":1,"code":1,"comments":1,"blobs":{}},"name":"./sub/main.rs"}],"children":{},"inaccurate":false},"Total":{"blanks":2,"code":2,"comments":5,"reports":[],"children":{},"inaccurate":false}})";

std::string replace(std::string text, std::string_view from, std::string_view to) {
    const auto at = text.find(from);
    REQUIRE(at != std::string::npos);
    text.replace(at, from.size(), to);
    return text;
}

}  // namespace

TEST_CASE("source stats: embedded languages are additive without duplicate physical files") {
    const auto report = parse_code_report(example, "/work");
    REQUIRE(report.error == CodeError::none);
    CHECK(report.files == 2);
    CHECK(report.total == CodeCounts{2, 5, 2});
    CHECK(report.total.lines() == 9);
    REQUIRE(report.languages.size() == 3);
    CHECK(report.languages[0].name == "Markdown");
    CHECK(report.languages[0].counts.lines() == 4);
    CHECK(report.languages[1].name == "Python");
    CHECK(report.languages[1].depth == 1);
    CHECK(report.languages[1].counts.lines() == 2);
    REQUIRE(report.languages[1].files.size() == 1);
    CHECK(report.languages[1].files[0].relative == "guide.md");
    CHECK(report.languages[2].depth == 0);
    CHECK(parse_code_report(std::string{example} + "\n", "/work").error == CodeError::none);
    CHECK(parse_code_report(replace(std::string{example}, "./guide.md", "/work/guide.md"), "/work")
              .error == CodeError::none);
    CHECK(parse_code_report(replace(std::string{example}, "false", "true"), "/work").inaccurate);
}

TEST_CASE("source stats: malformed, escaping, inconsistent and excessive reports fail closed") {
    for (const auto* path :
         {"../guide.md", "/outside/guide.md", "./sub/../../guide.md", ".", "guide\\u0000.md", ""}) {
        INFO(path);
        const auto report =
            parse_code_report(replace(std::string{example}, "./guide.md", path), "/work");
        CHECK(report.error == CodeError::invalid_output);
        CHECK(report.files == 0);
        CHECK(report.languages.empty());
    }
    for (const auto& invalid :
         {std::string{}, std::string{"{}"}, std::string{example.substr(0, 80)},
          std::string{example} + "{}", replace(std::string{example}, "./sub/main.rs", "./guide.md"),
          replace(std::string{example}, "\"blanks\":1", "\"blanks\":-1"),
          replace(std::string{example}, "\"blanks\":1", "\"blanks\":1.5"),
          replace(std::string{example}, "\"blanks\":1", "\"blanks\":18446744073709551615"),
          replace(std::string{example}, "\"blanks\":2", "\"blanks\":0"),
          replace(std::string{example}, "./guide.md", std::string(1, static_cast<char>(0xFF))),
          std::string(60, '[') + std::string(60, ']')}) {
        CHECK(parse_code_report(invalid, "/work").error == CodeError::invalid_output);
    }
    CHECK(parse_code_report(std::string(16 * 1024 * 1024 + 1, ' '), "/work").error ==
          CodeError::output_limit);
    rummage::core::CancelToken token;
    token.cancel();
    CHECK(parse_code_report(example, "/work", token).error == CodeError::stopped);
}
