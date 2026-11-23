#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/platform/program_output.hpp"

namespace rummage::platform {
struct CodeCounts {
    std::uint64_t code{0}, comments{0}, blanks{0};

    [[nodiscard]] std::uint64_t lines() const { return code + comments + blanks; }

    bool operator==(const CodeCounts&) const = default;
};

struct CodeFile {
    std::filesystem::path relative;
    CodeCounts counts;
};

struct CodeLanguage {
    std::string name, key;
    std::size_t depth{0};
    CodeCounts counts;
    std::vector<CodeFile> files;
};
enum class CodeError {
    none,
    missing_tool,
    invalid_root,
    remote,
    failed,
    invalid_output,
    output_limit,
    stopped,
    timed_out
};

struct CodeReport {
    std::filesystem::path root;
    std::vector<CodeLanguage> languages;
    CodeCounts total;
    std::uint64_t files{0};  // Physical source files; embedded fragments are not counted twice.
    bool inaccurate{false};
    CodeError error{CodeError::none};
};

// Bounded, strict decoding. Counts are derived from reports/blobs, never double-counted
// from Tokei's duplicate children/Total summaries. Paths must remain inside root.
[[nodiscard]] CodeReport parse_code_report(std::string_view json, const std::filesystem::path& root,
                                           const core::CancelToken& token = {});

// On-demand Tokei analysis. Process I/O is asynchronous; JSON decoding runs on a worker.
class CodeAnalysis {
public:
    using Done = std::function<void(std::shared_ptr<const CodeReport>)>;
    CodeAnalysis(core::EventLoop& loop, core::WorkerPool& pool, std::filesystem::path program = {},
                 std::chrono::milliseconds timeout = std::chrono::seconds{30});
    ~CodeAnalysis();
    void start(const std::filesystem::path& root, bool recursive, bool hidden, Done done);
    void cancel();
    void stop();

    [[nodiscard]] bool running() const { return running_; }

private:
    void finish(CodeReport report);
    core::EventLoop& loop_;
    core::WorkerPool& pool_;
    std::filesystem::path program_, root_;
    std::chrono::milliseconds timeout_;
    std::unique_ptr<ProgramOutput> process_;
    core::CancelToken parse_job_;
    core::EventLoop::SourceId timer_{0};
    Done done_;
    CodeError stopped_{CodeError::none};
    bool running_{false};
};
}  // namespace rummage::platform
