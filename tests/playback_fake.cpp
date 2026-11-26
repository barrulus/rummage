#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <span>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

#include "playback_protocol.hpp"
#include "protocol.hpp"

int main(int argc, char** argv) {
    namespace wire = rummage::isolate::playback_wire;
    namespace protocol = rummage::isolate::protocol;
    if (argc < 1)
        return 1;
    const auto mode = std::filesystem::path{argv[0]}.filename();
    if (mode == "hang-audio") {
        while (true)
            ::pause();
    }
    if (mode == "slow-audio") {
        // A real sound server may not consume PCM immediately after the stream opens.
        std::this_thread::sleep_for(std::chrono::seconds{2});
        std::ofstream capture{std::string{argv[0]} + ".pcm", std::ios::binary};
        std::array<char, 4096> bytes{};
        ssize_t count = 0;
        while ((count = ::read(0, bytes.data(), bytes.size())) > 0)
            capture.write(bytes.data(), count);
        return capture ? 0 : 1;
    }
    auto packet = protocol::receive_packet(3);
    if (!packet)
        return 1;
    packet->close_fds();
    if (mode == "hang") {
        while (true)
            ::pause();
    }
    wire::Response response;
    int payload = ::memfd_create("fake", MFD_CLOEXEC);
    if (mode == "oversized")
        response.width = 0xffffffff;
    if (mode == "unknown")
        response.status = 0xffffffff;
    if (mode == "unsealed")
        response.width = response.height = 0;
    const std::array<int, 1> fds{payload};
    static_cast<void>(protocol::send_packet(3, std::as_bytes(std::span{&response, 1}), fds));
    ::close(payload);
}
