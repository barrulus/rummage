#pragma once
#include <cstdint>
#include <optional>
#include <string>

#include "rummage/ui/geometry.hpp"

namespace rummage::ui {
// One atomic text-input-v3 done transaction. Offsets and lengths are UTF-8 bytes.
struct TextInputEdit {
    std::string preedit{};
    std::int32_t begin{};
    std::int32_t end{};
    std::optional<std::string> commit{};
    std::uint32_t before{};
    std::uint32_t after{};
};

struct TextInputState {
    const void* identity{};
    std::string text;
    std::int32_t cursor{};
    std::int32_t anchor{};
    bool password{};
    Rect cursor_rect{};  // surface logical coordinates, not buffer pixels
};
}  // namespace rummage::ui
