#pragma once

#include <leah/types.hpp>

// Numbers the kernel behaves by, that somebody might want to change.
//
// Every one of these was a constant in a header, which is the right place for
// a number nobody should touch and the wrong place for a policy. The two are
// worth telling apart: how many pages to read ahead of a fault is a judgement
// about this machine and this workload, and belongs to whoever is running it;
// the rank of a lock is a correctness invariant and belongs to the code.
//
// So this holds the first kind only. Anything whose wrong value is a bug
// rather than a preference stays where it is - lock ranks, ABI layouts, the
// page size, and every decision about what a process is allowed to do. A
// capability that could be switched off would not be a capability.
//
// Root only, because a number that changes how memory is reclaimed is a number
// that can be used to make the machine unusable.

namespace tunable {

enum class Key : u32 {
    None = 0,

    // Pages mapped either side of a file-mapping fault, this one included.
    // One is pure demand paging: nothing is mapped until it is touched. Higher
    // trades memory for fewer faults, which is the right trade on a machine
    // that reads a mapped file front to back and the wrong one on a machine
    // that pokes at a byte here and there.
    MapAhead = 1,

    // Program images the kernel will hold before evicting the least recently
    // used. Each costs the size of the file; the win is that running a program
    // again reads nothing at all.
    ImageLimit = 2,

    // Fill freed frames with 0xCC and check them on the way back out. Finds a
    // page used after it was given up, at the cost of a page-sized store per
    // free. On while there is a bug like that to find.
    PoisonFrames = 3,

    // Map a whole file mapping up front instead of a page at a time as it is
    // touched.
    //
    // Eager is the default and it is not the better design - demand paging is,
    // and it is implemented and works. It is the default because mapping
    // lazily destabilises the boot in a way not yet understood: the filesystem
    // server intermittently fails to start, at a rate the eager path does not
    // have. The mechanism is here and correct; the policy is conservative
    // until that is explained, and this is the switch between them.
    MapFileEager = 4,


    Count
};

void init();

// The current value, or -1 for a key this kernel does not know.
i64 get(Key key);

// Returns false for an unknown key or a value outside what the key allows.
bool set(Key key, i64 value);

// Fast path for the fault handler, which asks on every miss.
u32 map_ahead();
bool poison_frames();
bool map_file_eager();

} // namespace tunable
