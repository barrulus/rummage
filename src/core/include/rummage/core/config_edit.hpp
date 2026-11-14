#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace rummage::core {

struct ConfigEdit {
    // TOML path to a table (including e.g. actions[0]), literal key, and TOML value.
    std::string table, key, value;
    bool remove{false};
};

struct ConfigText {
    std::string text, error;
};

// Changes only the requested values, retaining comments and unrelated settings. Invalid
// edits return an error without producing a replacement file.
[[nodiscard]] ConfigText edit_config_text(std::string text, const std::vector<ConfigEdit>& edits,
                                          std::string_view append = {});
[[nodiscard]] std::string quote_config_string(std::string_view text);

}  // namespace rummage::core
