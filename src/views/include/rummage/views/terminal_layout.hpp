#pragma once

#include <cstdint>

namespace rummage::views {
enum class TerminalPlacement : std::uint8_t { bottom, top, left, right, floating };

// Window geometry is remembered; opening a window never starts a shell implicitly.
struct TerminalLayout {
    TerminalPlacement placement{TerminalPlacement::bottom};
    float width{480}, height{260}, x{40}, y{40};
    bool operator==(const TerminalLayout&) const = default;
};
}  // namespace rummage::views
