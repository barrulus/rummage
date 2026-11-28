#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "rummage/fs/entry.hpp"

namespace rummage::views {

// Null annotation values mean mixed or not loaded, never a request to clear them.
struct PhotoAnnotations {
    std::optional<std::string> description;
    std::optional<int> rating;
    std::optional<std::string> capture_date;
    std::optional<std::vector<std::string>> tags;
};

struct PhotoProperties {
    std::vector<std::int64_t> ids;
    std::filesystem::path path;
    fs::Entry entry;
    bool available{true};
    std::string title;
    std::vector<std::pair<std::string, std::string>> rows;
    PhotoAnnotations annotations;
};

// Only explicitly changed fields are present. Tags are additive/removal operations,
// preserving tags that differ among a bulk selection or have not been loaded.
struct PhotoEdits {
    std::optional<std::string> description;
    std::optional<int> rating;
    std::optional<std::string> capture_date;
    std::vector<std::string> add_tags;
    std::vector<std::string> remove_tags;
};

// The caller captures catalog identities; completion runs on the UI loop, empty on success.
using PhotoSave =
    std::function<void(std::vector<std::int64_t>, PhotoEdits, std::function<void(std::string)>)>;

}  // namespace rummage::views
