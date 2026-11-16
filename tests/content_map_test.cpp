#include "rummage/ui/content_map.hpp"

#include <doctest/doctest.h>

namespace ui = rummage::ui;

TEST_CASE("content map: surface positions past the margin, divided by the zoom, and back") {
    const ui::ContentMap plain{};
    CHECK(plain.to_content(40.0F) == 40.0F);
    CHECK(plain.to_surface(40.0F) == 40.0F);

    const ui::ContentMap map{.offset = 24.0F, .zoom = 1.5F};
    CHECK(map.to_content(24.0F) == 0.0F);
    CHECK(map.to_content(54.0F) == 20.0F);
    CHECK(map.to_surface(20.0F) == 54.0F);
    CHECK(map.to_surface(map.to_content(123.0F)) == 123.0F);
    CHECK(map.length(15.0F) == 10.0F);  // a distance has no offset
}

TEST_CASE("content map: a pointer event's position and touchpad scroll, not its wheel notches") {
    const ui::ContentMap map{.offset = 24.0F, .zoom = 2.0F};
    const ui::PointerEvent event =
        map.to_content(ui::PointerEvent{.kind = ui::PointerEvent::Kind::scroll,
                                        .x = 64.0F,
                                        .y = 44.0F,
                                        .scroll_x = 6.0F,
                                        .scroll_y = -30.0F,
                                        .wheel_x = 0.0F,
                                        .wheel_y = -1.0F,
                                        .scroll_finger = true});
    CHECK(event.kind == ui::PointerEvent::Kind::scroll);
    CHECK(event.x == 20.0F);
    CHECK(event.y == 10.0F);
    CHECK(event.scroll_x == 3.0F);
    CHECK(event.scroll_y == -15.0F);
    CHECK(event.wheel_y == -1.0F);
    CHECK(event.scroll_finger);

    const ui::DropEvent drop =
        map.to_content(ui::DropEvent{.kind = ui::DropEvent::Kind::drop, .x = 26.0F, .y = 124.0F});
    CHECK(drop.kind == ui::DropEvent::Kind::drop);
    CHECK(drop.x == 1.0F);
    CHECK(drop.y == 50.0F);
}
