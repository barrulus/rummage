#pragma once

// The archive family (ROADMAP 0.9.0): zip, tar, tar.gz and single .gz files, read through the
// descriptor they were opened as. Linked into the isolated helper (and tests and development
// tools), never into rummage. Every number an archive gives is checked before it is used; an
// entry never yields more bytes than it says it holds.

#include <cstdint>
#include <memory>

#include "rummage/archive/archive.hpp"

namespace rummage::formats::archive {

// The archive's format and entries, in the archive's order. A damaged archive keeps the entries
// read before the damage, with the failure saying what is wrong.
[[nodiscard]] rummage::archive::Listing list(int fd, const rummage::archive::Limits& limits);

// Writes the bytes of entry (as list gave it, in an archive of format) to out, at most
// max_bytes of them: more is a too_large failure. fd and out stay the caller's.
[[nodiscard]] rummage::archive::Read read_entry(int fd, rummage::archive::Format format,
                                                const rummage::archive::Entry& entry, int out,
                                                std::uint64_t max_bytes);

// Reads of one archive's entries in a row (extracting): a tar.gz goes on inflating from where the
// last read stopped when the next entry comes after it, where read_entry inflates it from its
// start each time. The archive may come as a new descriptor each time: it is the same archive
// while its device, inode, size and modification time are; a copy of the descriptor is kept until
// forget() or another archive.
class Sequence {
public:
    Sequence();
    ~Sequence();
    Sequence(const Sequence&) = delete;
    Sequence& operator=(const Sequence&) = delete;
    Sequence(Sequence&&) = delete;
    Sequence& operator=(Sequence&&) = delete;

    // As read_entry.
    [[nodiscard]] rummage::archive::Read read(int fd, rummage::archive::Format format,
                                              const rummage::archive::Entry& entry, int out,
                                              std::uint64_t max_bytes);

    // Lets the archive go (its descriptor and the inflater's memory).
    void forget();

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace rummage::formats::archive
