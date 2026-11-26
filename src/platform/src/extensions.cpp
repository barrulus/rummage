#include "rummage/platform/extensions.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <mutex>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <set>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <toml++/toml.hpp>
#include <unistd.h>

#include "rummage/core/file.hpp"
#include "rummage/core/i18n.hpp"

extern char** environ;

namespace rummage::platform::extensions {
namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

struct Fd {
    int value{-1};

    ~Fd() {
        if (value >= 0)
            ::close(value);
    }
};

std::mutex settings_mutex;
core::Config::Extensions settings;
std::uint64_t generation{1};
std::shared_ptr<const Registry> loaded = std::make_shared<Registry>();

bool identifier(std::string_view value) {
    return !value.empty() && value.size() <= 64 && std::ranges::all_of(value, [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-';
    });
}

bool label_ok(std::string_view text, std::size_t limit) {
    return !text.empty() && text.size() <= limit &&
           std::ranges::none_of(text, [](unsigned char c) { return c < 32 || c == 127; });
}

std::string bounded_file(const std::filesystem::path& path) {
    Fd fd{::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW)};
    struct stat info{};
    if (fd.value < 0 || ::fstat(fd.value, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size < 0 || info.st_size > 65536)
        return {};
    std::string text(static_cast<std::size_t>(info.st_size), '\0');
    std::size_t offset = 0;
    while (offset < text.size()) {
        auto count = ::read(fd.value, text.data() + offset, text.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return {};
        offset += static_cast<std::size_t>(count);
    }
    return text;
}

std::vector<std::filesystem::path> roots() {
    std::vector<std::filesystem::path> out;
    const char* data = ::getenv("XDG_DATA_HOME");
    const char* home = ::getenv("HOME");
    if (data && *data == '/')
        out.emplace_back(std::filesystem::path{data} / "rummage/extensions");
    else if (home && *home == '/')
        out.emplace_back(std::filesystem::path{home} / ".local/share/rummage/extensions");
    const char* directories = ::getenv("XDG_DATA_DIRS");
    std::string_view rest =
        directories && *directories ? directories : "/usr/local/share:/usr/share";
    while (!rest.empty()) {
        const auto colon = rest.find(':');
        auto part = rest.substr(0, colon);
        if (part.starts_with('/'))
            out.emplace_back(std::filesystem::path{part} / "rummage/extensions");
        rest.remove_prefix(std::min(rest.size(), part.size() + 1));
    }
    return out;
}

Json request(std::string_view operation, const core::ActionContext& context) {
    Json value = {{"version", 1},
                  {"operation", operation},
                  {"directory", core::file_uri(context.directory)},
                  {"files", Json::array()}};
    for (const auto& target : context.targets)
        value["files"].push_back(core::file_uri(target.path));
    return value;
}

Json response(std::string_view text) {
    auto value = Json::parse(text, [](int depth, Json::parse_event_t, Json&) {
        if (depth > 16)
            throw std::runtime_error("extension reply nesting exceeds 16");
        return true;
    });
    if (!value.is_object() || value.value("version", 0) != 1)
        throw std::runtime_error("extension reply needs version 1");
    return value;
}

std::vector<std::string> git(const std::filesystem::path& directory) {
    return {"git",
            "--literal-pathspecs",
            "--no-optional-locks",
            "-c",
            "core.fsmonitor=false",
            "-c",
            "core.untrackedCache=false",
            "-C",
            std::filesystem::absolute(directory).string()};
}

struct Cached {
    Status status;
    Clock::time_point when{};
    bool running{false};
};

std::mutex git_mutex;
std::map<std::filesystem::path, Cached> git_cache;

struct RootCache {
    std::filesystem::path root;
    Clock::time_point when;
};

std::map<std::filesystem::path, RootCache> root_cache;

void combine(Status& status, const std::string& name,
             std::shared_ptr<const fs::Decoration> decoration) {
    const auto it = status.entries.find(name);
    if (it == status.entries.end())
        status.entries.emplace(name, std::move(decoration));
    else {
        auto merged = std::make_shared<fs::Decoration>(*it->second);
        // A folder can contain hundreds of mixed changes. Keep one bounded indicator;
        // the complete, deduplicated meanings remain available in its status description.
        if (decoration->severity > merged->severity ||
            (decoration->severity == merged->severity &&
             decoration->badge.size() > merged->badge.size()))
            merged->badge = decoration->badge;
        bool present = false;
        for (std::size_t start = 0; start < merged->label.size();) {
            const auto end = merged->label.find("; ", start);
            if (merged->label.substr(start, end - start) == decoration->label)
                present = true;
            if (end == std::string::npos)
                break;
            start = end + 2;
        }
        if (!present && merged->label.size() + decoration->label.size() < 512)
            merged->label += "; " + decoration->label;
        merged->severity = std::max(merged->severity, decoration->severity);
        it->second = std::move(merged);
    }
}
}  // namespace

ProcessResult run(const std::vector<std::string>& argv, const std::filesystem::path& directory,
                  std::string_view input, const core::CancelToken& token,
                  std::chrono::milliseconds timeout, std::size_t limit, bool git_environment) {
    ProcessResult result;
    if (argv.empty() || argv.size() > 4096 || input.size() > 1024 * 1024 || token.cancelled()) {
        result.error = "Invalid or cancelled extension request";
        return result;
    }
    for (const auto& arg : argv)
        if (arg.find('\0') != std::string::npos) {
            result.error = "NUL in process argument";
            return result;
        }
    Fd in{::memfd_create("rummage-extension-input", MFD_CLOEXEC)},
        null{::open("/dev/null", O_WRONLY | O_CLOEXEC)};
    int pair[2];
    if (in.value < 0 || null.value < 0 || ::pipe2(pair, O_CLOEXEC) != 0) {
        result.error = "Cannot create extension pipes";
        return result;
    }
    Fd out{pair[0]}, child_out{pair[1]},
        cwd{::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    if (cwd.value < 0) {
        result.error = "Extension folder unavailable";
        return result;
    }
    std::size_t offset = 0;
    while (offset < input.size()) {
        const auto n = ::write(in.value, input.data() + offset, input.size() - offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            result.error = "Cannot write extension request";
            return result;
        }
        offset += static_cast<std::size_t>(n);
    }
    ::lseek(in.value, 0, SEEK_SET);
    ::fcntl(out.value, F_SETFL, O_NONBLOCK);
    std::vector<char*> arguments;
    for (const auto& arg : argv)
        arguments.push_back(const_cast<char*>(arg.c_str()));
    arguments.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attrs;
    ::posix_spawn_file_actions_init(&actions);
    ::posix_spawnattr_init(&attrs);
    ::posix_spawn_file_actions_adddup2(&actions, in.value, 0);
    ::posix_spawn_file_actions_adddup2(&actions, child_out.value, 1);
    ::posix_spawn_file_actions_adddup2(&actions, null.value, 2);
    ::posix_spawn_file_actions_addfchdir_np(&actions, cwd.value);
    ::posix_spawn_file_actions_addclosefrom_np(&actions, 3);
    sigset_t empty;
    ::sigemptyset(&empty);
    ::posix_spawnattr_setsigmask(&attrs, &empty);
    ::posix_spawnattr_setflags(&attrs, POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGMASK);
    std::vector<char*> environment;
    if (git_environment) {
        // Inherited repository/config redirection must never override the displayed folder.
        for (char** item = environ; *item; ++item)
            if (!std::string_view{*item}.starts_with("GIT_"))
                environment.push_back(*item);
        environment.push_back(nullptr);
    }
    pid_t pid = -1;
    const int started =
        ::posix_spawnp(&pid, argv.front().c_str(), &actions, &attrs, arguments.data(),
                       git_environment ? environment.data() : environ);
    ::posix_spawn_file_actions_destroy(&actions);
    ::posix_spawnattr_destroy(&attrs);
    if (started) {
        result.error = std::error_code{started, std::generic_category()}.message();
        return result;
    }
    ::close(child_out.value);
    child_out.value = -1;
    const auto deadline = Clock::now() + timeout;
    bool finished = false, eof = false;
    int wait_status = 0;
    while (!finished || !eof) {
        if (token.cancelled() || Clock::now() >= deadline) {
            result.error = token.cancelled() ? "Extension cancelled" : "Extension timed out";
            break;
        }
        std::array<char, 8192> bytes{};
        for (unsigned chunk = 0; chunk < 16; ++chunk) {
            const auto count = ::read(out.value, bytes.data(), bytes.size());
            if (count == 0) {
                eof = true;
                break;
            }
            if (count < 0) {
                if (errno == EINTR)
                    continue;
                if (errno != EAGAIN && errno != EWOULDBLOCK)
                    result.error = "Extension output failed";
                break;
            }
            if (result.output.size() + static_cast<std::size_t>(count) > limit) {
                result.error = "Extension output exceeded limit";
                break;
            }
            result.output.append(bytes.data(), static_cast<std::size_t>(count));
        }
        if (!result.error.empty())
            break;
        if (!finished) {
            siginfo_t info{};
            const auto waited =
                ::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT);
            if (waited == 0 && info.si_pid == pid)
                finished = true;
            else if (waited < 0 && errno != EINTR) {
                finished = true;
                result.error = "Extension child unavailable";
                break;
            }
        }
        if (!finished || !eof) {
            pollfd ready{eof ? -1 : out.value, POLLIN, 0};
            ::poll(&ready, 1, 20);
        }
    }
    // Also kill descendants retaining stdout or trying to survive the request lifecycle.
    ::kill(-pid, SIGKILL);
    // Leave the leader unreaped until its group is killed, preventing PID reuse.
    while (::waitpid(pid, &wait_status, 0) < 0 && errno == EINTR) {
    }
    if (result.error.empty())
        result.code = WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : -1;
    return result;
}

void configure(const core::Config::Extensions& config) {
    const std::lock_guard lock{settings_mutex};
    if (settings == config)
        return;
    settings = config;
    ++generation;
    auto empty = std::make_shared<Registry>();
    empty->config = config;
    empty->generation = generation;
    loaded = std::move(empty);
}

void reload() {
    const std::lock_guard lock{settings_mutex};
    ++generation;
    auto empty = std::make_shared<Registry>();
    empty->config = settings;
    empty->generation = generation;
    loaded = std::move(empty);
}

std::shared_ptr<const Registry> snapshot() {
    const std::lock_guard lock{settings_mutex};
    return loaded;
}

std::shared_ptr<const Registry> discover(const core::Config::Extensions& config,
                                         const std::vector<std::filesystem::path>& directories) {
    auto out = std::make_shared<Registry>();
    out->config = config;
    std::set<std::string> seen;
    for (const auto& id : config.enabled) {
        if (!identifier(id) || !seen.insert(id).second || seen.size() > 64) {
            out->problems.push_back("Invalid, duplicate or excess extension ID: " + id);
            continue;
        }
        std::filesystem::path folder;
        for (const auto& root : directories) {
            std::error_code ec;
            if (std::filesystem::exists(root / id / "extension.toml", ec)) {
                folder = root / id;
                break;
            }
        }
        if (folder.empty()) {
            out->problems.push_back(id + ": extension not installed");
            continue;
        }
        try {
            const auto manifest = toml::parse(bounded_file(folder / "extension.toml"));
            if (manifest["extension"]["api"].value_or(0) != 1 ||
                manifest["extension"]["id"].value_or(std::string{}) != id)
                throw std::runtime_error("incompatible API or ID");
            Extension value;
            value.id = id;
            value.label = manifest["extension"]["label"].value_or(id);
            const auto program = manifest["extension"]["program"].value_or(std::string{});
            if (!identifier(program) || !label_ok(value.label, 128))
                throw std::runtime_error("invalid program or label");
            value.program = folder / program;
            struct stat executable{};
            if (::stat(value.program.c_str(), &executable) != 0 || !S_ISREG(executable.st_mode) ||
                ::access(value.program.c_str(), X_OK) != 0)
                throw std::runtime_error("program unavailable or not executable");
            value.status = manifest["extension"]["status"].value_or(false);
            std::set<std::string> commands;
            if (const auto* entries = manifest["command"].as_array())
                for (const auto& node : *entries) {
                    const auto* table = node.as_table();
                    if (!table)
                        throw std::runtime_error("command must be a table");
                    const auto& entry = *table;
                    Command command;
                    command.id = entry["id"].value_or(std::string{});
                    command.label = entry["label"].value_or(std::string{});
                    if (!identifier(command.id) || !label_ok(command.label, 128) ||
                        !commands.insert(command.id).second || commands.size() > 32)
                        throw std::runtime_error("invalid or excess command");
                    const auto selection = entry["selection"].value_or(std::string{"any"});
                    if (selection == "one")
                        command.selection = core::Action::Selection::one;
                    else if (selection == "many")
                        command.selection = core::Action::Selection::many;
                    else if (selection == "background")
                        command.selection = core::Action::Selection::background;
                    else if (selection != "any")
                        throw std::runtime_error("invalid selection scope");
                    value.commands.push_back(std::move(command));
                }
            out->extensions.push_back(std::move(value));
        } catch (const std::exception& error) {
            out->problems.push_back(id + ": " + error.what());
        }
    }
    return out;
}

std::shared_ptr<const Registry> registry() {
    core::Config::Extensions config;
    std::uint64_t version;
    {
        const std::lock_guard lock{settings_mutex};
        config = settings;
        version = generation;
        if (loaded->generation == version &&
            (!loaded->extensions.empty() || !loaded->problems.empty() || config.enabled.empty()))
            return loaded;
    }
    auto found = std::make_shared<Registry>(*discover(config, roots()));
    found->generation = version;
    {
        const std::lock_guard lock{settings_mutex};
        if (generation == version)
            loaded = found;
    }
    return found;
}

Result invoke(const Extension& extension, const Command& command,
              const core::ActionContext& context, const core::CancelToken& token) {
    try {
        auto payload = request("command", context);
        payload["command"] = command.id;
        const auto process =
            run({extension.program.string()}, context.directory, payload.dump(), token);
        if (process.code != 0)
            return {process.error.empty() ? "Extension command failed" : process.error, true};
        const auto reply = response(process.output);
        auto message = reply.value("message", std::string{"Extension completed"});
        if (!label_ok(message, 1024))
            return {"Invalid extension message", true};
        return {message, reply.value("error", false)};
    } catch (const std::exception&) {
        return {"Invalid extension reply", true};
    }
}

Status git_status(const std::filesystem::path& directory, const core::CancelToken& token,
                  bool cached) {
    Status out;
    std::filesystem::path root;
    bool known = false;
    {
        const std::lock_guard lock{git_mutex};
        const auto it = root_cache.find(directory);
        if (cached && it != root_cache.end() &&
            Clock::now() - it->second.when < std::chrono::seconds{5}) {
            root = it->second.root;
            known = true;
        }
    }
    std::vector<std::string> args;
    if (!known) {
        args = git(directory);
        args.insert(args.end(), {"rev-parse", "--show-toplevel"});
        const auto discovered =
            run(args, directory, {}, token, std::chrono::seconds{2}, 8192, true);
        if (!discovered.error.empty()) {
            out.problem = "Git unavailable: " + discovered.error;
            return out;
        }
        if (discovered.code == 0) {
            auto root_text = discovered.output;
            if (!root_text.empty() && root_text.back() == '\n')
                root_text.pop_back();
            root = root_text;
            if (!root.is_absolute()) {
                out.problem = "Invalid Git repository root";
                return out;
            }
        }
        const std::lock_guard lock{git_mutex};
        if (root_cache.size() >= 128)
            root_cache.clear();
        root_cache[directory] = {root, Clock::now()};
    }
    if (root.empty())
        return out;
    Status all;
    bool use = false;
    {
        const std::lock_guard lock{git_mutex};
        auto& entry = git_cache[root];
        if (cached && (entry.running || Clock::now() - entry.when < std::chrono::seconds{2})) {
            all = entry.status;
            use = true;
        } else
            entry.running = true;
    }
    if (!use) {
        args = git(root);
        args.insert(args.end(), {"status", "--porcelain=v1", "-z", "--untracked-files=normal",
                                 "--ignored=matching", "--ignore-submodules=all"});
        const auto process =
            run(args, root, {}, token, std::chrono::seconds{3}, 2 * 1024 * 1024, true);
        all.repository = root;
        if (process.code != 0)
            all.problem = process.error.empty() ? "Git status failed" : process.error;
        else {
            std::string_view rest = process.output;
            std::size_t count = 0;
            while (!rest.empty()) {
                const auto end = rest.find('\0');
                if (end == std::string_view::npos || end < 3 || ++count > 32768) {
                    all.problem = "Invalid or excessive Git status";
                    break;
                }
                const auto record = rest.substr(0, end);
                rest.remove_prefix(end + 1);
                const char x = record[0], y = record[1];
                const auto name = record.substr(3);
                const std::filesystem::path relative{name};
                if (relative.is_absolute() || name.empty() || *relative.begin() == "..") {
                    all.problem = "Invalid Git path";
                    break;
                }
                auto badge = std::make_shared<fs::Decoration>();
                if (x == '!' && y == '!') {
                    badge->badge = "I";
                    badge->label = i18n::tr("Git: ignored");
                } else if (x == '?' && y == '?') {
                    badge->badge = "?";
                    badge->label = i18n::tr("Git: untracked");
                    badge->severity = 1;
                } else if (x == 'U' || y == 'U' || (x == 'A' && y == 'A') ||
                           (x == 'D' && y == 'D')) {
                    badge->badge = "!";
                    badge->label = i18n::tr("Git: conflicted");
                    badge->severity = 3;
                } else {
                    bool staged = x != ' ', modified = y != ' ';
                    badge->badge = staged ? (modified ? "SM" : "S") : "M";
                    badge->label = staged ? (modified ? i18n::tr("Git: staged and modified")
                                                      : i18n::tr("Git: staged"))
                                          : i18n::tr("Git: modified");
                    badge->severity = modified ? 2U : 1U;
                }
                all.entries[relative.lexically_normal().string()] = std::move(badge);
                if (x == 'R' || x == 'C' || y == 'R' || y == 'C') {
                    const auto old = rest.find('\0');
                    if (old == std::string_view::npos) {
                        all.problem = "Invalid Git rename";
                        break;
                    }
                    rest.remove_prefix(old + 1);
                }
            }
            if (!all.problem.empty())
                all.entries.clear();
        }
        const std::lock_guard lock{git_mutex};
        if (git_cache.size() > 8)
            git_cache.clear();
        git_cache[root] = {all, Clock::now(), false};
    }
    out.repository = root;
    out.problem = all.problem;
    std::error_code canonical_error;
    const auto physical = std::filesystem::canonical(directory, canonical_error);
    if (canonical_error) {
        out.problem = "Git folder unavailable";
        return out;
    }
    auto prefix = physical.lexically_relative(root).generic_string();
    if (prefix == ".")
        prefix.clear();
    else if (!prefix.empty())
        prefix += '/';
    for (const auto& [path, decoration] : all.entries) {
        // Git collapses untracked/ignored directory trees. Reuse that state for the
        // already-listed children when browsing inside one, without recursive scans.
        if (path.ends_with('/') && prefix.starts_with(path) &&
            (decoration->badge == "?" || decoration->badge == "I"))
            out.directory_decoration = decoration;
        if (!path.starts_with(prefix))
            continue;
        const auto tail = std::string_view{path}.substr(prefix.size());
        const auto slash = tail.find('/');
        const auto name = std::string{tail.substr(0, slash)};
        if (!name.empty())
            combine(out, name, decoration);
    }
    return out;
}

Status status(const std::filesystem::path& directory, const std::vector<std::string>& names,
              const core::CancelToken& token) {
    const auto current = registry();
    Status out;
    if (current->config.git)
        out = git_status(directory, token);
    if (out.directory_decoration)
        for (const auto& name : names)
            out.entries.try_emplace(name, out.directory_decoration);
    if (names.size() > 4096) {
        out.problem = "Extension status is limited to 4096 entries per folder";
        return out;
    }
    core::ActionContext context;
    context.directory = directory;
    for (const auto& name : names)
        context.targets.push_back({directory / name, {}});
    const auto deadline = Clock::now() + std::chrono::seconds{6};
    for (const auto& extension : current->extensions) {
        if (Clock::now() >= deadline) {
            out.problem = "Extension status budget exceeded";
            break;
        }
        if (token.cancelled())
            break;
        if (!extension.status)
            continue;
        try {
            const auto process = run({extension.program.string()}, directory,
                                     request("status", context).dump(), token,
                                     std::min(std::chrono::milliseconds{3000},
                                              std::chrono::duration_cast<std::chrono::milliseconds>(
                                                  deadline - Clock::now())));
            if (process.code != 0) {
                out.problem =
                    extension.id + ": " + (process.error.empty() ? "status failed" : process.error);
                continue;
            }
            const auto reply = response(process.output);
            const auto& decorations = reply.at("decorations");
            if (!decorations.is_object() || decorations.size() > 4096)
                throw std::runtime_error("invalid decorations");
            Status provider;
            for (const auto& [uri, item] : decorations.items()) {
                const auto path = core::path_from_uri(uri);
                if (!path || path->parent_path() != directory ||
                    std::ranges::find(names, path->filename().string()) == names.end())
                    throw std::runtime_error("decoration path outside request");
                auto value = std::make_shared<fs::Decoration>();
                value->badge = item.value("badge", std::string{});
                value->label = item.value("label", std::string{});
                value->severity = item.value("severity", 0U);
                if (!label_ok(value->badge, 12) || !label_ok(value->label, 128) ||
                    value->severity > 3)
                    throw std::runtime_error("invalid badge");
                combine(provider, path->filename().string(), std::move(value));
            }
            for (auto& [name, value] : provider.entries)
                combine(out, name, std::move(value));
        } catch (const std::exception&) {
            out.problem = extension.id + ": invalid status reply";
        }
    }
    if (!current->problems.empty())
        out.problem = current->problems.front();
    return out;
}

ProcessResult git_diff(const core::ActionContext& context, bool staged,
                       const core::CancelToken& token) {
    auto args = git(context.directory);
    args.insert(args.end(), {"diff", "--no-ext-diff", "--no-textconv", "--no-color"});
    if (staged)
        args.push_back("--cached");
    args.push_back("--");
    for (const auto& target : context.targets)
        args.push_back(target.path.string());
    return run(args, context.directory, {}, token, std::chrono::seconds{3}, 65536, true);
}
}  // namespace rummage::platform::extensions
