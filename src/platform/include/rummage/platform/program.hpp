#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>

// Whether a file is a program Rummage may start by itself (issue #17): an ELF executable, an
// AppImage included. Scripts are not: a downloaded ".sh" looks like a document, so it opens as
// one. Blocking (reads the file's first bytes): call it from a worker.
namespace rummage::platform {

struct ProgramCheck {
    bool program{false};     // an ELF executable (not a shared library, not an object file)
    bool executable{false};  // the user may run it (access X_OK)
    bool noexec{false};      // its file system is mounted noexec: the kernel would refuse
};

// What path is; a file that cannot be read is not a program.
[[nodiscard]] ProgramCheck check_program(const std::filesystem::path& path);

// For tests: the decision from a file's first bytes and its program headers, read through
// read(offset, out) (false when out cannot be filled). An ELF of type EXEC is a program; one of
// type DYN is a program when it asks for an interpreter (PT_INTERP: a position-independent
// executable) or carries the AppImage magic (its runtime is a static PIE, with no interpreter),
// not otherwise (a shared library).
[[nodiscard]] bool is_elf_program(
    const std::function<bool(std::uint64_t offset, std::span<unsigned char> out)>& read);

}  // namespace rummage::platform
