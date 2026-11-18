#include "rummage/views/format.hpp"

#include <doctest/doctest.h>

#include "rummage/views/media_details.hpp"

using rummage::views::format_size;
using rummage::views::visible_rows;

TEST_CASE("format_size: SI units, one decimal") {
    CHECK(format_size(0) == "0 bytes");
    CHECK(format_size(1) == "1 byte");
    CHECK(format_size(999) == "999 bytes");
    CHECK(format_size(1000) == "1.0 kB");
    CHECK(format_size(1234) == "1.2 kB");
    CHECK(format_size(18'100'000) == "18.1 MB");
    CHECK(format_size(999'950) == "1.0 MB");  // not "1000.0 kB"
    CHECK(format_size(1'300'000'000) == "1.3 GB");
}

TEST_CASE("visible_rows: only what intersects the viewport") {
    CHECK(visible_rows(0, 28.0F, 0.0F, 500.0F).size() == 0);
    const auto top = visible_rows(10000, 28.0F, 0.0F, 280.0F);
    CHECK(top.first == 0);
    CHECK(top.last == 10);
    const auto partial = visible_rows(10000, 28.0F, 14.0F, 280.0F);  // half rows at both ends
    CHECK(partial.first == 0);
    CHECK(partial.last == 11);
    const auto end = visible_rows(100, 28.0F, 2800.0F - 280.0F, 280.0F);
    CHECK(end.first == 90);
    CHECK(end.last == 100);
    const auto beyond = visible_rows(5, 28.0F, 0.0F, 1000.0F);  // short folder
    CHECK(beyond.last == 5);
}

TEST_CASE("media details: reduced aspect ratio, arbitrary tags and truncation are visible") {
    rummage::picture::PhotoInfo info;
    info.width = 2560;
    info.height = 1440;
    info.add_detail("Source tag", "the full value");
    info.details_truncated = true;
    const auto rows = rummage::views::media_details(info);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].first == "Aspect ratio");
    CHECK(rows[0].second == "16:9");
    CHECK(rows[1].first == "Source tag");
    CHECK(rows[1].second == "the full value");
    CHECK(rows[2].first == "Metadata");
}
