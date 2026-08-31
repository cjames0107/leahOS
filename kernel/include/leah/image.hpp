#pragma once

#include <leah/types.hpp>

// Program images, held once and mapped by everyone.
//
// The problem this solves is that exec used to be handed a *blob*. Whoever
// wanted to run a program read it, and the kernel copied those bytes into
// freshly allocated frames - so a hundred processes running against the same
// libc had a hundred private copies of it, and every exec paid to read and
// copy the whole thing again. Shared libraries had cut the size on disk and
// not one byte in memory, which is the half people actually mean.
//
// An image is that missing piece: a file's contents, held as physical frames,
// mapped read-only into every address space that wants them. It is the same
// object other systems call a memory object or a VMO, arrived at from the
// other direction - shm already had "frames more than one space can map", and
// this is that with a name and a version on it.
//
// The kernel still cannot read a file. It is handed the bytes, once, by a
// process that has a filesystem, exactly as before; what is new is that it
// keeps them. `name` is an opaque string used to recognise the same image
// again - ring 0 cannot resolve it, open it, or find out whether it exists.
// `version` is whatever the caller uses to mean "still the same file"; libc
// passes the size and modification time, so an image whose file has changed is
// simply not found and a new one is made.
//
// Lifetime rides on the frame reference counts copy-on-write already needed.
// The image holds one reference per frame and every mapping takes another, so
// evicting an image that something is still running is safe: the slot goes,
// the frames stay until the last mapping does.

namespace image {

// Thirty-two images, which is the working set of a running desktop several
// times over: the shell, the terminal, libc, ld.so, and whatever is being run
// at the moment. Past that the least recently used one is evicted, which costs
// the next exec of that program a read and nothing else.
constexpr usize kMaxImages = 32;
constexpr usize kNameMax   = 72;
constexpr u64   kMaxBytes  = 8ull * 1024 * 1024;

void init();

// An image is named to the rest of the kernel by an opaque pointer, and to a
// process by a handle on one - which is what lets the right to execute a
// program be something a process is given rather than something it assumes.
// Nothing outside this file may look inside the pointer.

// The image called `name` at `version`, or nullptr. A name that is known at a
// different version is a different image and is not returned.
void* find(const char* name, u64 version);
void* find_locked(const char* name, u64 version);

// Keep a copy of `bytes` under `name`. Returns the image, or nullptr if it
// will not fit. `bytes` is in the caller's address space and is copied here.
void* create(const char* name, u64 version, const u8* bytes, u64 size);

bool valid(void* image);
u64  size_of(void* image);

// The frame holding byte `offset`, which must be page aligned. Returns 0 past
// the end. Does not take a reference - see share_frame.
paddr_t frame_at(void* image, u64 offset);
paddr_t frame_at_locked(void* image, u64 offset);

// Take a reference on that frame, for a mapping about to be made of it.
bool share_frame(void* image, u64 offset);

// Copy out of an image, for the parts that cannot be shared: a writable
// segment needs its own copy, and the loader has to read program headers.
bool read(void* image, u64 offset, void* into, u64 bytes);

} // namespace image
