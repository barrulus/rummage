// This process receives only stereo 48 kHz signed 16-bit PCM. No media parser or pathname.
// It owns the PulseAudio connection (including PipeWire's PulseAudio compatibility server).
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <pulse/simple.h>
#include <unistd.h>

int main() {
    const pa_sample_spec spec{.format = PA_SAMPLE_S16LE, .rate = 48000, .channels = 2};
    const pa_buffer_attr buffer{
        .maxlength = 19200, .tlength = 9600, .prebuf = 0, .minreq = 1920, .fragsize = 0};
    int error = 0;
    pa_simple* audio = pa_simple_new(nullptr, "Rummage", PA_STREAM_PLAYBACK, nullptr, "Quick Look",
                                     &spec, nullptr, &buffer, &error);
    if (!audio)
        return 1;
    std::array<std::uint8_t, 9600> bytes{};
    std::size_t pending = 0;
    while (true) {
        const auto got = ::read(0, bytes.data() + pending, bytes.size() - pending);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            break;
        const auto total = pending + static_cast<std::size_t>(got);
        const auto complete =
            total - (total % 4);  // one stereo s16 frame; SOCK_STREAM can split it
        if (complete > 0 && pa_simple_write(audio, bytes.data(), complete, &error) < 0) {
            pa_simple_free(audio);
            return 1;
        }
        pending = total - complete;
        if (pending)
            std::memmove(bytes.data(), bytes.data() + complete, pending);
    }
    // Only normal EOF closes the write side. Pause/change/close kill this process instead.
    // PulseAudio's synchronous drain can remain pending after fragmented input underruns.
    // Allow the reported queued audio to play, with a fixed cap; the owner can still kill us
    // immediately on pause/seek/close and also imposes its own EOF deadline.
    const pa_usec_t latency = pa_simple_get_latency(audio, &error);
    const bool complete = pending == 0 && latency != static_cast<pa_usec_t>(-1);
    if (complete)
        ::usleep(static_cast<useconds_t>(std::min<pa_usec_t>(latency, 400000)));
    pa_simple_free(audio);
    return complete ? 0 : 1;
}
