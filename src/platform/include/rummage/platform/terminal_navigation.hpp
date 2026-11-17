#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include "rummage/platform/desktop_entry.hpp"
#include "rummage/platform/launcher.hpp"

namespace rummage::platform {

// One private, one-shot directory handoff. Loop thread only; cancel before the picker closes.
// Terminal clients may detach: their exit is never used as the directory handoff.
class TerminalNavigation {
public:
    using Report = std::function<void(std::string, bool)>;
    using Navigate = std::function<void(const std::filesystem::path&)>;
    TerminalNavigation(core::EventLoop& loop, Navigate navigate, Report report);
    ~TerminalNavigation();
    TerminalNavigation(const TerminalNavigation&) = delete;
    TerminalNavigation& operator=(const TerminalNavigation&) = delete;

    // An empty shell uses $SHELL, falling back to the account's login shell. An empty helper
    // uses this executable. Explicit paths allow integration tests to run the real helper.
    void start(const std::filesystem::path& directory, const Environment& environment,
               std::filesystem::path shell = {}, std::filesystem::path helper = {});
    void cancel();

    [[nodiscard]] bool active() const { return socket_ >= 0; }

    [[nodiscard]] const std::filesystem::path& session_directory() const { return session_; }

private:
    void receive();
    core::EventLoop& loop_;
    Navigate navigate_;
    Report report_;
    Launcher launcher_;
    std::filesystem::path session_;
    int socket_{-1};
    core::EventLoop::SourceId source_{};
};

// Internal CLI entry point, run inside the terminal, before GUI/thread initialization.
// Supervises the interactive shell and only sends a directory after a normal shell exit.
int run_terminal_navigation(const std::filesystem::path& session,
                            const std::filesystem::path& shell,
                            const std::filesystem::path& directory);

}  // namespace rummage::platform
