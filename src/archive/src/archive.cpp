#include "rummage/archive/archive.hpp"

#include <mutex>
#include <utility>

namespace rummage::archive {

std::string_view format_name(Format format) {
    switch (format) {
        case Format::zip:
            return "zip";
        case Format::tar:
            return "tar";
        case Format::tar_gzip:
            return "tar.gz";
        case Format::gzip:
            return "gzip";
        case Format::unknown:
            break;
    }
    return "unknown";
}

namespace {

std::mutex engine_mutex;
Lister current_lister;          // guarded by engine_mutex
EntryReader current_reader;     // guarded by engine_mutex
SequenceOpener current_opener;  // guarded by engine_mutex

constexpr const char* kNoEngine = "no engine reads archives";

}  // namespace

void set_lister(Lister lister) {
    const std::scoped_lock lock{engine_mutex};
    current_lister = std::move(lister);
}

void set_entry_reader(EntryReader reader) {
    const std::scoped_lock lock{engine_mutex};
    current_reader = std::move(reader);
}

Listing list_archive(const std::filesystem::path& archive, const Limits& limits) {
    Lister lister;
    {
        const std::scoped_lock lock{engine_mutex};
        lister = current_lister;
    }
    if (!lister) {
        return {.format = Format::unknown,
                .failure = Failure::unreadable,
                .detail = kNoEngine,
                .entries = {}};
    }
    return lister(archive, limits);
}

Read read_archive_entry(const std::filesystem::path& archive, Format format, const Entry& entry,
                        int out, std::uint64_t max_bytes) {
    EntryReader reader;
    {
        const std::scoped_lock lock{engine_mutex};
        reader = current_reader;
    }
    if (!reader) {
        return {.failure = Failure::unreadable, .detail = kNoEngine, .bytes = 0};
    }
    return reader(archive, format, entry, out, max_bytes);
}

namespace {

// Without an engine's sequence: each read on its own.
class OneByOne final : public EntrySequence {
public:
    OneByOne(std::filesystem::path archive, Format format)
        : archive_{std::move(archive)}, format_{format} {}

    Read read(const Entry& entry, int out, std::uint64_t max_bytes) override {
        return read_archive_entry(archive_, format_, entry, out, max_bytes);
    }

private:
    std::filesystem::path archive_;
    Format format_;
};

}  // namespace

void set_sequence_opener(SequenceOpener opener) {
    const std::scoped_lock lock{engine_mutex};
    current_opener = std::move(opener);
}

std::unique_ptr<EntrySequence> open_entry_sequence(const std::filesystem::path& archive,
                                                   Format format, std::function<bool()> stopped) {
    SequenceOpener opener;
    {
        const std::scoped_lock lock{engine_mutex};
        opener = current_opener;
    }
    if (opener) {
        if (std::unique_ptr<EntrySequence> sequence = opener(archive, format, std::move(stopped))) {
            return sequence;
        }
    }
    return std::make_unique<OneByOne>(archive, format);
}

}  // namespace rummage::archive
