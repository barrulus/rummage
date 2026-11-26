#pragma once
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "rummage/core/config.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/entry.hpp"

namespace rummage::platform::extensions {
struct Command {
    std::string id, label;
    core::Action::Selection selection{core::Action::Selection::any};
};

struct Extension {
    std::string id, label;
    std::filesystem::path program;
    bool status{false};
    std::vector<Command> commands;
};

struct Registry {
    std::uint64_t generation{};
    core::Config::Extensions config;
    std::vector<Extension> extensions;
    std::vector<std::string> problems;
};

struct Result {
    std::string message;
    bool failed{false};
};

struct Status {
    std::map<std::string, std::shared_ptr<const fs::Decoration>> entries;
    std::filesystem::path repository;
    std::shared_ptr<const fs::Decoration> directory_decoration;
    std::string problem;
};

struct ProcessResult {
    std::string output;
    int code{-1};
    std::string error;
};

// Bounded process-group lifetime, stdin payload and stdout; stderr is discarded.
ProcessResult run(const std::vector<std::string>& argv, const std::filesystem::path& directory,
                  std::string_view input, const core::CancelToken& token,
                  std::chrono::milliseconds timeout = std::chrono::seconds{3},
                  std::size_t limit = 2 * 1024 * 1024, bool git_environment = false);
// configure/snapshot do no filesystem work. registry(), status() and invoke() are worker calls.
void configure(const core::Config::Extensions& config);
void reload();
std::shared_ptr<const Registry> snapshot();
std::shared_ptr<const Registry> registry();
std::shared_ptr<const Registry> discover(const core::Config::Extensions&,
                                         const std::vector<std::filesystem::path>& roots);
Result invoke(const Extension&, const Command&, const core::ActionContext&,
              const core::CancelToken&);
Status status(const std::filesystem::path&, const std::vector<std::string>& names,
              const core::CancelToken&);
Status git_status(const std::filesystem::path&, const core::CancelToken&, bool cached = true);
ProcessResult git_diff(const core::ActionContext&, bool staged, const core::CancelToken&);
}  // namespace rummage::platform::extensions
