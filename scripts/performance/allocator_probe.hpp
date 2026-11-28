// Benchmark-only glibc accounting and controlled reclamation. Include only in a
// temporary instrumented main and call allocator_probe::poll() on its event loop.
// ALLOCATOR_TRACE names a JSONL output; ALLOCATOR_CONTROL names a private command
// file. Atomically publish "sample TAG\n" or "trim TAG\n" to that file. The probe
// consumes it once. A trim writes before/after samples and the malloc_trim result.
// Tags may contain ASCII letters, numbers, underscores and hyphens only.
#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <unistd.h>

namespace allocator_probe {

inline FILE* output() {
    static FILE* stream = [] {
        const char* path = std::getenv("ALLOCATOR_TRACE");
        FILE* result = path ? std::fopen(path, "w") : nullptr;
        if (result)
            std::setvbuf(result, nullptr, _IONBF, 0);
        return result;
    }();
    return stream;
}

inline void sample(const char* tag, const char* stage, int trimmed = -1) {
    FILE* out = output();
    if (!out)
        return;
    unsigned long pages = 0;
    unsigned long virtual_pages = 0;
    if (FILE* statm = std::fopen("/proc/self/statm", "r")) {
        if (std::fscanf(statm, "%lu %lu", &virtual_pages, &pages) != 2)
            pages = 0;
        std::fclose(statm);
    }
    const auto heap = mallinfo2();
    const auto time = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
    std::fprintf(out,
                 "{\"time_ns\":%lld,\"pid\":%d,\"tag\":\"%s\",\"stage\":\"%s\","
                 "\"rss_bytes\":%lu,\"live_bytes\":%zu,\"free_bytes\":%zu,"
                 "\"arena_bytes\":%zu,\"mmapped_bytes\":%zu,\"trim_result\":%d}\n",
                 static_cast<long long>(time), static_cast<int>(getpid()), tag, stage,
                 pages * static_cast<unsigned long>(sysconf(_SC_PAGESIZE)),
                 heap.uordblks + heap.hblkhd, heap.fordblks, heap.arena, heap.hblkhd, trimmed);
}

inline void poll() {
    if (!output())
        return;
    const char* path = std::getenv("ALLOCATOR_CONTROL");
    if (!path)
        return;
    FILE* control = std::fopen(path, "r");
    if (!control)
        return;
    char action[16]{};
    char tag[80]{};
    const int fields = std::fscanf(control, "%15s %79s", action, tag);
    std::fclose(control);
    if (unlink(path) != 0 || fields != 2 ||
        std::strspn(tag, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") !=
            std::strlen(tag))
        return;
    if (std::strcmp(action, "sample") == 0) {
        sample(tag, "sample");
    } else if (std::strcmp(action, "trim") == 0) {
        sample(tag, "before_trim");
        const int trimmed = malloc_trim(0);
        sample(tag, "after_trim", trimmed);
    }
}

}  // namespace allocator_probe
