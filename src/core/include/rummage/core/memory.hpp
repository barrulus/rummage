#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Where the process's memory is, as the kernel, glibc and the GPU driver see it: a development
// report (ROADMAP 6.3b), not a feature.
namespace rummage::core {

struct MemoryReport {
    // /proc/self/smaps_rollup, bytes.
    std::uint64_t rss{0};
    std::uint64_t peak_rss{0};   // the most it has ever been (VmHWM, /proc/self/status)
    std::uint64_t anonymous{0};  // heap, thread stacks, anonymous mappings: rummage's own
    std::uint64_t private_clean{0};
    std::uint64_t private_dirty{0};
    std::uint64_t shared{0};  // clean and dirty

    // Resident bytes by mapping (/proc/self/smaps), largest first: "[heap]", "[anon]",
    // "/dev/dri/renderD128", "libraries", "fonts", or a file's path.
    struct Mapping {
        std::string name;
        std::uint64_t rss{0};
        std::uint64_t anonymous{0};
    };

    std::vector<Mapping> mappings;

    // glibc's malloc (mallinfo2), bytes: live allocations and allocator address-space capacity.
    // All 0 under ASan or TSan: their malloc replaces it.
    std::uint64_t heap_arena{0};    // obtained with brk and in arenas
    std::uint64_t heap_mmapped{0};  // large blocks with their own mmap
    std::uint64_t heap_in_use{0};
    // Free allocator capacity, including pages malloc_trim may already have decommitted.
    // This is not a count of resident bytes; read it alongside rss/anonymous.
    std::uint64_t heap_free{0};

    // The GPU driver's view of this process (fdinfo of its DRM file, amdgpu and others),
    // bytes; nullopt when the driver does not say.
    std::optional<std::uint64_t> gpu_vram;
    std::optional<std::uint64_t> gpu_gtt;  // system memory the GPU maps
    std::optional<std::uint64_t> gpu_cpu;
};

// Makes malloc give large blocks back to the system as soon as they are freed: blocks of 16 MB or
// more get mappings of their own. glibc's default threshold grows with the blocks freed (up to
// 32 MB), so the 15-30 MB buffers of decoding photos end up in the worker threads' arenas, which
// it keeps: 430 MB after browsing a folder of ORF files, 21 MB of it in use (ROADMAP 6.3b). Call
// it first thing in main(), before any thread starts.
void give_back_large_blocks();

// Attempts to return unused whole pages in malloc arenas to the system (malloc_trim), after
// letting go of many small blocks at once, such as the paths of finding files.
void give_back_free_memory();

// Reclaim only after a substantial amount of allocator capacity is free. Run on a worker
// after a burst of large allocations settles, not in a draw or input callback. The caller
// must coalesce requests: free capacity can stay high even after pages have been returned.
// Returns true when malloc reports reclaiming memory; live allocations are untouched.
[[nodiscard]] bool give_back_free_memory_if_needed(
    std::uint64_t minimum_free_bytes = std::uint64_t{64} << 20);

// Reads it now (a few files of /proc; a few milliseconds).
[[nodiscard]] MemoryReport read_memory_report();

// The report as lines for the log, sizes in MB.
[[nodiscard]] std::vector<std::string> describe(const MemoryReport& report);

}  // namespace rummage::core
