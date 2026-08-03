#pragma once

#include <leah/types.hpp>

// Synchronous message passing between address spaces.
//
// This is the primitive the rest of the system is being rebuilt onto. Today the
// filesystem calls the disk driver with a function call, because both are
// compiled into one binary; the point of what follows is that it should be able
// to call it when the two are separate processes that share no memory.
//
// Synchronous request/reply rather than asynchronous queues, because every use
// this system has is a question with an answer: read this block, resolve this
// path, send this frame. An asynchronous primitive would mean every caller
// inventing its own way of waiting for the reply, and every one of them getting
// it slightly wrong.
//
// Bulk data does not travel in the message. A message carries a shared-memory
// key instead, and the two ends map the same pages - copying a disk block twice
// through the kernel to move it between two processes would make a microkernel
// exactly as slow as its reputation says.

namespace ipc {

// Big enough for a path and its arguments, which is what almost every message
// in this system is. Anything larger is what the shared segment is for.
constexpr usize kInlineBytes = 256;

struct Message {
    u32 tag;                    // what this message is; the server defines them
    u32 bytes;                  // how much of `data` means anything
    i64 word[4];                // small arguments, and small answers
    i32 shm_key;                // bulk payload, or 0 for none
    u32 shm_bytes;
    char data[kInlineBytes];
};

// Well-known port names. A server claims one; a client asks for it by number.
// Numbers rather than strings because the set is small, fixed, and belongs to
// the system rather than to whoever happens to start first.
constexpr u32 kPortNet   = 1;
constexpr u32 kPortVfs   = 2;
constexpr u32 kPortBlock = 3;
constexpr u32 kPortNic   = 4;   // the raw Ethernet device
constexpr u32 kPortAudio = 5;
constexpr u32 kPortAuth  = 6;   // the account database
constexpr u32 kPortUsb   = 7;   // the USB host controller
constexpr u32 kPortPs2   = 8;   // the 8042 and what hangs off it

void init();

// Server side. create claims a name; recv blocks until a request arrives and
// returns a handle to answer with; reply answers it and unblocks the caller.
i64 port_create(u32 name);
i64 port_destroy(i32 port);
i64 recv(i32 port, Message* out, u32* caller_pid);

// The same, but returns -1 immediately when nothing is waiting. A server that
// also owns hardware cannot block forever on its port: it has a card to drain
// as well, and the two have to be looked at in the same loop.
i64 try_recv(i32 port, Message* out, u32* caller_pid);
i64 reply(i32 handle, const Message* msg);

// Client side. open finds a port by name; call sends and blocks for the answer.
i64 port_open(u32 name);
i64 call(i32 port, const Message* request, Message* reply_out);

// Every request a dying task was waiting on, and every request it had accepted
// and not answered, has to be unstuck - otherwise a server that crashes takes
// its clients with it, blocked forever on a reply that is never coming.
void abandon(u32 pid);

} // namespace ipc
