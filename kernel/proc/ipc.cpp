#include <leah/cpu.hpp>
#include <leah/ipc.hpp>
#include <leah/lock.hpp>
#include <leah/object.hpp>
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
    // The caller stopped waiting - it ran out of time, or a signal arrived -
    // while a server still holds the request and still owes an answer.
    //
    // This is not Abandoned with the ends swapped. Abandoned is addressed to
    // the caller, which is still asleep on the channel and which frees the
    // slot when it wakes and finds it. Here the caller has already gone, so
    // there is nobody to read the state or return the slot, and the server is
    // about to write an answer into a request nothing is waiting for. The slot
    // belongs to the server from here: it is freed when the answer arrives and
    // is thrown away, or when the server dies still owing it.
    //
    // Without the distinction a timeout would leak a slot every time, because
    // the caller cannot free what the server may still be writing to, and the
    // server would refuse an unfamiliar state and leave it as it found it.
    Dropped,
};

/* One capability in transit.
 *
 * Held as what it names rather than as the number that named it, because the
 * number belongs to a table the receiver cannot see. Between the server's
 * reply and the caller waking up, this is the only thing that remembers the
 * object at all. */
struct Carried {
    object::Type type;
    void*        pointer;
    u32          rights;
};

struct Request {
    State   state;
    i32     port;
    u32     caller_pid;
    u32     server_pid;
    Message body;       // the request on the way out, the reply on the way back
    Carried carried[kMaxCarried];
    u32     carried_n;
};

struct Port {
    bool used;
    u32  name;
    u32  owner_pid;
};

Port    g_ports[kMaxPorts];

/* Ports and the requests in flight on them. Ranked below the scheduler because
 * that is the direction: a server with no request waiting holds this, checks,
 * and then asks to be blocked. */
sync::RankedLock g_lock(sync::Rank::Ipc, "ipc");
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

/* With the lock already held: abandon() closes every port a dying process
 * owned, and would otherwise take it once per port. */
/* Collects what to wake rather than waking. Its two callers both hold the
 * lock, and waking reaches the scheduler - which nothing here does while
 * holding a lock of its own. */
i64 port_destroy_locked(i32 port, u64* wake, u32* count, u32 max)
{
    if (!valid_port(port) || g_ports[port].owner_pid != scheduler::current_pid())
        return -1;
    g_ports[port].used = false;
    // Anything waiting on this port will never be answered now.
    for (usize i = 0; i < kMaxRequests; ++i) {
        if (g_requests[i].port != port || g_requests[i].state == State::Free)
            continue;
        g_requests[i].state = State::Abandoned;
        if (*count < max)
            wake[(*count)++] = request_channel(static_cast<i32>(i));
    }
    return 0;
}

} // namespace

void init()
{
    memset(g_ports, 0, sizeof(g_ports));
    memset(g_requests, 0, sizeof(g_requests));
}

i64 port_create(u32 name)
{
    sync::Guard guard(g_lock);
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
    sync::Guard guard(g_lock);
    return find_port(name);
}

i64 port_destroy(i32 port)
{
    i64 answer;
    u64 wake_channel[8];
    u32 wake_count = 0;
    {
        sync::Guard inner(g_lock);
        answer = port_destroy_locked(port, wake_channel, &wake_count, 8);
    }
    for (u32 i = 0; i < wake_count; ++i)
        scheduler::wake(wake_channel[i]);
    return answer;
}

// How long to sleep to reach a given moment.
//
// block_on_until takes the length of a nap, not the moment to wake up at, and
// the difference is not visible at the call site because both are counts of
// ticks. Handing it a deadline asks for a nap of however many ticks the
// machine has been up - a wake at roughly twice the uptime, which is to say
// never. recv did this from the day it grew a deadline: a port with traffic
// on it still returned -2, because a message woke the task and the clock was
// checked on the way round, and an idle port - the only kind whose timeout
// matters - waited forever.
static u64 nap_until(u64 deadline_ticks)
{
    const u64 now = timer::ticks();
    return deadline_ticks > now ? deadline_ticks - now : 1;
}

// Stop waiting for an answer, and leave the slot in whichever hand can still
// account for it. Queued means no server ever took it, so nothing will write
// here and it goes straight back. Taken means one is working on it and will
// write into this body, so it stays reserved until that write happens.
static i64 stop_waiting(Request& r, i64 code)
{
    r.state = r.state == State::Taken ? State::Dropped : State::Free;
    return code;
}

