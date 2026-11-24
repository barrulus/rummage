#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

namespace rummage::ui {

// Axis-aligned rectangle in logical pixels, origin at the top-left, y pointing down.
struct Rect {
    float x{0.0F};
    float y{0.0F};
    float width{0.0F};
    float height{0.0F};

    [[nodiscard]] constexpr float right() const noexcept { return x + width; }

    [[nodiscard]] constexpr float bottom() const noexcept { return y + height; }

    [[nodiscard]] constexpr bool empty() const noexcept { return width <= 0.0F || height <= 0.0F; }

    [[nodiscard]] constexpr Rect united(const Rect& other) const noexcept {
        if (empty())
            return other;
        if (other.empty())
            return *this;
        const float left = std::min(x, other.x), top = std::min(y, other.y);
        return {left, top, std::max(right(), other.right()) - left,
                std::max(bottom(), other.bottom()) - top};
    }

    // The point is inside: left and top edges included, right and bottom left out.
    [[nodiscard]] constexpr bool contains(float px, float py) const noexcept {
        return px >= x && px < right() && py >= y && py < bottom();
    }

    [[nodiscard]] constexpr Rect intersect(const Rect& other) const noexcept {
        const float left = std::max(x, other.x);
        const float top = std::max(y, other.y);
        const float r = std::min(right(), other.right());
        const float b = std::min(bottom(), other.bottom());
        return {left, top, std::max(0.0F, r - left), std::max(0.0F, b - top)};
    }

    // Shrinks (positive amount) or grows (negative) the rectangle on every side.
    [[nodiscard]] constexpr Rect inset(float amount) const noexcept {
        return {x + amount, y + amount, width - (2.0F * amount), height - (2.0F * amount)};
    }
};

// Clockwise annular sector in logical pixels, angles in radians from the right.
// The hit shape matches Renderer::draw_sector, including the hole and wrap at 2*pi.
struct AnnularSector {
    float x{0}, y{0}, inner{0}, outer{0}, start{0}, sweep{0};

    [[nodiscard]] bool contains(float px, float py) const noexcept {
        const float dx = px - x, dy = py - y;
        const float radius = std::hypot(dx, dy);
        if (radius < inner || radius >= outer || sweep <= 0)
            return false;
        constexpr float turn = 2 * std::numbers::pi_v<float>;
        float angle = std::fmod(std::atan2(dy, dx) - start, turn);
        if (angle < 0)
            angle += turn;
        return angle < sweep;
    }
};

// Corner radii in logical pixels.
struct Corners {
    float top_left{0.0F};
    float top_right{0.0F};
    float bottom_right{0.0F};
    float bottom_left{0.0F};

    [[nodiscard]] static constexpr Corners all(float radius) noexcept {
        return {radius, radius, radius, radius};
    }
};

}  // namespace rummage::ui
