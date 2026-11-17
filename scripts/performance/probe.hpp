// Benchmark-only instrumentation, included by instrument.py in a temporary main.cpp.
// Each line uses CLOCK_MONOTONIC. Frame callbacks are compositor pacing signals,
// not proof of physical presentation. Never include this header in shipped builds.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace performance_probe {
inline FILE* output = [] {
    const char* path = std::getenv("PERFORMANCE_TRACE");
    return path ? std::fopen(path, "w") : nullptr;
}();
inline unsigned sequence = 0;
inline unsigned drawn_sequence = 0;
inline bool busy = true;
inline bool moving = true;
inline std::string selected;

inline long long now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline void key(unsigned symbol) {
    if (!output)
        return;
    std::fprintf(output, "key %lld %u %u\n", now(), ++sequence, symbol);
    std::fflush(output);
}

template <class Browser>
void draw(Browser& browser, int width, int height) {
    if (!output)
        return;
    const auto folder = browser.columns().focused_folder();
    const auto* entry = folder ? folder->cursor_entry() : nullptr;
    selected = entry ? entry->name : "-";
    drawn_sequence = sequence;
    busy = browser.loading();
    moving = browser.animating();
    std::fprintf(output, "draw %lld %u %d %d %s %d %d\n", now(), drawn_sequence, busy, moving,
                 selected.c_str(), width, height);
    std::fflush(output);
}

inline void frame() {
    if (!output)
        return;
    std::fprintf(output, "frame %lld %u %d %d %s\n", now(), drawn_sequence, busy, moving,
                 selected.c_str());
    std::fflush(output);
}
}  // namespace performance_probe
