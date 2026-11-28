#include "rummage/core/memory.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <sys/mman.h>
#include <vector>

#include <doctest/doctest.h>

namespace {

// The "Swap:" line of /proc/self/smaps_rollup, for the message when a check fails: memory swapped
// out is not counted as the process's anonymous memory.
std::string swap_line() {
    std::ifstream in{"/proc/self/smaps_rollup"};
    std::string line;
    while (std::getline(in, line)) {
        if (line.starts_with("Swap:")) {
            return line;
        }
    }
    return "Swap: ?";
}

}  // namespace

TEST_CASE("memory report: the process, malloc and the lines for the log") {
    const rummage::core::MemoryReport before = rummage::core::read_memory_report();
    CHECK(before.rss > 0);
    CHECK(before.anonymous > 0);
    CHECK(before.anonymous <= before.rss);
    REQUIRE_FALSE(before.mappings.empty());
    for (std::size_t i = 1; i < before.mappings.size(); ++i) {
        CHECK(before.mappings[i - 1].rss >= before.mappings[i].rss);  // largest first
    }

    // 64 MB from malloc: malloc counts it. Whether the process grows by it depends on what malloc
    // kept from earlier tests (once on Fedora it reused resident pages: the process's anonymous
    // memory came out the same to the byte, *to be verified*), so the process is checked below
    // with a mapping of its own.
    constexpr std::size_t size = std::size_t{64} << 20U;
    std::vector<std::uint8_t> block(size, 1);
    asm volatile("" : : "r"(block.data()) : "memory");  // NOLINT(hicpp-no-assembler)
    const rummage::core::MemoryReport during = rummage::core::read_memory_report();
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
    // Under a sanitizer malloc is the sanitizer's: glibc's mallinfo2 reports nothing.
    CHECK(during.heap_in_use >= before.heap_in_use + size);
#endif

    // 64 MB mapped and touched here: new pages, which the process's anonymous memory must show.
    const rummage::core::MemoryReport unmapped = rummage::core::read_memory_report();
    void* const mapped =
        ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    REQUIRE(mapped != MAP_FAILED);  // NOLINT(performance-no-int-to-ptr)
    std::memset(mapped, 1, size);
    asm volatile("" : : "r"(mapped) : "memory");  // NOLINT(hicpp-no-assembler)
    const rummage::core::MemoryReport touched = rummage::core::read_memory_report();
    ::munmap(mapped, size);
    INFO("anonymous " << unmapped.anonymous << " before the mapping, " << touched.anonymous
                      << " with it; malloc's arena " << during.heap_arena << ", mmapped "
                      << during.heap_mmapped << "; " << swap_line());
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
    // Other threads (earlier tests' pools, malloc's arenas) may give pages back between the two
    // reads (124 KB short of the block once in the CI): a margin.
    constexpr std::size_t margin = std::size_t{4} << 20U;
    CHECK(touched.anonymous + margin >= unmapped.anonymous + size);
#else
    // The sanitizer's allocator gives freed memory back on its own, between the two reads too
    // (135 MB once in the CI, more than the block): no difference holds, only that the process
    // holds the mapping.
    CHECK(touched.anonymous >= size);
#endif

    const std::vector<std::string> lines = rummage::core::describe(during);
    REQUIRE(lines.size() >= 3);
    CHECK(lines[0].starts_with("process: rss "));
    CHECK(lines[1].starts_with("malloc: in use "));
    CHECK(lines[2].starts_with("gpu: "));  // "?" without a GPU client in the tests
    CHECK(block.back() == 1);
}

TEST_CASE("memory: large blocks go back to the system once freed") {
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
    rummage::core::give_back_large_blocks();
    const rummage::core::MemoryReport before = rummage::core::read_memory_report();
    for (int i = 0; i < 8; ++i) {
        // 20 MB, as a decoded photo preview: over the threshold, so a mapping of its own.
        std::vector<std::uint8_t> block(std::size_t{20} << 20U, static_cast<std::uint8_t>(i));
        CHECK(block.back() == i);
    }
    const rummage::core::MemoryReport after = rummage::core::read_memory_report();
    // malloc's own counters, not the process's anonymous memory: another thread still running
    // from an earlier test (Mesa's software renderer keeps some) may allocate meanwhile.
    CHECK(after.heap_free < before.heap_free + (std::size_t{4} << 20U));    // nothing kept free
    CHECK(after.heap_mmapped <= before.heap_mmapped);                       // each unmapped
    CHECK(after.heap_arena < before.heap_arena + (std::size_t{4} << 20U));  // none in the heap
#endif
}

TEST_CASE("memory: reclaim a settled small-block burst without freeing live objects") {
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
    rummage::core::give_back_large_blocks();
    // Below the application's mmap threshold, with a live final block preventing the
    // ordinary top-of-heap release from hiding the retained-pages scenario.
    constexpr std::size_t size = std::size_t{2} << 20;
    std::vector<std::unique_ptr<std::uint8_t[]>> blocks;
    for (int i = 0; i < 64; ++i) {
        auto block = std::make_unique<std::uint8_t[]>(size);
        std::memset(block.get(), 0x5a, size);
        asm volatile("" : : "r"(block.get()) : "memory");  // NOLINT(hicpp-no-assembler)
        blocks.push_back(std::move(block));
    }
    for (std::size_t i = 0; i + 1 < blocks.size(); ++i)
        blocks[i].reset();
    const auto retained = rummage::core::read_memory_report();
    REQUIRE(retained.heap_free >= (std::uint64_t{64} << 20));
    CHECK_FALSE(
        rummage::core::give_back_free_memory_if_needed(std::numeric_limits<std::uint64_t>::max()));
    REQUIRE(rummage::core::give_back_free_memory_if_needed());
    const auto reclaimed = rummage::core::read_memory_report();
    INFO("RSS before " << retained.rss << ", after " << reclaimed.rss << "; " << swap_line());
    CHECK(reclaimed.rss + (std::uint64_t{32} << 20) < retained.rss);
    CHECK(blocks.back()[0] == 0x5a);
    CHECK(blocks.back()[size - 1] == 0x5a);
    // Reporting itself makes small allocations; trimming must not consume the live block.
    CHECK(reclaimed.heap_in_use + (std::uint64_t{1} << 20) >= retained.heap_in_use);
#endif
}
