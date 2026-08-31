#pragma once

#include <leah/types.hpp>

// Kernel objects, and the handles processes hold them by.
//
// This is the beginning of capability-based security here, and the thing it
// replaces is ambient authority: the property that a process can reach a
// resource because of *who it is* rather than because of *what it was given*.
// Under ambient authority every check has to be made again at every use, by
// whoever happens to own the resource, against an identity that says nothing
// about intent. Under capabilities the check is made once, when the handle is
// issued, and holding the handle is the answer.
//
// A handle is an index into a table this process does not own and cannot
// write. That is the whole of "unforgeable": there is no bit pattern a process
// can invent that names an object it was not given, because the naming happens
// on the other side of the boundary. Rights ride along with the handle and can
// only ever be narrowed, so a process can hand out less authority than it
// holds and never more.
//
// The shape is NT's - a typed object with an access mask - because that is what
// lets a handle say things a byte stream cannot: this one may be waited on but
// not signalled, mapped but not written. Naming will be the other tradition's
// job: a server exports a tree, you walk a path, and what you get back is one
// of these.
//
// Note what already existed and is being renamed rather than invented. Port
// I/O permission is a capability. A shm id is a slot and a generation, which
// is this encoding. authd owning /etc/shadow so that no program needs setuid
// is the capability answer to privilege, arrived at years before the word.

namespace object {

enum class Type : u32 {
    None = 0,
    Port,           // a named IPC port, server side
    Shm,            // a shared memory segment
    Image,          // a held program image
    Pty,            // one end of a pseudo-terminal
    Pipe,
    Process,
    Thread,
    Event,
};

// What a handle permits. A right that is not in the mask is not a permission
// that can be recovered: duplicate may narrow and never widen, which is what
// makes it safe to hand a handle to something less trusted than yourself.
enum Rights : u32 {
    kRead      = 1u << 0,
    kWrite     = 1u << 1,
    kExecute   = 1u << 2,   // may be exec'd - see the note below
    kMap       = 1u << 3,
    kSignal    = 1u << 4,
    kWait      = 1u << 5,
    kDuplicate = 1u << 6,   // may be copied at all
    kTransfer  = 1u << 7,   // may be sent to another process
    kDestroy   = 1u << 8,
    kAll       = 0x1FFu,
};

// kExecute is the one that pays for this immediately. Today nothing can
// enforce the execute bit on a program: execve is handed bytes and never
// learns which file they came from, so the only place to check is libc, which
// is the process checking itself. With this, the filesystem server issues an
// image handle carrying kExecute only to a caller who may have it, and execve
// takes the handle rather than a pointer. The check moves to the one side that
// both knows the answer and cannot be talked out of it.

constexpr usize kMaxHandles = 128;

// A handle is a slot and a generation, not a slot.
//
// The slot is recycled the moment a handle is closed, and the next open hands
// it straight back out - so anything still holding the old number is talking
// about somebody else's object with nothing anywhere to say so. The generation
// turns that silence into a refusal. shm learned this the hard way; see the
// same encoding there.
using Handle = i32;
constexpr Handle kNoHandle = -1;

void init();

// Give this process a handle on `pointer`. Returns kNoHandle if its table is
// full.
Handle give(u32 pid, Type type, void* pointer, u32 rights);

// Look one up. Returns nullptr unless the handle names a live object of
// exactly `type` and carries every right in `needed`.
void* look(u32 pid, Handle handle, Type type, u32 needed);

// The rights on a handle, or 0.
u32 rights_of(u32 pid, Handle handle);
Type type_of(u32 pid, Handle handle);

// A second handle on the same object, with rights narrowed by `mask`. Fails if
// the original does not carry kDuplicate.
Handle duplicate(u32 pid, Handle handle, u32 mask);

bool close(u32 pid, Handle handle);

// Everything this process held, dropped. Called when it exits, and by execve
// for the handles that do not survive it.
void close_all(u32 pid);

// The child of a fork holds what its parent held. Handles are copied, not
// shared: closing one in the child must not close the parent's.
void inherit(u32 from_pid, u32 to_pid);

} // namespace object
