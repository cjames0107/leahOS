#pragma once

#include <leah/types.hpp>

// Shared memory between processes.
//
// A segment is a set of physical frames that more than one address space can
// map at once - the first thing in this kernel that two processes can both
// write to. Everything shared until now was shared by accident of a fork and
// then copied apart on the first write; this is the opposite, and is what a
// window server outside the kernel needs in order to hand a client its pixels.
//
// Segments are named by an integer key rather than by a path, because there is
// no /dev or /tmp to hang a name off yet and a well-known number is enough for
// two processes to find each other. Opening the same key twice returns the same
// segment.
//
// Lifetime rides on the frame reference counts that copy-on-write already
// needed: the segment holds one reference per frame, and every mapping takes
// another. A process that exits - or is killed - drops its references through
// the ordinary address-space teardown, so nothing here has to be told about it.

namespace shm {

/* Thirty-two was chosen when a segment meant a window. It now also means "a
 * process that has touched a file", because that is how libc reaches vfsd, and
 * a desktop with a dozen servers, several windows and a handful of short-lived
 * programs runs out during ordinary use - at which point every later open
 * fails and the machine stops being able to read anything. */
constexpr usize kMaxSegments = 512;

// The keys the window server and its clients agree on.
constexpr u32 kWindowServerKey = 1;         // the control block
constexpr u32 kWindowPixelKeyBase = 0x1000; // + slot, one per window

void init();

// Creation flags.
enum Flags : u32 {
    None   = 0,
    // Readable and writable by every user, not just the creator and root. A
    // deliberate choice by whoever creates the segment, because it gives up the
    // protection the default provides - a server's rendezvous block needs it,
    // the pixels behind a window do not.
    Public = 1u << 0,
};

// Open the segment named `key`, creating it with `bytes` if it does not exist.
// An existing segment is returned as it is and `bytes` is ignored. Returns the
// segment id, or -1.
i32 open(u32 key, u64 bytes, u32 uid, u32 flags);

// Whether `uid` may map this segment.
bool accessible(i32 id, u32 uid);

// Drop the segment's own reference and free the slot, so the key can be used
// again. Mappings that already exist hold their own references and keep the
// frames alive, so this is safe while the other side is still reading.
bool destroy(i32 id, u32 uid);

// Give back every segment a dying process created. Without this a segment
// outlives its owner forever, and the table is small.
void abandon(u32 pid);

// Physical frame `index` of a segment, for the syscall layer to map.
paddr_t frame_of(i32 id, usize index);

u64   size_of(i32 id);
usize page_count(i32 id);

// Who created it. A segment is readable and writable by its creator and by
// root, which is the same rule the rest of the system uses for owned things.
u32 owner_uid_of(i32 id);
bool exists(i32 id);

// Take another reference to every frame, because the caller is about to map
// them into an address space that will later drop one per frame on teardown.
bool share_frames(i32 id);

} // namespace shm
