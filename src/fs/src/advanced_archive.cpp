#include "rummage/fs/advanced_archive.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "rummage/core/i18n.hpp"
#include "rummage/fs/operations.hpp"

extern char** environ;  // NOLINT(readability-redundant-declaration)

namespace rummage::fs {
namespace {
struct Fd {
    int value{-1};

    ~Fd() {
        if (value >= 0)
            close(value);
    }
};

struct Temporary {
    std::filesystem::path path;

    ~Temporary() {
        std::error_code ignored;
        if (!path.empty())
            std::filesystem::remove_all(path, ignored);
    }
};

std::string last_error() { return std::error_code{errno, std::generic_category()}.message(); }

// Blocking worker job: fixed diagnostic memory, private stdin, no shell and no inherited fds.
// Cancellation kills the process group and reaps before deleting staging output.
std::string run(const ArchiveOptions& options, std::vector<std::string> args,
                const std::filesystem::path& source, std::string_view password,
                const core::CancelToken& token) {
    if (token.cancelled())
        return {};
    Fd input{memfd_create("rummage-archive-input", MFD_CLOEXEC)};
    std::array<int, 2> pipefd{-1, -1};
    if (input.value < 0 || pipe2(pipefd.data(), O_CLOEXEC) != 0)
        return last_error();
    Fd output{pipefd[0]}, child_output{pipefd[1]};
    fcntl(output.value, F_SETFL, O_NONBLOCK);
    if (!password.empty()) {
        for (int repeat = 0; repeat < 2; ++repeat)
            if (write(input.value, password.data(), password.size()) !=
                    static_cast<ssize_t>(password.size()) ||
                write(input.value, "\n", 1) != 1)
                return last_error();
    }
    if (lseek(input.value, 0, SEEK_SET) < 0)
        return last_error();
    Fd directory{open(source.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
    if (directory.value < 0)
        return last_error();
    args.insert(args.begin(), options.program.string());
    std::vector<char*> argv;
    for (auto& arg : args)
        argv.push_back(arg.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attrs;
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attrs);
    posix_spawn_file_actions_adddup2(&actions, input.value, 0);
    posix_spawn_file_actions_adddup2(&actions, child_output.value, 1);
    posix_spawn_file_actions_adddup2(&actions, child_output.value, 2);
    posix_spawn_file_actions_addfchdir_np(&actions, directory.value);
    posix_spawn_file_actions_addclosefrom_np(&actions, 3);
    sigset_t empty;
    sigemptyset(&empty);
    posix_spawnattr_setsigmask(&attrs, &empty);
    posix_spawnattr_setflags(&attrs, POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGMASK);
    pid_t child = -1;
    const int spawned = posix_spawn(&child, argv.front(), &actions, &attrs, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attrs);
    if (spawned != 0)
        return std::error_code{spawned, std::generic_category()}.message();
    close(child_output.value);
    child_output.value = -1;
    int status = 0;
    for (;;) {
        if (token.cancelled()) {
            kill(-child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
            }
            return {};
        }
        // Diagnostics may contain backend-supplied names and secret prompts. Drain, never log.
        std::array<char, 8192> bytes{};
        for (int chunks = 0; chunks < 16 && read(output.value, bytes.data(), bytes.size()) > 0;
             ++chunks) {
        }
        const auto waited = waitpid(child, &status, WNOHANG);
        if (waited == child)
            break;
        if (waited < 0 && errno != EINTR)
            return last_error();
        pollfd ready{output.value, POLLIN, 0};
        poll(&ready, 1, 50);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return std::string{
            i18n::tr("7z failed: check the password, input files and free disk space")};
    return {};
}

std::vector<std::string> test_args(const std::filesystem::path& path, bool password) {
    std::vector<std::string> args{"t", "-bd", "-bso0", "-bsp0", "-y"};
    if (!password)
        args.push_back("-p-");  // no interactive prompt without submitted input
    args.push_back("--");
    args.push_back(path.string());
    return args;
}
}  // namespace

ArchivePassword::ArchivePassword(std::string_view text) : value{text} {}

ArchivePassword::~ArchivePassword() {
    if (!value.empty())
        explicit_bzero(value.data(), value.size());
}

std::filesystem::path seven_zip_program() {
    const char* raw =
        std::getenv("PATH");  // NOLINT(concurrency-mt-unsafe): environment is immutable
    if (!raw)
        return {};
    const std::string_view path{raw};
    for (const std::string_view name : {"7zz", "7z"}) {
        std::size_t at = 0;
        while (at <= path.size()) {
            const auto end = std::min(path.find(':', at), path.size());
            const auto component = path.substr(at, end - at);
            if (component.starts_with('/')) {
                auto program = std::filesystem::path{component} / name;
                if (access(program.c_str(), X_OK) == 0)
                    return program;
            }
            at = end + 1;
        }
    }
    return {};
}

TransferResult run_advanced_archive(const TransferRequest& request, const core::CancelToken& token,
                                    const TransferCallbacks& callbacks) {
    TransferResult result;
    if (!request.archive_options) {
        result.refused = "Missing archive options";
        return result;
    }
    const auto& options = *request.archive_options;
    ArchivePassword secret{options.password ? options.password->value : std::string{}};
    if (options.password && !options.password->value.empty()) {
        explicit_bzero(options.password->value.data(), options.password->value.size());
        options.password->value.clear();
    }
    if (options.program.empty()) {
        result.refused = i18n::tr(
            "Install 7zz (7-Zip) or 7z and try again; these also support archive passwords.");
        return result;
    }
    if (request.names.empty() || options.level < 0 || options.level > 9 ||
        options.volume_bytes > (std::uint64_t{1} << 40U) ||
        (options.volume_bytes != 0 && options.volume_bytes < 1024) || secret.value.size() > 4096 ||
        secret.value.find_first_of(std::string_view{"\r\n\0", 3}) != std::string::npos ||
        ((options.encrypt_names || options.password) && secret.value.empty()) ||
        std::ranges::any_of(request.names,
                            [](const auto& name) { return name_problem(name).has_value(); })) {
        result.refused = i18n::tr("Invalid archive options");
        return result;
    }
    if (token.cancelled()) {
        result.cancelled = true;
        return result;
    }
    Temporary temporary;
    std::filesystem::path output;
    if (options.test_only) {
        if (request.names.size() != 1) {
            result.refused = i18n::tr("Invalid archive options");
            return result;
        }
        output = request.source / request.names.front();
    } else {
        if (name_problem(request.archive_name)) {
            result.refused = i18n::tr("Invalid archive options");
            return result;
        }
        std::string pattern = (request.destination / ".rummage-archive-XXXXXX").string();
        if (!mkdtemp(pattern.data())) {
            result.refused = last_error();
            return result;
        }
        temporary.path = pattern;
        output = temporary.path / request.archive_name;
        std::vector<std::string> args{"a",      "-t7z",  "-mx=" + std::to_string(options.level),
                                      "-mmt=2", "-snl",  "-bd",
                                      "-bso0",  "-bsp0", "-y",
                                      "-spd"};
        if (options.volume_bytes)
            args.push_back("-v" + std::to_string(options.volume_bytes) + "b");
        if (!secret.value.empty())
            args.push_back("-p");  // create prompts twice on stdin
        if (options.encrypt_names)
            args.push_back("-mhe=on");
        args.push_back("--");
        args.push_back(output.string());
        for (const auto& name : request.names)
            args.push_back("./" + name);
        if (callbacks.progress)
            callbacks.progress({.current = request.archive_name});
        const auto error = run(options, std::move(args), request.source, secret.value, token);
        if (!error.empty())
            result.errors.push_back(error);
        if (options.volume_bytes)
            output += ".001";
    }
    if (result.errors.empty() && !token.cancelled() && (options.verify || options.test_only)) {
        if (callbacks.progress)
            callbacks.progress({.current = output.filename().string(), .verifying = true});
        const auto error = run(options, test_args(output, !secret.value.empty()), request.source,
                               secret.value, token);
        if (!error.empty())
            result.errors.push_back(error);
    }
    result.cancelled = token.cancelled();
    if (result.cancelled || !result.errors.empty())
        return result;
    if (options.test_only) {
        result.files_done = 1;
        return result;
    }
    const auto from = options.volume_bytes ? temporary.path : output;
    const auto proposed = request.archive_name + (options.volume_bytes ? " volumes" : "");
    for (unsigned attempt = 0; attempt < 10000; ++attempt) {
        const auto name =
            attempt == 0 ? proposed
                         : free_name(request.destination, proposed, options.volume_bytes != 0, {});
        if (renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, (request.destination / name).c_str(),
                      RENAME_NOREPLACE) == 0) {
            if (options.volume_bytes)
                temporary.path.clear();
            result.placed.push_back(name);
            result.items.push_back({.from = {}, .to = request.destination / name});
            result.files_done = request.names.size();
            return result;
        }
        if (errno != EEXIST) {
            result.errors.push_back(last_error());
            return result;
        }
    }
    result.errors.push_back(std::string{i18n::tr("Invalid archive options")});
    return result;
}
}  // namespace rummage::fs
