#include "rummage/isolate/playback.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "playback_protocol.hpp"
#include "protocol.hpp"
#include "rummage/engines/engines.hpp"
extern char** environ;

namespace rummage::isolate {
namespace {
namespace wire = playback_wire;
using Clock = std::chrono::steady_clock;

struct Child {
    pid_t pid{-1};
    int socket{-1};

    ~Child() { stop(); }

    void stop() {
        if (socket >= 0) {
            ::close(socket);
            socket = -1;
        }
        if (pid > 0) {
            ::kill(pid, SIGKILL);
            while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
            }
            pid = -1;
        }
    }

    bool finish(std::stop_token stop_token, const std::atomic<bool>& paused,
                const std::atomic<std::int64_t>& seeking) {
        if (pid < 0)
            return true;
        ::shutdown(socket, SHUT_WR);
        const auto deadline = Clock::now() + std::chrono::milliseconds{500};
        while (!stop_token.stop_requested() && !paused.load() && seeking.load() < 0 &&
               Clock::now() < deadline) {
            int status = 0;
            if (::waitpid(pid, &status, WNOHANG) == pid) {
                pid = -1;
                ::close(socket);
                socket = -1;
                return WIFEXITED(status) && WEXITSTATUS(status) == 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        stop();
        return true;
    }

    bool start(const std::filesystem::path& program, bool audio = false) {
        if (program.empty())
            return false;
        int pair[2];
        if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) < 0)
            return false;
        if (audio) {
            ::close(pair[0]);
            ::close(pair[1]);
            if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) < 0)
                return false;
        }
        // Move child descriptor above stdio/socket destination before dup and closefrom.
        const int child = ::fcntl(pair[1], F_DUPFD_CLOEXEC, 4);
        ::close(pair[1]);
        if (child < 0) {
            ::close(pair[0]);
            return false;
        }
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, child, audio ? 0 : 3);
        posix_spawn_file_actions_addclosefrom_np(&actions, audio ? 3 : 4);
        const std::string executable = program.string();
        std::array<char*, 2> args{const_cast<char*>(executable.c_str()), nullptr};
        const int result =
            ::posix_spawn(&pid, executable.c_str(), &actions, nullptr, args.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        ::close(child);
        if (result != 0) {
            pid = -1;
            ::close(pair[0]);
            return false;
        }
        socket = pair[0];
        ::fcntl(socket, F_SETFL, ::fcntl(socket, F_GETFL) | O_NONBLOCK);
        return true;
    }
};

std::filesystem::path program(std::string_view name) {
    // Split packages can live in a separate Nix output. Its media engine is the authority.
    const auto registry = engines::installed();
    for (const auto& engine : registry->engines()) {
        if (engine.manifest.name == "media") {
            const auto candidate = engine.program.parent_path() / name;
            if (::access(candidate.c_str(), X_OK) == 0)
                return candidate;
        }
    }
    return engines::locate_program(name);
}

bool wait_readable(int fd, std::stop_token stop) {
    const auto deadline = Clock::now() + std::chrono::seconds{6};
    while (!stop.stop_requested() && Clock::now() < deadline) {
        pollfd value{.fd = fd, .events = POLLIN, .revents = 0};
        const int result = ::poll(&value, 1, 20);
        if (result > 0)
            return (value.revents & POLLIN) != 0;
        if (result < 0 && errno != EINTR)
            return false;
    }
    return false;
}

struct Decoded {
    wire::Response header;
    std::shared_ptr<picture::Image> image;
    std::vector<std::int16_t> samples;
};

std::optional<Decoded> read_result(int fd) {
    auto packet = protocol::receive_packet(fd);
    if (!packet)
        return {};
    wire::Response header;
    if (packet->length != sizeof(header) || packet->fds[1] >= 0) {
        packet->close_fds();
        return {};
    }
    std::memcpy(&header, packet->bytes.data(), sizeof(header));
    if (header.magic != wire::kMagic || header.status > 3 || header.audio > 1 ||
        header.duration_ms < 0 || header.duration_ms > 7LL * 24 * 60 * 60 * 1000 ||
        header.width > wire::kMaxSide || header.height > wire::kMaxSide ||
        (header.width == 0) != (header.height == 0) || header.samples > wire::kMaxSamples ||
        header.samples % 2 != 0) {
        packet->close_fds();
        return {};
    }
    Decoded value{.header = header, .image = {}, .samples = {}};
    if (header.status >= 2) {
        packet->close_fds();
        return value;
    }
    const auto pixels = static_cast<std::size_t>(header.width) * header.height * 4;
    const auto bytes = pixels + header.samples * 2;
    const int payload = packet->fds[0];
    struct stat stat{};
    const auto seals = ::fcntl(payload, F_GET_SEALS);
    if (payload < 0 || ::fstat(payload, &stat) < 0 || stat.st_size != static_cast<off_t>(bytes) ||
        seals < 0 || (seals & (F_SEAL_WRITE | F_SEAL_SHRINK)) != (F_SEAL_WRITE | F_SEAL_SHRINK)) {
        packet->close_fds();
        return {};
    }
    std::vector<std::uint8_t> data(bytes);
    std::size_t offset = 0;
    while (offset < bytes) {
        const auto got =
            ::pread(payload, data.data() + offset, bytes - offset, static_cast<off_t>(offset));
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0) {
            packet->close_fds();
            return {};
        }
        offset += static_cast<std::size_t>(got);
    }
    packet->close_fds();
    if (pixels) {
        value.image = std::make_shared<picture::Image>();
        value.image->width = header.width;
        value.image->height = header.height;
        value.image->rgba.assign(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(pixels));
    }
    value.samples.resize(header.samples);
    if (header.samples)
        std::memcpy(value.samples.data(), data.data() + pixels, header.samples * 2);
    return value;
}
}  // namespace

