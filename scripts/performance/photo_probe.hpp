// Benchmark-only live Photo Library traversal. Included only in temporary objects.
#pragma once
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "allocator_probe.hpp"
#include "rummage/ui/seat.hpp"

namespace photo_probe {
inline FILE* output = [] {
    const char* p = std::getenv("PHOTO_TRACE");
    return p ? std::fopen(p, "w") : nullptr;
}();
inline int preview_ready = -1;
inline unsigned sequence = 0;
inline unsigned phase = 0;
inline unsigned tick = 0, sent = 0;
inline bool initialized = false;
inline unsigned refresh_sent = 0, refresh_done = 0;
inline bool refresh_active = false, refresh_trimmed = false;
inline unsigned drawn_sequence = 0, drawn_phase = 0;
inline long long drawn_cursor = -1;
inline int drawn_preview = -1;
inline std::string drawn_identity;

inline long long now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline void preview(bool ready) { preview_ready = ready; }

inline void begin_draw() { preview_ready = -1; }

inline float width() {
    const char* value = std::getenv("PHOTO_WIDTH");
    return value ? static_cast<float>(std::atof(value)) : 1280.0F;
}

template <class Browser>
void draw(Browser& b) {
    if (!output)
        return;
    const auto f = b.grid().folder();
    drawn_sequence = sequence;
    drawn_phase = phase;
    drawn_cursor = f && f->cursor() ? static_cast<long long>(*f->cursor()) : -1LL;
    drawn_preview = preview_ready;
    drawn_identity = f && f->cursor_entry() ? f->cursor_entry()->key() : "-";
    std::fprintf(output, "draw %lld %u %u %lld %d %zu %zu\n", now(), sequence, phase,
                 f && f->cursor() ? static_cast<long long>(*f->cursor()) : -1LL, preview_ready,
                 b.grid().columns(), f ? f->entries().size() : 0);
    std::fflush(output);
}

inline void frame() {
    if (output) {
        std::fprintf(output, "frame %lld %u %u %lld %d %s\n", now(), drawn_sequence, drawn_phase,
                     drawn_cursor, drawn_preview, drawn_identity.c_str());
        std::fflush(output);
    }
}

template <class Loop, class Browser, class Window>
void install(Loop& loop, Browser& b, Window& window) {
    using namespace std::chrono_literals;
    const unsigned steps = std::getenv("PHOTO_STEPS")
                               ? static_cast<unsigned>(std::atoi(std::getenv("PHOTO_STEPS")))
                               : 600;
    const unsigned icon =
        std::getenv("PHOTO_ICON") ? static_cast<unsigned>(std::atoi(std::getenv("PHOTO_ICON"))) : 2;
    const unsigned refresh_passes =
        std::getenv("PHOTO_REFRESH_PASSES")
            ? static_cast<unsigned>(std::atoi(std::getenv("PHOTO_REFRESH_PASSES")))
            : 0;
    b.open_photo_library();
    b.set_mode(Browser::Mode::grid);
    b.set_grid_size(icon);
    b.set_info_visible(true);
    b.set_info_auto_hide(false);
    b.info_pane().set_width(300);
    b.set_sidebar_visible(false);
    loop.add_timer(40ms, 40ms, [&, steps, refresh_passes]() {
        allocator_probe::poll();
        if (!initialized) {
            const auto f = b.grid().folder();
            if (!b.showing_photos() || !f || f->entries().empty() || f->loading())
                return;
            if (std::getenv("PHOTO_PRELOAD_ALL") && f->entries().size() < f->total_count()) {
                f->notify_visible_range(0, f->entries().size());
                return;
            }
            initialized = true;
            allocator_probe::sample("loaded", "sample");
        }
        ++tick;
        if (tick % 25 == 0)
            allocator_probe::sample("periodic", "sample");
        if (refresh_passes) {
            phase = 10;
            if (std::getenv("PHOTO_REFRESH_SCROLL") && refresh_done < refresh_passes) {
                rummage::ui::KeyEvent event;
                event.keysym = sent++ == 0 ? XKB_KEY_Home : XKB_KEY_Down;
                const auto f = b.grid().folder();
                ++sequence;
                const auto dispatched = now();
                b.key(event, Browser::Clock::now());
                std::fprintf(output, "key %lld %u %u %lld %s\n", dispatched, sequence, phase,
                             f && f->cursor() ? static_cast<long long>(*f->cursor()) : -1LL,
                             f && f->cursor_entry() ? f->cursor_entry()->key().c_str() : "-");
                std::fflush(output);
                window.request_redraw();
            }
            if (refresh_active && !b.photo_library()->loading()) {
                refresh_active = false;
                ++refresh_done;
                tick = 0;
                std::fprintf(output, "refresh_done %lld %u %zu\n", now(), refresh_done,
                             b.grid().folder()->entries().size());
                std::fflush(output);
                allocator_probe::sample("refresh_complete", "sample");
            }
            if (!refresh_active && refresh_sent < refresh_passes && tick >= 25 &&
                !b.photo_library()->loading()) {
                ++refresh_sent;
                refresh_active = true;
                tick = 0;
                std::fprintf(output, "refresh_begin %lld %u %zu\n", now(), refresh_sent,
                             b.grid().folder()->entries().size());
                std::fflush(output);
                b.photo_library()->refresh();
                window.request_redraw();
            } else if (refresh_done == refresh_passes && !refresh_trimmed && tick >= 875) {
                for (const auto& line : b.memory_lines())
                    std::fprintf(stderr, "before_trim: %s\n", line.c_str());
                allocator_probe::sample("refresh_idle", "before_trim");
                const int result = malloc_trim(0);
                allocator_probe::sample("refresh_idle", "after_trim", result);
                ::raise(SIGUSR1);
                refresh_trimmed = true;
                tick = 0;
            } else if (refresh_trimmed && tick >= 75) {
                allocator_probe::sample("post_trim_idle", "sample");
                std::fprintf(output, "done %lld\n", now());
                std::fflush(output);
                loop.quit(0);
            }
            return;
        }
        if (phase == 0 && tick >= 125) {
            phase = 1;
            tick = 0;
        }
        if (phase == 1 || phase == 3 || phase == 5) {
            rummage::ui::KeyEvent event;
            event.keysym = sent == 0 ? XKB_KEY_Home : XKB_KEY_Down;
            const auto f = b.grid().folder();
            ++sequence;
            const auto dispatched = now();
            b.key(event, Browser::Clock::now());
            std::fprintf(output, "key %lld %u %u %lld %s\n", dispatched, sequence, phase,
                         f && f->cursor() ? static_cast<long long>(*f->cursor()) : -1LL,
                         f && f->cursor_entry() ? f->cursor_entry()->key().c_str() : "-");
            std::fflush(output);
            window.request_redraw();
            if (++sent > steps) {
                ++phase;
                tick = 0;
                sent = 0;
            }
        } else if ((phase == 2 || phase == 4) && tick >= 250) {
            allocator_probe::sample(phase == 2 ? "pass1_idle" : "pass2_idle", "sample");
            ::raise(SIGUSR1);
            ++phase;
            tick = 0;
        } else if (phase == 6 && tick >= 875) {
            for (const auto& line : b.memory_lines())
                std::fprintf(stderr, "before_trim: %s\n", line.c_str());
            allocator_probe::sample("pass3_idle", "before_trim");
            const int result = malloc_trim(0);
            allocator_probe::sample("pass3_idle", "after_trim", result);
            ::raise(SIGUSR1);
            phase = 7;
            tick = 0;
        } else if (phase == 7 && tick >= 75) {
            allocator_probe::sample("post_trim_idle", "sample");
            std::fprintf(output, "done %lld\n", now());
            std::fflush(output);
            loop.quit(0);
        }
    });
}
}  // namespace photo_probe
