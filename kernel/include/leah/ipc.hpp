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

// How many capabilities one message may carry. Two, because a reply that
// hands back more than a couple of objects is a reply that wants a different
// shape - and every slot costs on every message whether or not it is used.
constexpr usize kMaxCarried = 2;

struct Message {
    u32 tag;                    // what this message is; the server defines them
    u32 bytes;                  // how much of `data` means anything
    i64 word[4];                // small arguments, and small answers
    i32 shm_key;                // bulk payload, or 0 for none
    u32 shm_bytes;
    // Capabilities travelling with this message.
    //
    // A handle number means nothing outside the table it came from, so these
    // are not copied across: the kernel resolves each one in the sender's
    // table and installs the object it names in the receiver's, writing the
    // receiver's own numbers back into the message it is handed. A sender can
    // pass no right it does not hold, which is what makes this safe to do
    // between processes that do not trust each other.
    i32 handle[kMaxCarried];
    u32 handles;                // how many of them mean anything
    char data[kInlineBytes];
};

// Well-known port names. A server claims one; a client asks for it by number.
// Numbers rather than strings because the set is small, fixed, and belongs to
// the system rather than to whoever happens to start first.
constexpr u32 kPortNet   = 1;
constexpr u32 kPortVfs   = 2;
constexpr u32 kPortBlock = 3;
constexpr u32 kPortBlock2 = 9;   // the AHCI driver's, when there is one
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
// Wait for a request. `deadline_ticks` of zero waits forever; otherwise the
// call returns -2 when that many ticks have passed with nothing arriving.
//
// A server that can only block forever cannot do anything on its own - it has
// no clock, so periodic work has to become somebody else's process. This is
// what lets a server keep its own time.
i64 recv(i32 port, Message* out, u32* caller_pid, u64 deadline_ticks = 0);

// The same, but returns -1 immediately when nothing is waiting. A server that
// also owns hardware cannot block forever on its port: it has a card to drain
// as well, and the two have to be looked at in the same loop.
i64 try_recv(i32 port, Message* out, u32* caller_pid);
i64 reply(i32 handle, const Message* msg);

// Client side. open finds a port by name; call sends and blocks for the answer.
i64 port_open(u32 name);
// A deadline of 0 waits as long as it takes, which is what every caller did
// before there was a choice. Anything else returns -2 when the time runs out.
//
// A client with no deadline is a client that trusts a server completely: one
// that never answers stops the caller forever, and a caller stopped inside a
// server that others depend on stops them too. That is how a single wedged
// driver became a wedged machine, with nothing in the log to say so.
//
// 0 on success. -2 when the deadline passes, -3 when a signal arrives, -1 when
// the server is gone or there was never one. The three are separate because
// only the first two leave anything worth retrying.
i64 call(i32 port, const Message* request, Message* reply_out,
         u64 deadline_ticks = 0);

// Every request a dying task was waiting on, and every request it had accepted
// and not answered, has to be unstuck - otherwise a server that crashes takes
// its clients with it, blocked forever on a reply that is never coming.
void abandon(u32 pid);

} // namespace ipc
