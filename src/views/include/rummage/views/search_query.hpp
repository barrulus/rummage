#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rummage::views {

// Search syntax is shared by live searches and saved queries. Dates are UTC day boundaries.
struct SearchQuery {
    bool content{false}, hidden{false}, fold_case{false};
    std::string text, type, extension, error, tag, comment;
    std::optional<unsigned> rating;
    std::optional<std::int64_t> after, before;
    std::vector<std::string> excluded;

    [[nodiscard]] bool metadata() const {
        return content || !type.empty() || !extension.empty() || after || before || !tag.empty() ||
               !comment.empty() || rating;
    }
};

[[nodiscard]] SearchQuery parse_search_query(std::string_view query, bool hidden = false);

struct SearchStats {
    std::size_t matches{0}, unreadable{0}, binary{0}, oversized{0}, changed{0};
    bool byte_limit{false};
};

struct SearchBudget {
    static constexpr std::uint64_t kFileBytes = 8 * 1024 * 1024;
    static constexpr std::uint64_t kTotalBytes = 512 * 1024 * 1024;
    std::atomic<std::uint64_t> bytes{0};
};

}  // namespace rummage::views
