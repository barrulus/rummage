// A preview helper that misbehaves on purpose, for tests/helpers_test.cpp. Its last argument
// says how: crash (aborts on a request), hang (never answers), garbage (answers nonsense),
// big-lie (claims a picture larger than the memfd it sends), goodbye (says goodbye instead of
// answering, as a helper leaving after its idle time would), no-sandbox (exits as a helper that
// could not build its cage).
#include <array>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>

#include "protocol.hpp"

namespace protocol = rummage::isolate::protocol;

int main(int argc, char** argv) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const std::string_view mode{argv[argc - 1]};
    if (mode == "missing-libraries") {
        return 127;
    }
    if (mode == "no-sandbox") {
        return protocol::kSandboxFailedExit;  // as a helper that could not build its cage
    }
    std::signal(SIGPIPE, SIG_IGN);  // NOLINT(cert-err33-c)
    while (true) {
        std::optional<protocol::Packet> packet = protocol::receive_packet(protocol::kSocketFd);
        if (!packet) {
            return 0;
        }
        packet->close_fds();
        if (mode == "crash") {
            std::abort();
        }
        if (mode == "hang") {
            while (true) {
                ::pause();
            }
        }
        if (mode == "garbage") {
            const std::array<std::byte, 7> junk{};
            static_cast<void>(protocol::send_packet(protocol::kSocketFd, junk));
            continue;
        }
        if (mode == "goodbye") {
            const protocol::Request goodbye{.magic = protocol::kGoodbyeMagic};
            static_cast<void>(
                protocol::send_packet(protocol::kSocketFd, std::as_bytes(std::span{&goodbye, 1})));
            return 0;
        }
        if (mode == "text-lie") {
            // Three lines said, two sent.
            const std::string_view two = "a\nb";
            const int memfd = ::memfd_create("lie", MFD_CLOEXEC | MFD_ALLOW_SEALING);
            if (memfd < 0 ||
                ::write(memfd, two.data(), two.size()) != static_cast<ssize_t>(two.size())) {
                return 1;
            }
            ::fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE);
            protocol::TextResponse response;
            response.lines = 3;
            response.bytes = static_cast<std::uint32_t>(two.size());
            const std::array<int, 1> fds{memfd};
            static_cast<void>(protocol::send_packet(protocol::kSocketFd,
                                                    std::as_bytes(std::span{&response, 1}), fds));
            ::close(memfd);
            continue;
        }
        // big-lie: 4 bytes of pixels for a 1000x1000 picture.
        protocol::Response response;
        response.width = 1000;
        response.height = 1000;
        off_t bytes = 4;
        if (mode == "pixel-overflow") {
            // width * height = 2^62 + 1: multiplying by four wraps to four bytes.
            response.width = 2147418113U;
            response.height = 2147549185U;
        } else if (mode == "wide" || mode == "tall" || mode == "fits") {
            response.width = mode == "wide" ? 129 : 64;
            response.height = mode == "tall" ? 129 : 32;
            bytes = static_cast<off_t>(response.width) * response.height * 4;
        } else if (mode == "zero-width" || mode == "zero-height") {
            response.width = mode == "zero-width" ? 0 : 1;
            response.height = mode == "zero-height" ? 0 : 1;
            bytes = 0;
        }
        const int memfd = ::memfd_create("lie", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (memfd < 0 || ::ftruncate(memfd, bytes) != 0) {
            return 1;
        }
        ::fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW);
        const std::array<int, 1> fds{memfd};
        static_cast<void>(protocol::send_packet(protocol::kSocketFd,
                                                std::as_bytes(std::span{&response, 1}), fds));
        ::close(memfd);
    }
}
