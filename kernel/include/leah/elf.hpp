#pragma once

#include <leah/types.hpp>

// ELF64 loading.
//
// This is the kernel-side loader that will eventually back execve(). It reads
// through the VFS rather than off a raw device, so anything the filesystem can
// reach is executable.

namespace elf {

struct Image {
    vaddr_t entry;
    vaddr_t lowest;      // lowest mapped address
    vaddr_t highest;     // one past the highest
    u32     segments;
};

enum class Error : u8 {
    None,
    NotFound,
    TooSmall,
    NotElf,
    WrongClass,        // not 64-bit, or not little endian
    WrongMachine,      // not x86-64
    WrongType,         // not an executable
    BadProgramHeader,
    OutOfMemory,
    ReadFailed,
};

const char* error_name(Error error);

// Maps every PT_LOAD segment into the current address space at its own
// p_vaddr. Returns Error::None on success.
Error load(const char* path, Image& out);

} // namespace elf
