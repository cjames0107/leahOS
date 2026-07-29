#include <leah/heap.hpp>
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
    u32      key;
    u32      owner_uid;
    u64      bytes;
    u32      flags;
    usize    pages;
    paddr_t* frames;        // one entry per page, in order
};

Segment g_segments[kMaxSegments];

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

bool valid(i32 id)
{
    return id >= 0 && id < static_cast<i32>(kMaxSegments) && g_segments[id].used;
}

} // namespace

void init()
{
    memset(g_segments, 0, sizeof(g_segments));
}

bool accessible(i32 id, u32 uid)
{
    if (!valid(id))
        return false;
    return uid == 0 || uid == g_segments[id].owner_uid ||
           (g_segments[id].flags & Public) != 0;
}

i32 open(u32 key, u64 bytes, u32 uid, u32 flags)
{
    if (key == 0)
        return -1;                      // 0 is reserved for "no key"

    for (usize i = 0; i < kMaxSegments; ++i) {
        if (!g_segments[i].used || g_segments[i].key != key)
            continue;
        // Same rule as a file: the owner, or root - unless the creator marked
        // it public.
        if (!accessible(static_cast<i32>(i), uid))
            return -1;
        // The slot is claimed before its frames exist, so a segment can be
        // found here while it is still being built. pages is set last and is
        // therefore the signal that it is finished; waiting is right, because
        // the answer is about to be yes.
        for (u32 spin = 0; g_segments[i].pages == 0 && spin < 10000; ++spin)
            scheduler::yield();
        if (g_segments[i].pages == 0)
            return -1;
        return static_cast<i32>(i);
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
        segment.used      = true;
        segment.key       = key;
        segment.owner_uid = uid;
        segment.bytes     = bytes;
        segment.flags     = flags;
        if (!allocate(segment, pages)) {
            segment.used = false;
            return -1;
        }
        return static_cast<i32>(i);
    }
    return -1;
}

bool destroy(i32 id, u32 uid)
{
    if (!valid(id))
        return false;
    Segment& segment = g_segments[id];
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
    if (!valid(id) || index >= g_segments[id].pages)
        return 0;
    return g_segments[id].frames[index];
}

u64   size_of(i32 id)      { return valid(id) ? g_segments[id].bytes : 0; }
usize page_count(i32 id)   { return valid(id) ? g_segments[id].pages : 0; }
u32   owner_uid_of(i32 id) { return valid(id) ? g_segments[id].owner_uid : 0; }
bool  exists(i32 id)       { return valid(id); }

bool share_frames(i32 id)
{
    if (!valid(id))
        return false;
    const Segment& segment = g_segments[id];
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
