#include <algorithm>

#include "draw_helpers.hpp"
#include "rummage/fs/trash.hpp"
#include "rummage/views/browser.hpp"

namespace rummage::views {

ItemBounds Browser::motion_item_bounds() const {
    if (pane().insights.is_open()) {
        ItemBounds result;
        result.cursor = result.selection = pane().insights.selected_bounds();
        return result;
    }
    switch (pane().mode) {
        case Mode::columns:
            return pane().columns.motion_bounds();
        case Mode::grid:
            return pane().grid.motion_bounds();
        case Mode::details:
            return pane().details.motion_bounds();
    }
    return {};
}

void Browser::start_motion(core::MotionEvent event) {
    const ui::MotionChannel channel = event == core::MotionEvent::palette_open ||
                                              event == core::MotionEvent::search_results ||
                                              event == core::MotionEvent::quick_look
                                          ? ui::MotionChannel::overlay
                                          : ui::pane_motion_channel(context_area_);
    if (motion_.start(channel, event, Clock::now()))
        redraw_();
}

void Browser::observe_motion(Clock::time_point now) {
    auto& observed = area();
    if (pane().insights.is_open()) {
        motion_.cancel(ui::pane_motion_channel(context_area_));
        observed.motion_pending = false;
        observed.motion_tab = nullptr;
        return;
    }
    const auto folder = current_folder();
    if (!folder) {
        motion_.cancel(ui::pane_motion_channel(context_area_));
        return;
    }
    // A switched/closed tab has a new identity. Never replay another tab's pending reveal.
    if (observed.motion_tab != &pane()) {
        motion_.cancel(ui::pane_motion_channel(context_area_));
        observed.motion_tab = &pane();
        observed.motion_folder = folder->path();
        observed.motion_pending = false;
        observed.motion_mode = pane().mode;
        observed.motion_selection = folder->selection_revision();
        observed.motion_cursor = folder->cursor_entry() ? folder->cursor_entry()->name : "";
        observed.motion_back = false;
        return;
    }
    if (observed.motion_folder != folder->path()) {
        cancel_operation_motion();
        // Keep the previously painted cursor: the new folder has no layout yet.
        if (observed.motion_origin.empty())
            observed.motion_origin = {observed.view_area.x + observed.view_area.width / 2,
                                      observed.view_area.y + observed.view_area.height / 2, 0, 0};
        observed.motion_folder = folder->path();
        observed.motion_pending = true;
        motion_.cancel(ui::pane_motion_channel(context_area_));
    }
    if (observed.motion_pending && !folder->model().loading()) {
        observed.motion_pending = false;
        if (!folder->model().error())
            motion_.start(ui::pane_motion_channel(context_area_),
                          observed.motion_back ? core::MotionEvent::directory_back
                                               : core::MotionEvent::directory_open,
                          now);
        else
            motion_.start(ui::pane_motion_channel(context_area_),
                          core::MotionEvent::invalid_operation, now);
        observed.motion_back = false;
    } else if (!observed.motion_pending && observed.motion_mode != pane().mode) {
        motion_.start(ui::pane_motion_channel(context_area_), core::MotionEvent::view_change, now);
    }
    observed.motion_mode = pane().mode;
    const std::string cursor = folder->cursor_entry() ? folder->cursor_entry()->name : "";
    if (!observed.motion_pending && !motion_.frame(ui::pane_motion_channel(context_area_), now)) {
        if (folder->selected_count() > 1 &&
            observed.motion_selection != folder->selection_revision())
            motion_.start(ui::pane_motion_channel(context_area_),
                          core::MotionEvent::selection_change, now);
        else if (cursor != observed.motion_cursor && !cursor.empty())
            motion_.start(ui::pane_motion_channel(context_area_), core::MotionEvent::focus_change,
                          now);
    }
    observed.motion_cursor = cursor;
    observed.motion_selection = folder->selection_revision();
}

void Browser::draw_motion_sprite(ui::Renderer& renderer, const theme::Palette& palette,
                                 ui::IconSet& icons, Clock::time_point now) {
    const auto frame = motion_.frame(ui::MotionChannel::operation, now);
    if (!frame)
        return;
    const auto rect = frame->geometry();
    if (rect.empty())
        return;
    renderer.push_clip(motion_bounds_);
    const auto& source = motion_sprite_.bounds;
    const auto& clip = motion_sprite_.clip;
    const float sx = rect.width / source.width, sy = rect.height / source.height;
    // Capture the padded backdrop before clipping the retained item. Only the
    // foreground uses the original item clip; emitted light can extend around it.
    const bool captured = renderer.begin_motion(rect.inset(-std::max(rect.width, rect.height)),
                                                motion_.config().quality, true);
    renderer.push_clip({rect.x + (clip.x - source.x) * sx, rect.y + (clip.y - source.y) * sy,
                        clip.width * sx, clip.height * sy});
    draw_item_visual(renderer, icons, palette, motion_sprite_, rect);
    renderer.pop_clip();
    if (captured)
        renderer.end_motion(*frame, rect.x + rect.width / 2, rect.y + rect.height / 2,
                            palette.primary);
    renderer.pop_clip();
}

Browser::OperationMotion Browser::capture_operation_motion(
    const fs::TransferRequest& request) const {
    OperationMotion source{.area = context_area_, .epoch = motion_epoch_, .visual = {}};
    if (const auto folder = current_folder(); folder && folder->path() == request.source) {
        if (const auto* entry = folder->cursor_entry();
            entry && std::ranges::find(request.names, entry->name) != request.names.end()) {
            const auto painted = motion_item_bounds().visual;
            if (painted.path == request.source / entry->name && !painted.clip.empty())
                source.visual = painted;
        }
    }
    return source;
}

void Browser::complete_operation_motion(const OperationMotion& source,
                                        const fs::TransferRequest& request,
                                        const fs::TransferResult& result) {
    // Effects acknowledge confirmed items only. A skip, partial failure or cancellation
    // never gets a success animation. Ignore stale geometry after navigation/tab changes.
    if (!result.cancelled && result.errors.empty() && result.refused.empty() &&
        !source.visual.bounds.empty() && source.area == active_area_ &&
        source.epoch == motion_epoch_ && current_folder() &&
        current_folder()->path() == request.source &&
        std::ranges::any_of(result.items,
                            [&](const auto& item) { return item.from == source.visual.path; })) {
        std::optional<core::MotionEvent> event;
        if (request.kind == fs::TransferKind::copy)
            event = core::MotionEvent::file_copy;
        else if (request.kind == fs::TransferKind::move)
            event = core::MotionEvent::file_move;
        else if (request.kind == fs::TransferKind::trash)
            event = core::MotionEvent::file_trash;
        if (event) {
            // An off-screen destination uses a small in-window receipt by the status bar.
            ui::Rect target{split_bounds_.right() - 40, split_bounds_.bottom() - 36, 24, 24};
            if (request.kind == fs::TransferKind::trash) {
                if (const auto trash = sidebar_.place_icon_rect(fs::trash_path()))
                    target = *trash;
            } else if (const auto other = other_folder();
                       other && other->path() == request.destination) {
                const auto& destination = areas_[1 - active_area_]->view_area;
                target = {destination.x + destination.width / 2 - 16,
                          destination.y + destination.height / 2 - 16, 32, 32};
            }
            if (motion_.start(ui::MotionChannel::operation, *event, Clock::now(),
                              source.visual.bounds, target))
                motion_sprite_ = source.visual;
            redraw_();
        }
    }
}

void Browser::begin_motion_frame(ui::Renderer& renderer, const ui::Rect& bounds,
                                 Clock::time_point now) {
    const bool running = effects_active_ && motion_.ambient_enabled();
    if (ambient_tick_ != Clock::time_point{} && ambient_drawn_ && running)
        ambient_seconds_ +=
            std::max(0.0, std::chrono::duration<double>(now - ambient_tick_).count()) *
            static_cast<double>(motion_.config().ambient.speed);
    ambient_tick_ = now;
    ambient_drawn_ = false;
    if (!motion_.ambient_enabled())
        renderer.release_ambient();
    if (!settings_.previewing())
        renderer.release_ambient(true);
    motion_.advance(now);
    if (!motion_.frame(ui::MotionChannel::operation, now))
        motion_sprite_ = {};
    if (motion_bounds_.width != bounds.width || motion_bounds_.height != bounds.height) {
        cancel_layout_motion();
    }
    motion_bounds_ = bounds;
    if (!motion_.config().enabled || motion_.config().duration_scale == 0 || motion_.reduced())
        renderer.release_motion();
    if (!split_visible_)
        motion_.cancel(ui::pane_motion_channel(1 - active_area_));
}

void Browser::layout_motion_area(const ui::Rect& bounds) {
    const auto previous = area().motion_view_bounds;
    if (previous.x != bounds.x || previous.y != bounds.y || previous.width != bounds.width ||
        previous.height != bounds.height) {
        cancel_operation_motion();
        motion_.cancel(ui::pane_motion_channel(context_area_));
    }
    area().motion_view_bounds = bounds;
}

void Browser::draw_view_with_motion(ui::Renderer& renderer, const theme::Palette& palette,
                                    ui::IconSet& icons, Clock::time_point now) {
    observe_motion(now);
    const auto frame = motion_.frame(ui::pane_motion_channel(context_area_), now);
    const bool native = frame && core::native_motion_feedback(frame->effect);
    const bool captured =
        frame && !native && renderer.begin_motion(area().view_area, motion_.config().quality);
    if (frame && !native && !captured)
        motion_.cancel(ui::pane_motion_channel(context_area_));
    if (motion_.ambient_enabled() && motion_.config().ambient.background) {
        ambient_drawn_ =
            renderer.draw_ambient(area().view_area, motion_.config().ambient,
                                  static_cast<float>(ambient_seconds_), palette.primary) ||
            ambient_drawn_;
    }
    draw_view(renderer, area().view_area, palette, icons, now);
    const auto items = motion_item_bounds();
    if (!frame || native)
        area().motion_origin = items.cursor;
    if (captured) {
        const auto origin = area().motion_origin;
        renderer.end_motion(*frame, origin.x + origin.width / 2, origin.y + origin.height / 2,
                            palette.primary);
    }
    if (motion_.ambient_enabled() && motion_.config().ambient.selection_glow &&
        !items.selection.empty()) {
        renderer.push_clip(area().view_area);
        ambient_drawn_ =
            renderer.draw_ambient(items.selection, motion_.config().ambient,
                                  static_cast<float>(ambient_seconds_), palette.primary, true) ||
            ambient_drawn_;
        renderer.pop_clip();
    }
    if (native) {
        const auto cursor = items.cursor;
        renderer.push_clip(area().view_area);
        const float opacity = (1.0F - frame->progress) * 0.45F;
        if (frame->effect == core::MotionEffect::focus_glow && !cursor.empty())
            renderer.draw_shadow(cursor, ui::Corners::all(8), 8,
                                 palette.primary.with_alpha(opacity));
        else if (frame->effect == core::MotionEffect::energy_trace) {
            const auto group = items.selection;
            renderer.draw_box(group, {.fill = palette.primary.with_alpha(0),
                                      .radius = ui::Corners::all(5),
                                      .border_width = 2,
                                      .border = palette.primary.with_alpha(opacity)});
            // A short highlight travels clockwise around the selected group's visible bounds.
            const float perimeter = 2 * (group.width + group.height);
            const float distance = perimeter * frame->progress;
            ui::Rect tip{group.x, group.y, 4, 4};
            if (distance < group.width)
                tip.x += distance;
            else if (distance < group.width + group.height) {
                tip.x = group.right() - 4;
                tip.y += distance - group.width;
            } else if (distance < 2 * group.width + group.height) {
                tip.x = group.right() - (distance - group.width - group.height);
                tip.y = group.bottom() - 4;
            } else
                tip.y = group.bottom() - (distance - 2 * group.width - group.height);
            if (!group.empty())
                renderer.fill_rect(tip, palette.primary.with_alpha(1 - frame->progress));
        }
        renderer.pop_clip();
    }
}

std::optional<ui::Motion::Frame> Browser::begin_overlay_motion(ui::Renderer& renderer,
                                                               const ui::Rect& bounds,
                                                               const theme::Palette& palette,
                                                               ui::IconSet& icons,
                                                               Clock::time_point now) {
    const bool look_open = quick_look_.is_open();
    const bool palette_open = palette_.is_open();
    if (look_open && !motion_quick_look_)
        motion_.start(ui::MotionChannel::overlay, core::MotionEvent::quick_look, now);
    else if (palette_open && !motion_palette_)
        motion_.start(ui::MotionChannel::overlay, core::MotionEvent::palette_open, now);
    else if ((!look_open && motion_quick_look_) || (!palette_open && motion_palette_))
        motion_.cancel(ui::MotionChannel::overlay);
    motion_quick_look_ = look_open;
    motion_palette_ = palette_open;
    draw_motion_sprite(renderer, palette, icons, now);
    auto overlay_motion = motion_.frame(ui::MotionChannel::overlay, now);
    const bool capture_overlay =
        overlay_motion && renderer.begin_motion(bounds, motion_.config().quality);
    if (overlay_motion && !capture_overlay) {
        motion_.cancel(ui::MotionChannel::overlay);
        overlay_motion.reset();
    }
    return overlay_motion;
}

void Browser::end_overlay_motion(ui::Renderer& renderer,
                                 const std::optional<ui::Motion::Frame>& frame,
                                 const ui::Rect& bounds, const theme::Palette& palette) {
    if (frame)
        renderer.end_motion(*frame, bounds.x + bounds.width / 2, bounds.y + bounds.height / 2,
                            palette.primary);
    if (!motion_.active() && !settings_.animating())
        renderer.release_motion_textures();
}

void Browser::leave_motion() {
    cancel_operation_motion();
    area().motion_tab = nullptr;
    area().motion_pending = false;
    motion_.cancel(ui::pane_motion_channel(context_area_));
    motion_.cancel(ui::MotionChannel::overlay);
}

void Browser::cancel_operation_motion() {
    ++motion_epoch_;
    motion_.cancel(ui::MotionChannel::operation);
    motion_sprite_ = {};
}

void Browser::cancel_layout_motion() {
    ++motion_epoch_;
    motion_.cancel_all();
    motion_sprite_ = {};
}

std::optional<std::chrono::milliseconds> Browser::ambient_interval() const {
    if (!effects_active_)
        return std::nullopt;
    if (const auto preview = settings_.ambient_interval())
        return preview;
    if (!ambient_drawn_ || !motion_.ambient_enabled() || motion_.config().ambient.speed <= 0)
        return std::nullopt;
    return std::chrono::milliseconds{(1000 + motion_.config().ambient.fps - 1) /
                                     motion_.config().ambient.fps};
}

}  // namespace rummage::views
