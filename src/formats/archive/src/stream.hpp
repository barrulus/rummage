#pragma once

// What the archive readers read from and write to: a file through pread, a sequential stream
// (a window of the file, or what inflating it gives), and a sink (a descriptor or memory) that
// refuses more than it was allowed. A problem is thrown as Problem and caught where a listing or
// a read is answered.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "rummage/archive/archive.hpp"

namespace rummage::formats::archive::detail {

struct Problem {
    rummage::archive::Failure failure;
    std::string detail;
};

[[noreturn]] void damaged(std::string detail);

class File {
public:
    // fd stays the caller's. Throws unreadable when fstat fails.
    explicit File(int fd);

    [[nodiscard]] std::uint64_t size() const { return size_; }

    // Up to to.size() bytes from offset (fewer at the end); throws unreadable on an error.
    [[nodiscard]] std::size_t read_at(std::uint64_t offset, std::span<std::uint8_t> to) const;

    // Exactly to.size() bytes from offset, or throws damaged (what: what was being read).
    void read_exactly(std::uint64_t offset, std::span<std::uint8_t> to, const char* what) const;

private:
    int fd_;
    std::uint64_t size_{0};
};

class Stream {
public:
    Stream() = default;
    virtual ~Stream() = default;
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;
    Stream(Stream&&) = delete;
    Stream& operator=(Stream&&) = delete;

    // Up to to.size() bytes; 0 only at the end.
    virtual std::size_t read(std::span<std::uint8_t> to) = 0;

    // Passes over count bytes; false when the stream ends first.
    virtual bool skip(std::uint64_t count);

    // Exactly to.size() bytes, or false at the end (what was read is in to).
    bool read_exactly(std::span<std::uint8_t> to);

    // Bytes given (or skipped) so far.
    [[nodiscard]] std::uint64_t offset() const { return offset_; }

protected:
    std::uint64_t offset_{0};
};

// The bytes of file from begin to end.
class FileStream final : public Stream {
public:
    FileStream(const File& file, std::uint64_t begin, std::uint64_t end);
    std::size_t read(std::span<std::uint8_t> to) override;
    bool skip(std::uint64_t count) override;

private:
    const File& file_;
    std::uint64_t at_;
    std::uint64_t end_;
};

// What inflating the bytes of file from begin to end gives: raw deflate (zip), or gzip members
// one after the other (bytes after the last member that do not start another are left alone).
// A broken stream, or a member whose checksum or length does not match, throws damaged.
class Inflater final : public Stream {
public:
    enum class Wrapping : std::uint8_t { raw, gzip };

    Inflater(const File& file, std::uint64_t begin, std::uint64_t end, Wrapping wrapping);
    ~Inflater() override;
    Inflater(const Inflater&) = delete;
    Inflater& operator=(const Inflater&) = delete;
    Inflater(Inflater&&) = delete;
    Inflater& operator=(Inflater&&) = delete;

    std::size_t read(std::span<std::uint8_t> to) override;

private:
    struct State;
    std::unique_ptr<State> state_;
};

class Sink {
public:
    explicit Sink(std::uint64_t max_bytes) : max_{max_bytes} {}

    virtual ~Sink() = default;
    Sink(const Sink&) = delete;
    Sink& operator=(const Sink&) = delete;
    Sink(Sink&&) = delete;
    Sink& operator=(Sink&&) = delete;

    // Throws too_large past max_bytes, unreadable when the bytes cannot be written.
    void write(std::span<const std::uint8_t> bytes);

    [[nodiscard]] std::uint64_t written() const { return written_; }

protected:
    virtual void put(std::span<const std::uint8_t> bytes) = 0;

private:
    std::uint64_t max_;
    std::uint64_t written_{0};
};

class FdSink final : public Sink {
public:
    FdSink(int fd, std::uint64_t max_bytes) : Sink{max_bytes}, fd_{fd} {}

private:
    void put(std::span<const std::uint8_t> bytes) override;
    int fd_;
};

class MemorySink final : public Sink {
public:
    explicit MemorySink(std::uint64_t max_bytes) : Sink{max_bytes} {}

    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const { return bytes_; }

private:
    void put(std::span<const std::uint8_t> bytes) override;
    std::vector<std::uint8_t> bytes_;
};

// Copies count bytes of from into to (with crc32 of them in crc when not null); throws damaged
// when from ends first.
void copy(Stream& from, std::uint64_t count, Sink& to, std::uint32_t* crc = nullptr);

// Everything from gives, into to (with crc32 as above).
void copy_all(Stream& from, Sink& to, std::uint32_t* crc = nullptr);

// Little-endian numbers from bytes (the caller has checked there are enough).
[[nodiscard]] std::uint16_t le16(std::span<const std::uint8_t> bytes, std::size_t at);
[[nodiscard]] std::uint32_t le32(std::span<const std::uint8_t> bytes, std::size_t at);
[[nodiscard]] std::uint64_t le64(std::span<const std::uint8_t> bytes, std::size_t at);

// Seconds since 1970 of a civil date and time (UTC).
[[nodiscard]] std::int64_t epoch_seconds(int year, int month, int day, int hour, int minute,
                                         int second);

}  // namespace rummage::formats::archive::detail
