#include "rummage/platform/network.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include "rummage/core/file.hpp"
#include "rummage/core/i18n.hpp"
#include "rummage/platform/desktop_entry.hpp"
extern char** environ;

namespace rummage::platform {
namespace {
std::string decode(std::string_view field) {
    std::string out;
    for (std::size_t i = 0; i < field.size(); ++i) {
        if (field[i] == '\\' && i + 3 < field.size()) {
            unsigned byte{};
            const auto [end, ec] =
                std::from_chars(field.data() + i + 1, field.data() + i + 4, byte);
            if (ec != std::errc{} || end != field.data() + i + 4 || byte < 32 || byte > 255)
                return {};
            out += static_cast<char>(byte);
            i += 3;
        } else if (static_cast<unsigned char>(field[i]) < 32)
            return {};
        else
            out += field[i];
    }
    return out;
}

std::string escaped(std::string_view text) {
    return core::file_uri("/" + std::string{text}).substr(8);
}
}  // namespace

std::vector<fs::Place> parse_network_services(std::string_view output) {
    std::vector<fs::Place> found;
    while (!output.empty() && found.size() < 256) {
        const auto length = output.find('\n');
        auto line = output.substr(0, length);
        output.remove_prefix(std::min(output.size(), line.size() + 1));
        if (!line.starts_with("=;") || line.size() > 8192)
            continue;
        std::vector<std::string> fields;
        while (!line.empty() && fields.size() < 12) {
            const auto split = line.find(';');
            const auto part = line.substr(0, split);
            fields.push_back(decode(part));
            line.remove_prefix(std::min(line.size(), part.size() + 1));
        }
        if (fields.size() < 9 || fields[3].empty() || fields[6].empty())
            continue;
        const auto& type = fields[4];
        std::string scheme;
        if (type == "_smb._tcp")
            scheme = "smb";
        else if (type == "_sftp-ssh._tcp" || type == "_ssh._tcp")
            scheme = "sftp";
        else if (type == "_ftp._tcp")
            scheme = "ftp";
        else if (type == "_webdav._tcp")
            scheme = "dav";
        else if (type == "_webdavs._tcp")
            scheme = "davs";
        else if (type == "_nfs._tcp")
            scheme = "nfs";
        else
            continue;
        const auto& host = fields[6];
        if (host.find_first_of("/@:#?[]\\") != std::string::npos)
            continue;
        unsigned port{};
        const auto [end, ec] =
            std::from_chars(fields[8].data(), fields[8].data() + fields[8].size(), port);
        if (ec != std::errc{} || end != fields[8].data() + fields[8].size() || !port ||
            port > 65535)
            continue;
        std::string path = "/";
        if (fields.size() > 9) {
            const auto at = fields[9].find("\"path=");
            if (at != std::string::npos) {
                const auto value = fields[9].substr(at + 6, fields[9].find('"', at + 6) - (at + 6));
                if (!value.empty() && value.front() == '/')
                    path = value;
            }
        }
        std::string uri = scheme + "://" + escaped(host);
        if (scheme != "nfs")
            uri += ":" + std::to_string(port);
        uri += "/" + escaped(path.substr(1));
        if (!fs::parse_remote_uri(uri))
            continue;
        if (std::ranges::any_of(found, [&](const auto& place) { return place.uri == uri; }))
            continue;
        found.push_back({.section = fs::Place::Section::bookmarks,
                         .label = fields[3] + " · " + scheme + " · " + host,
                         .path = {},
                         .icon = "server",
                         .disk = {.volume = {}, .mounted = false},
                         .uri = uri});
    }
    return found;
}

NetworkDiscovery discover_network(const core::CancelToken& token) {
    NetworkDiscovery result;
    const char* env = ::getenv("PATH");
    const auto program = find_program("avahi-browse", env ? env : "");
    if (!program) {
        result.error = i18n::tr("Install avahi-browse to discover network servers");
        return result;
    }
    std::array<int, 2> pipes{};
    if (::pipe2(pipes.data(), O_CLOEXEC) != 0) {
        result.error = i18n::tr("Network discovery could not start");
        return result;
    }
    posix_spawn_file_actions_t actions{};
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    std::string executable = program->string();
    std::array<char*, 6> args{executable.data(),
                              const_cast<char*>("--all"),
                              const_cast<char*>("--resolve"),
                              const_cast<char*>("--parsable"),
                              const_cast<char*>("--terminate"),
                              nullptr};
    pid_t child{};
    const int spawn =
        ::posix_spawn(&child, executable.c_str(), &actions, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(pipes[1]);
    if (spawn != 0) {
        ::close(pipes[0]);
        result.error = i18n::tr("Network discovery could not start");
        return result;
    }
    ::fcntl(pipes[0], F_SETFL, O_NONBLOCK);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{6};
    std::string output;
    int status{};
    bool exited = false;
    while (!token.cancelled() && std::chrono::steady_clock::now() < until) {
        pollfd poller{pipes[0], POLLIN, 0};
        ::poll(&poller, 1, 50);
        std::array<char, 4096> bytes{};
        for (;;) {
            const auto n = ::read(pipes[0], bytes.data(), bytes.size());
            if (n <= 0)
                break;
            output.append(bytes.data(), static_cast<std::size_t>(n));
            if (output.size() > 1024 * 1024)
                break;
        }
        if (output.size() > 1024 * 1024)
            break;
        if (::waitpid(child, &status, WNOHANG) == child) {
            exited = true;
            break;
        }
    }
    if (!exited) {
        ::kill(child, SIGKILL);
        while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
    }
    ::close(pipes[0]);
    if (token.cancelled())
        return result;
    result.places = parse_network_services(output);
    if (!exited || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        result.error =
            i18n::tr("Network discovery unavailable or timed out; check the Avahi service");
    return result;
}
}  // namespace rummage::platform
