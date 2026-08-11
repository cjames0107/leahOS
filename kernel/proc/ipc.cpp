#include <leah/cpu.hpp>
#include <leah/ipc.hpp>
#include <leah/scheduler.hpp>
#include <leah/timer.hpp>
#include <leah/string.hpp>

namespace ipc {
namespace {

constexpr usize kMaxPorts    = 16;
constexpr usize kMaxRequests = 64;

// A request in flight. It holds both halves of the exchange, because the
// caller's copy of the message is in an address space the server cannot see and
// the reply is going the other way through the same gap.
enum class State : u8 {
    Free,
    Queued,         // waiting for a server to take it
    Taken,          // a server has it and owes an answer
    Answered,       // the answer is here; the caller has not collected it yet
    Abandoned,      // the other end died; collect an error and move on
};

struct Request {
    State   state;
    i32     port;
    u32     caller_pid;
    u32     server_pid;
    Message body;       // the request on the way out, the reply on the way back
};

struct Port {
    bool used;
    u32  name;
    u32  owner_pid;
};

Port    g_ports[kMaxPorts];
Request g_requests[kMaxRequests];

// Channels to sleep on. Distinct address ranges, so a waiter for a port cannot
// be woken by a reply and the other way round.
u64 port_channel(i32 port)      { return reinterpret_cast<u64>(&g_ports[port]); }
u64 request_channel(i32 handle) { return reinterpret_cast<u64>(&g_requests[handle]); }

i32 find_port(u32 name)
{
    for (usize i = 0; i < kMaxPorts; ++i)
        if (g_ports[i].used && g_ports[i].name == name)
            return static_cast<i32>(i);
    return -1;
}

bool valid_port(i32 port)
{
    return port >= 0 && port < static_cast<i32>(kMaxPorts) && g_ports[port].used;
}

} // namespace

void init()
{
    memset(g_ports, 0, sizeof(g_ports));
    memset(g_requests, 0, sizeof(g_requests));
}

i64 port_create(u32 name)
{
    if (name == 0 || find_port(name) >= 0)
        return -1;              // taken: one server per name, first one wins
    for (usize i = 0; i < kMaxPorts; ++i) {
        if (g_ports[i].used)
            continue;
        g_ports[i].used = true;
        g_ports[i].name = name;
        g_ports[i].owner_pid = scheduler::current_pid();
        return static_cast<i64>(i);
    }
    return -1;
}

i64 port_open(u32 name)
{
    return find_port(name);
}

i64 port_destroy(i32 port)
{
    if (!valid_port(port) || g_ports[port].owner_pid != scheduler::current_pid())
        return -1;
    g_ports[port].used = false;
    // Anything waiting on this port will never be answered now.
    for (usize i = 0; i < kMaxRequests; ++i) {
        if (g_requests[i].port != port || g_requests[i].state == State::Free)
            continue;
        g_requests[i].state = State::Abandoned;
        scheduler::wake(request_channel(static_cast<i32>(i)));
    }
    return 0;
}

i64 call(i32 port, const Message* request, Message* reply_out)
{
    if (!valid_port(port) || request == nullptr || reply_out == nullptr)
        return -1;

    i32 handle = -1;
    for (usize i = 0; i < kMaxRequests; ++i) {
        if (g_requests[i].state == State::Free) {
            handle = static_cast<i32>(i);
            break;
        }
    }
    if (handle < 0)
        return -1;              // the system is saturated; fail rather than wait

    Request& r = g_requests[handle];
    r.state      = State::Queued;
    r.port       = port;
    r.caller_pid = scheduler::current_pid();
    r.server_pid = 0;
    memcpy(&r.body, request, sizeof(Message));

    // Wake a server that is waiting, then sleep until it answers. The order
    // matters and the interrupt state is why: syscalls are entered with
    // interrupts masked, so nothing can slip between marking this queued and
    // going to sleep on it.
    scheduler::wake(port_channel(port));
    while (r.state != State::Answered && r.state != State::Abandoned) {
        scheduler::block_on(request_channel(handle));
        if (scheduler::signal_pending()) {
            // Abandoned rather than left queued: the server may still answer,
            // and it must not write into a slot this caller has stopped
            // watching. Freeing it here would let the next caller take the
            // same slot and receive the previous one's reply.
            r.state = State::Abandoned;
            return -1;
        }
    }

    const bool ok = r.state == State::Answered;
    if (ok)
        memcpy(reply_out, &r.body, sizeof(Message));
    r.state = State::Free;
    return ok ? 0 : -1;
}

// The shared body of recv and try_recv: take a queued request if there is one.
static i64 take(i32 port, Message* out, u32* caller_pid)
{
    for (usize i = 0; i < kMaxRequests; ++i) {
        Request& r = g_requests[i];
        if (r.state != State::Queued || r.port != port)
            continue;
        r.state = State::Taken;
        r.server_pid = scheduler::current_pid();
        memcpy(out, &r.body, sizeof(Message));
        if (caller_pid != nullptr)
            *caller_pid = r.caller_pid;
        return static_cast<i64>(i);
    }
    return -1;
}

i64 try_recv(i32 port, Message* out, u32* caller_pid)
{
    if (!valid_port(port) || out == nullptr ||
        g_ports[port].owner_pid != scheduler::current_pid())
        return -1;
    return take(port, out, caller_pid);
}

i64 recv(i32 port, Message* out, u32* caller_pid, u64 deadline_ticks)
{
    if (!valid_port(port) || out == nullptr)
        return -1;
    if (g_ports[port].owner_pid != scheduler::current_pid())
        return -1;              // only the server may take from its own port

    for (;;) {
        const i64 got = take(port, out, caller_pid);
        if (got >= 0)
            return got;
        // Nothing waiting. The port is the channel, so a request arriving wakes
        // exactly the server that can serve it.
        if (deadline_ticks == 0) {
            scheduler::block_on(port_channel(port));
            // A signal wakes this, and until now that was all it did: nothing
            // had arrived, so the loop went round and slept again. The signal
            // stayed pending and the process stayed alive - which is why a
            // server sitting here survived a terminate and survived a kill.
            // Returning hands control back to the syscall exit, where signals
            // are actually delivered.
            if (scheduler::signal_pending())
                return -3;
        } else {
            if (timer::ticks() >= deadline_ticks)
                return -2;      // the wait ran out; the caller has work to do
            scheduler::block_on_until(port_channel(port), deadline_ticks);
            if (!valid_port(port))
                return -1;
            // Take before deciding the wait expired, and *return* what was
            // taken. Testing the clock first would throw away a request that
            // arrived in the same instant the deadline passed, and taking
            // without returning would drop one outright.
            const i64 after = take(port, out, caller_pid);
            if (after >= 0)
                return after;
            if (timer::ticks() >= deadline_ticks)
                return -2;
            if (scheduler::signal_pending())
                return -3;
        }
        if (!valid_port(port))
            return -1;          // the port went away while we slept
    }
}

i64 reply(i32 handle, const Message* msg)
{
    if (handle < 0 || handle >= static_cast<i32>(kMaxRequests) || msg == nullptr)
        return -1;
    Request& r = g_requests[handle];
    if (r.state != State::Taken || r.server_pid != scheduler::current_pid())
        return -1;
    memcpy(&r.body, msg, sizeof(Message));
    r.state = State::Answered;
    scheduler::wake(request_channel(handle));
    return 0;
}

void abandon(u32 pid)
{
    for (usize i = 0; i < kMaxPorts; ++i)
        if (g_ports[i].used && g_ports[i].owner_pid == pid)
            port_destroy(static_cast<i32>(i));

    for (usize i = 0; i < kMaxRequests; ++i) {
        Request& r = g_requests[i];
        if (r.state == State::Free)
            continue;
        // A server that died owing an answer, or a caller that died waiting for
        // one. Either way the exchange cannot complete, and leaving it in place
        // would hold a slot and a sleeping task forever.
        if (r.server_pid == pid && r.state == State::Taken) {
            r.state = State::Abandoned;
            scheduler::wake(request_channel(static_cast<i32>(i)));
        } else if (r.caller_pid == pid) {
            r.state = State::Free;
        }
    }
}

} // namespace ipc