Playback::Playback(std::filesystem::path path, Programs programs)
    : thread_{[this, path = std::move(path), programs = std::move(programs)](std::stop_token stop) {
          run(stop, path, programs);
      }} {}

Playback::~Playback() { thread_.request_stop(); }

void Playback::pause(bool value) {
    const std::scoped_lock lock{mutex_};
    if (!state_.failed)
        paused_.store(value);
}

void Playback::seek(std::int64_t ms) {
    const std::scoped_lock lock{mutex_};
    if (state_.failed)
        return;
    state_.loading = true;
    seek_.store(std::clamp<std::int64_t>(ms, 0, 7LL * 24 * 60 * 60 * 1000));
}

void Playback::volume(int value) { volume_.store(std::clamp(value, 0, 100)); }

Playback::Snapshot Playback::snapshot() const {
    const std::scoped_lock lock{mutex_};
    auto state = state_;
    state.paused = paused_.load();
    return state;
}

void Playback::run(std::stop_token stop, const std::filesystem::path& path,
                   const Programs& programs) {
    Child decoder, audio;
    auto fail = [this](std::string message) {
        const std::scoped_lock lock{mutex_};
        state_.error = std::move(message);
        state_.loading = false;
        state_.failed = true;
        paused_.store(true);
    };
    if (!decoder.start(programs.decoder.empty() ? program("rummage-playback") : programs.decoder)) {
        fail("Playback backend unavailable");
        return;
    }
    int file = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
    struct stat stat{};
    if (file < 0 || ::fstat(file, &stat) < 0 || !S_ISREG(stat.st_mode)) {
        if (file >= 0)
            ::close(file);
        fail("Media file unavailable");
        return;
    }
    bool first = true, ended = false, audio_failed = false, was_paused = true;
    std::int64_t position = 0;
    auto tick = Clock::now();
    while (!stop.stop_requested()) {
        auto wanted = seek_.exchange(-1);
        const bool paused = paused_.load();
        if (!paused && was_paused && wanted < 0)
            wanted = ended ? 0 : position;
        was_paused = paused;
        if (wanted >= 0) {
            position = wanted;
            ended = false;
            audio.stop();
            tick = Clock::now();
        }
        if (paused_.load())
            audio.stop();
        if (!first && wanted < 0 && (paused_.load() || ended)) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
            tick = Clock::now();
            continue;
        }
        wire::Request request{.seek = wanted >= 0 ? 1U : 0U, .position_ms = position};
        const std::array<int, 1> fds{file};
        const bool sent =
            protocol::send_packet(decoder.socket, std::as_bytes(std::span{&request, 1}),
                                  first ? std::span<const int>{fds} : std::span<const int>{});
        if (first) {
            ::close(file);
            file = -1;
        }
        if (!sent || !wait_readable(decoder.socket, stop)) {
            if (!stop.stop_requested())
                fail("Playback failed or timed out");
            break;
        }
        auto decoded = read_result(decoder.socket);
        if (!decoded || decoded->header.status >= 2) {
            fail(decoded && decoded->header.status == 3 ? "Media seeking unavailable"
                                                        : "Unsupported or damaged media");
            break;
        }
        if (seek_.load() >= 0) {
            first = false;
            continue;
        }  // a newer seek owns the display and sound
        if (!paused_.load() && !decoded->samples.empty() && !audio_failed) {
            if (audio.pid < 0 &&
                !audio.start(programs.audio.empty() ? program("rummage-audio") : programs.audio,
                             true))
                audio_failed = true;
            for (auto& sample : decoded->samples)
                sample = static_cast<std::int16_t>(static_cast<int>(sample) * volume_.load() / 100);
            auto bytes = std::as_bytes(std::span{decoded->samples});
            const auto deadline = Clock::now() + std::chrono::milliseconds{250};
            while (!audio_failed && !bytes.empty() && !stop.stop_requested() && !paused_.load() &&
                   seek_.load() < 0) {
                const auto wrote = ::send(audio.socket, bytes.data(), bytes.size(), MSG_NOSIGNAL);
                if (wrote > 0)
                    bytes = bytes.subspan(static_cast<std::size_t>(wrote));
                else if (wrote < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) &&
                         Clock::now() < deadline) {
                    pollfd output{.fd = audio.socket, .events = POLLOUT, .revents = 0};
                    ::poll(&output, 1, 10);
                } else
                    audio_failed = true;
            }
            if (audio_failed)
                audio.stop();
        }
        {
            const std::scoped_lock lock{mutex_};
            if (decoded->image)
                state_.image = std::move(decoded->image);
            state_.position_ms = position;
            state_.duration_ms = decoded->header.duration_ms;
            state_.loading = false;
            state_.ended = decoded->header.status == 1;
            if (audio_failed)
                state_.error = "Audio output unavailable";
        }
        ended = decoded->header.status == 1;
        first = false;
        if (ended) {
            if (!audio.finish(stop, paused_, seek_)) {
                const std::scoped_lock lock{mutex_};
                state_.error = "Audio output unavailable";
            }
            paused_.store(true);
            audio.stop();
            was_paused = true;
        } else if (!paused_.load())
            position += wire::kStepMs;
        tick += std::chrono::milliseconds{wire::kStepMs};
        while (!stop.stop_requested() && !paused_.load() && seek_.load() < 0 && Clock::now() < tick)
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        if (Clock::now() - tick > std::chrono::milliseconds{100})
            tick = Clock::now();
    }
    if (file >= 0)
        ::close(file);
}
}  // namespace rummage::isolate