i64 call(i32 port, const Message* request, Message* reply_out,
         u64 deadline_ticks)
{
    i32 handle = -1;
    {
    /* Queued under the lock, and the lock let go before the server is woken:
     * waking reaches the scheduler, and no subsystem lock is held across a
     * call into it. Nothing is lost in the gap - the answer is written under
     * this same lock, and the wait below re-checks under it. */
    sync::Guard guard(g_lock);
    if (!valid_port(port) || request == nullptr || reply_out == nullptr)
        return -1;

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

    }

    // Wake a server that is waiting, then sleep until it answers.
    scheduler::wake(port_channel(port));

    sync::Guard guard(g_lock);
    Request& r = g_requests[handle];
    while (r.state != State::Answered && r.state != State::Abandoned) {
        if (deadline_ticks == 0) {
            scheduler::block_on_releasing(request_channel(handle), g_lock);
        } else {
            if (timer::ticks() >= deadline_ticks)
                return stop_waiting(r, -2);
            scheduler::block_on_until_releasing(request_channel(handle),
                                                nap_until(deadline_ticks),
                                                g_lock);
        }
        // The answer first, before either reason for giving up. A reply that
        // landed in the same instant the clock ran out is a completed exchange,
        // and throwing it away to report a timeout would be a lie about a
        // request that was actually served.
        if (r.state == State::Answered || r.state == State::Abandoned)
            break;
        if (deadline_ticks != 0 && timer::ticks() >= deadline_ticks)
            return stop_waiting(r, -2);
        // -3, the same as recv uses, rather than -1. A signal arriving is not
        // the server having died, and the caller can tell them apart only if
        // they arrive as different numbers - one is worth retrying and the
        // other is not.
        if (scheduler::signal_pending())
            return stop_waiting(r, -3);
    }

    const bool ok = r.state == State::Answered;
    if (ok)
        /* Install what came with it, in this process's table, and rewrite the
         * numbers to the ones it will know them by. */
        for (u32 i = 0; i < r.carried_n && i < kMaxCarried; ++i)
            r.body.handle[i] = object::give(scheduler::current_tgid(),
                                            r.carried[i].type,
                                            r.carried[i].pointer,
                                            r.carried[i].rights);
        for (u32 i = r.carried_n; i < kMaxCarried; ++i)
            r.body.handle[i] = object::kNoHandle;
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
    sync::Guard guard(g_lock);
    if (!valid_port(port) || out == nullptr ||
        g_ports[port].owner_pid != scheduler::current_pid())
        return -1;
    return take(port, out, caller_pid);
}

i64 recv(i32 port, Message* out, u32* caller_pid, u64 deadline_ticks)
{
    sync::Guard guard(g_lock);
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
            scheduler::block_on_releasing(port_channel(port), g_lock);
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
            scheduler::block_on_until_releasing(port_channel(port),
                                                nap_until(deadline_ticks),
                                                g_lock);
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
    {
    sync::Guard guard(g_lock);
    if (handle < 0 || handle >= static_cast<i32>(kMaxRequests) || msg == nullptr)
        return -1;
    Request& r = g_requests[handle];
    if (r.state == State::Dropped && r.server_pid == scheduler::current_pid()) {
        // The caller gave up while this was being worked on. The slot was held
        // open only so this write had somewhere safe to land; now that the
        // answer is here and unwanted, it can go back. Reported as a failure
        // because that is what it is - the work was done and delivered to
        // nobody - and a server that only ever ignored this value carries on
        // exactly as before.
        r.state = State::Free;
        return -1;
    }
    if (r.state != State::Taken || r.server_pid != scheduler::current_pid())
        return -1;
    memcpy(&r.body, msg, sizeof(Message));

    /* Resolve what the server is passing, in the server's own table. A handle
     * it does not hold is dropped rather than refused: the reply is still a
     * valid answer, and the caller finds one fewer capability than it hoped
     * for, which it has to be able to cope with anyway. */
    r.carried_n = 0;
    const u32 offered = r.body.handles > kMaxCarried ? kMaxCarried : r.body.handles;
    const u32 server = scheduler::current_tgid();
    for (u32 i = 0; i < offered; ++i) {
        const object::Type type = object::type_of(server, r.body.handle[i]);
        if (type == object::Type::None)
            continue;
        r.carried[r.carried_n].type = type;
        r.carried[r.carried_n].pointer =
            object::look(server, r.body.handle[i], type, 0);
        /* No more than the server itself holds. This is the whole of the
         * safety property: authority can be passed on and never invented. */
        r.carried[r.carried_n].rights = object::rights_of(server, r.body.handle[i]);
        ++r.carried_n;
    }
    r.body.handles = r.carried_n;

    r.state = State::Answered;
    }
    scheduler::wake(request_channel(handle));
    return 0;
}

void abandon(u32 pid)
{
    u64 wake_channel[8];
    u32 wake_count = 0;
    {
    sync::Guard guard(g_lock);
    for (usize i = 0; i < kMaxPorts; ++i)
        if (g_ports[i].used && g_ports[i].owner_pid == pid)
            port_destroy_locked(static_cast<i32>(i), wake_channel, &wake_count, 8);

    for (usize i = 0; i < kMaxRequests; ++i) {
        Request& r = g_requests[i];
        if (r.state == State::Free)
            continue;
        // A server that died owing an answer, or a caller that died waiting for
        // one. Either way the exchange cannot complete, and leaving it in place
        // would hold a slot and a sleeping task forever.
        if (r.server_pid == pid && r.state == State::Dropped) {
            r.state = State::Free;      // held for an answer that will not come
        } else if (r.server_pid == pid && r.state == State::Taken) {
            r.state = State::Abandoned;
            if (wake_count < 8)
                wake_channel[wake_count++] = request_channel(static_cast<i32>(i));
        } else if (r.caller_pid == pid) {
            // Freeing this outright was wrong while a server still held it.
            // The slot would go back to the pool with a reply still coming,
            // and if the next request landed in it and went to the same
            // server, that server's stale answer would arrive with a handle
            // that now names somebody else's request.
            r.state = r.state == State::Taken ? State::Dropped : State::Free;
        }
    }
    }
    /* Outside the lock, for the reason every other subsystem now has the same
     * note about: waking reaches the scheduler. */
    for (u32 i = 0; i < wake_count; ++i)
        scheduler::wake(wake_channel[i]);
}

} // namespace ipc
