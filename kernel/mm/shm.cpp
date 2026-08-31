#include <leah/heap.hpp>
#include <leah/lock.hpp>
#include <leah/memory.hpp>
#include <leah/pmm.hpp>
#include <leah/scheduler.hpp>
#include <leah/shm.hpp>
#include <leah/string.hpp>
#include <leah/vmm.hpp>

namespace shm {
namespace {

struct Segment {
    bool     used;
    u32      generation;    // see the id encoding below
    u32      key;
    u32      owner_uid;
    u32      owner_pid;     // who created it, so it can be reclaimed
    u64      bytes;
    u32      flags;
    usize    pages;
    paddr_t* frames;        // one entry per page, in order
};

Segment g_segments[kMaxSegments];

/* Below the frame allocator it calls and above the scheduler that tears it
 * down: shm::abandon runs from a process's last breath, with the scheduler's
 * own lock held. */
sync::RankedLock g_lock(sync::Rank::Shm, "shm");

/* An id is a slot and a generation, not a slot.
 *
 * The slot on its own is recycled the moment a segment is destroyed, and the
 * next open hands it straight back out. Anyone still holding the old number is
 * then talking about somebody else's memory, and nothing anywhere says so -
 * the id is valid, the segment exists, and the writes land in the wrong place.
 *
 * That is not hypothetical. libc kept a file's position in a segment, close()
 * destroyed it when the last descriptor *in that process* let go, and in
 * `echo one | wc -l` with the output redirected the echo stage did exactly
 * that while the shell and wc were still using it. The freed slot went to the
 * next open, which was vfsd's transfer buffer, and the filesystem server
 * stopped answering. It took a day to find, because every symptom was
 * somewhere else.
 *
 * The generation counts allocations of the slot, so a stale id fails to
 * validate instead of naming a stranger. Eight bits of slot and the rest
 * generation, which wraps after two million opens of one slot - long enough
 * that the wrap is not the thing to worry about.
 */
static_assert(kMaxSegments <= 1024, "the id encoding gives the slot ten bits");

constexpr i32 kSlotBits = 10;
constexpr i32 kSlotMask = (1 << kSlotBits) - 1;

i32 make_id(usize slot, u32 generation)
{
    return static_cast<i32>((generation << kSlotBits) |
                            (static_cast<u32>(slot) & kSlotMask));
}

/* The slot an id names, or -1 when the id is stale, malformed or free. Every
 * entry point goes through this; none of them index g_segments directly. */
i32 slot_of(i32 id)
{
    if (id < 0)
        return -1;
    const usize slot = static_cast<usize>(id) & kSlotMask;
    const u32 generation = static_cast<u32>(id) >> kSlotBits;
    if (slot >= kMaxSegments)
        return -1;
    const Segment& segment = g_segments[slot];
    if (!segment.used || segment.generation != generation)
        return -1;
    return static_cast<i32>(slot);
}

u32 g_next_generation = 1;      // never 0, so a zeroed slot never validates

// Frames are allocated one at a time rather than as one contiguous run. A
// window's pixels can be a megabyte, and demanding a megabyte of contiguous
// physical memory from a fragmented allocator fails for no good reason - the
// mapping is built page by page either way.
bool allocate(Segment& segment, usize pages)
{
    segment.frames = static_cast<paddr_t*>(kmalloc(pages * sizeof(paddr_t)));
    if (segment.frames == nullptr)
        return false;

    for (usize i = 0; i < pages; ++i) {
        const paddr_t frame = pmm::alloc();
        if (frame == 0) {
            for (usize j = 0; j < i; ++j)
                pmm::free(segment.frames[j]);
            kfree(segment.frames);
            segment.frames = nullptr;
            return false;
        }
        // A fresh segment reads as zero rather than as whatever the last owner
        // of the frame left in it.
        memset(reinterpret_cast<void*>(memory::phys_to_direct(frame)), 0,
               vmm::kPageSize);
        segment.frames[i] = frame;
    }
    segment.pages = pages;
    return true;
}

/* With the lock already held: open() asks this about every segment it
 * walks past, and would otherwise take the lock a second time. */
bool accessible_locked(i32 id, u32 uid)
{
    const i32 slot = slot_of(id);
    if (slot < 0)
        return false;
    return uid == 0 || uid == g_segments[slot].owner_uid ||
           (g_segments[slot].flags & Public) != 0;
}

} // namespace

void init()
{
    memset(g_segments, 0, sizeof(g_segments));
}

bool accessible(i32 id, u32 uid)
{
    sync::Guard guard(g_lock);
    return accessible_locked(id, uid);
}

i32 open(u32 key, u64 bytes, u32 uid, u32 flags)
{
    sync::Guard guard(g_lock);
    if (key == 0)
        return -1;                      // 0 is reserved for "no key"

    for (usize i = 0; i < kMaxSegments; ++i) {
        if (!g_segments[i].used || g_segments[i].key != key)
            continue;
        // Same rule as a file: the owner, or root - unless the creator marked
        // it public.
        if (!accessible_locked(make_id(i, g_segments[i].generation), uid))
            return -1;
        // The slot is claimed before its frames exist, so a segment can be
        // found here while it is still being built. pages is set last and is
        // therefore the signal that it is finished; waiting is right, because
        // the answer is about to be yes.
        for (u32 spin = 0; g_segments[i].pages == 0 && spin < 10000; ++spin)
            scheduler::yield();
        if (g_segments[i].pages == 0)
            return -1;
        return make_id(i, g_segments[i].generation);
    }

    if (bytes == 0)
        return -1;                      // opening something that is not there
    const usize pages = static_cast<usize>((bytes + vmm::kPageSize - 1) / vmm::kPageSize);
    if (pages == 0 || pages > 4096)     // 16 MiB is far more than anything needs
        return -1;

    for (usize i = 0; i < kMaxSegments; ++i) {
        Segment& segment = g_segments[i];
        if (segment.used)
            continue;
        // Claim the slot before filling it, not after.
        //
        // allocate() asks the page allocator for a frame per page, and a window
        // is hundreds of pages; a task holding the kernel lock that long will be
        // preempted, and the lock is handed on when that happens. So another
        // task can arrive here in the middle - and if the slot still says it is
        // free it picks the same one, zeroes the Segment, and the first task
        // resumes writing frames through a pointer that is now null. That is a
        // kernel panic a few hundred pages into whichever window happened to be
        // second, which is to say: whenever two programs opened a window at
        // once.
        memset(&segment, 0, sizeof(segment));
        segment.used       = true;
        segment.generation = g_next_generation++;
        segment.key       = key;
        segment.owner_uid = uid;
        segment.owner_pid = scheduler::current_tgid();
        segment.bytes     = bytes;
        segment.flags     = flags;
        if (!allocate(segment, pages)) {
            segment.used = false;
            return -1;
        }
        return make_id(i, segment.generation);
    }
    return -1;
}

void abandon(u32 pid)
{
    sync::Guard guard(g_lock);
    /* Segments belong to the process that made them, and nothing was giving
     * them back. libc opens one per process for talking to vfsd, keyed by pid
     * so two processes never share a transfer buffer - which means a fresh key
     * every time and a segment that outlived its owner. There are thirty-two;
     * a few dozen program launches exhausted them, every later open failed,
     * and the machine stopped being able to read files. It looked like the
     * desktop falling apart under use, because that is what it was. */
    for (usize i = 0; i < kMaxSegments; ++i) {
        Segment& segment = g_segments[i];
        if (!segment.used || segment.owner_pid != pid)
            continue;
        for (usize page = 0; page < segment.pages; ++page)
            pmm::release(segment.frames[page]);
        kfree(segment.frames);
        memset(&segment, 0, sizeof(segment));
    }
}

bool destroy(i32 id, u32 uid)
{
    sync::Guard guard(g_lock);
    const i32 slot = slot_of(id);
    if (slot < 0)
        return false;
    Segment& segment = g_segments[slot];
    if (uid != 0 && uid != segment.owner_uid)
        return false;

    for (usize i = 0; i < segment.pages; ++i)
        pmm::release(segment.frames[i]);
    kfree(segment.frames);
    memset(&segment, 0, sizeof(segment));
    return true;
}

paddr_t frame_of(i32 id, usize index)
{
    sync::Guard guard(g_lock);
    const i32 slot = slot_of(id);
    if (slot < 0 || index >= g_segments[slot].pages)
        return 0;
    return g_segments[slot].frames[index];
}

u64   size_of(i32 id)      { const i32 s = slot_of(id);
                             return s < 0 ? 0 : g_segments[s].bytes; }
usize page_count(i32 id)   { const i32 s = slot_of(id);
                             return s < 0 ? 0 : g_segments[s].pages; }
u32   owner_uid_of(i32 id) { const i32 s = slot_of(id);
                             return s < 0 ? 0 : g_segments[s].owner_uid; }
bool  exists(i32 id)       { return slot_of(id) >= 0; }

bool share_frames(i32 id)
{
    sync::Guard guard(g_lock);
    const i32 slot = slot_of(id);
    if (slot < 0)
        return false;
    const Segment& segment = g_segments[slot];
    for (usize i = 0; i < segment.pages; ++i) {
        if (!pmm::share(segment.frames[i])) {
            // Undo, so a failure part-way does not leave counts inflated.
            for (usize j = 0; j < i; ++j)
                pmm::release(segment.frames[j]);
            return false;
        }
    }
    return true;
}

} // namespace shm
