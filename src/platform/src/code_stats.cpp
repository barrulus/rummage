#include "rummage/platform/code_stats.hpp"

#include <sys/stat.h>
#include <sys/wait.h>

#include "rummage/fs/measure.hpp"
#include "rummage/platform/desktop_entry.hpp"

#ifndef RUMMAGE_TOKEI_PROGRAM
#  define RUMMAGE_TOKEI_PROGRAM ""
#endif

namespace rummage::platform {
CodeAnalysis::CodeAnalysis(core::EventLoop& loop, core::WorkerPool& pool,
                           std::filesystem::path program, std::chrono::milliseconds timeout)
    : loop_{loop}, pool_{pool}, program_{std::move(program)}, timeout_{timeout} {}

CodeAnalysis::~CodeAnalysis() { cancel(); }

void CodeAnalysis::cancel() {
    loop_.remove(timer_);
    timer_ = 0;
    parse_job_.cancel();
    process_.reset();
    running_ = false;
    done_ = {};
}

void CodeAnalysis::finish(CodeReport report) {
    loop_.remove(timer_);
    timer_ = 0;
    running_ = false;
    process_.reset();
    auto done = std::move(done_);
    if (done)
        done(std::make_shared<const CodeReport>(std::move(report)));
}

void CodeAnalysis::stop() {
    if (!running_)
        return;
    stopped_ = CodeError::stopped;
    if (process_)
        process_->stop();
    else {
        parse_job_.cancel();
        CodeReport report;
        report.root = root_;
        report.error = stopped_;
        finish(std::move(report));
    }
}

void CodeAnalysis::start(const std::filesystem::path& root, bool recursive, bool hidden,
                         Done done) {
    cancel();
    root_ = root;
    done_ = std::move(done);
    stopped_ = CodeError::none;
    const auto fail = [this](CodeError error) {
        CodeReport report;
        report.root = root_;
        report.error = error;
        finish(std::move(report));
    };
    struct stat info{};
    if (lstat(root.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) {
        fail(CodeError::invalid_root);
        return;
    }
    if (fs::is_remote(root)) {
        fail(CodeError::remote);
        return;
    }
    auto program = program_;
    if (program.empty()) {
        const auto found = find_program("tokei", Environment::from_environment().path);
        program = found ? *found : std::filesystem::path{RUMMAGE_TOKEI_PROGRAM};
    }
    if (program.empty()) {
        fail(CodeError::missing_tool);
        return;
    }
    process_ =
        std::make_unique<ProgramOutput>(loop_, [this](const std::string& output, int status) {
            const bool truncated = process_->truncated();
            process_.reset();
            loop_.remove(timer_);
            timer_ = 0;
            if (stopped_ != CodeError::none || truncated || !WIFEXITED(status) ||
                WEXITSTATUS(status) != 0) {
                CodeReport report;
                report.root = root_;
                report.error = stopped_ != CodeError::none ? stopped_
                               : truncated                 ? CodeError::output_limit
                                                           : CodeError::failed;
                finish(std::move(report));
                return;
            }
            parse_job_ = pool_.submit(
                core::Priority::nearby,
                [output, root = root_](const core::CancelToken& token) {
                    return parse_code_report(output, root, token);
                },
                [this](CodeReport report) { finish(std::move(report)); });
        });
    std::vector<std::string> args{"--output", "json"};
    if (hidden)
        args.push_back("--hidden");
    if (!recursive)
        args.insert(args.end(), {"--exclude", "*/*"});
    args.insert(args.end(), {"--", "."});
    running_ = true;
    if (!process_->start(program, args, root)) {
        fail(CodeError::failed);
        return;
    }
    timer_ = loop_.add_timer(timeout_, std::chrono::milliseconds{0}, [this] {
        timer_ = 0;
        stopped_ = CodeError::timed_out;
        process_->stop();
    });
}
}  // namespace rummage::platform
