# The startup fault that moves when you look at it

Not fixed. Written down so the next attempt starts where this one stopped
rather than from the beginning.

## What happens

A process faults immediately after exec, before it does anything of its own.
It has been seen on audiod, which init restarts when it dies - so the failure
appears as a service that will not come back:

    audiod[66] faulted: unmapped address at 0x0, reading from 0x409f70
    init: audiod died 5 times; leaving it stopped

It faults on address 0, and the report says **reading**.

That last word matters, and an earlier version of this document got it wrong.
The address was disassembled to `push %rbp` at the start of a function, and
from that came "RSP is 8, the stack pointer is gone" - which is a good story
and is not what the machine said. A push is a *write*; the fault was a read.
The disassembly was of a binary rebuilt after the event, so the address no
longer meant what it had when the fault happened, and a fortnight of looking
for a smashed stack came out of one stale objdump.

What the report actually describes is a plain null dereference at a fixed code
address, in a process that has just been exec'd and has not yet done anything
of its own. `tools/vm/machine.py` now keeps the binaries whenever a run faults,
so the next occurrence can be resolved against the image that produced it.

## What it depends on

Not on memory pressure, and not on how big anything is:

  - Making the UI view pool a static array (~90K of BSS) reproduced it twice
    in a row. Allocating that pool on demand made it stop.
  - 110K of *untouched* padding in another file puts BSS **higher** than the
    failing build and does not reproduce it at all.
  - Restoring the static pool a week later, after libc had changed twice for
    unrelated reasons, did **not** reproduce it.

So it is sensitive to where things land, not to how much there is. That is the
signature of an out-of-bounds write whose victim depends on what happens to sit
next to it: harmless in one layout, fatal in another.

It has been seen twice on unrelated changes - once when net, cred and audio
were given call deadlines, which broke `cat` for reasons that made no sense
either, and once here.

## Audited and clean

Every place the symptom could come from, read line by line:

  - **Every `shm_map` in libc.** All thirteen check for null, including the two
    that looked unguarded at a glance.
  - **`position()`**, the one function that could plausibly hand back a null
    for a dereference like this. It falls back to `&e->offset` rather than
    returning 0, so `*position(e)` is always safe.
  - **Signal delivery.** `push_signal_frame` validates the whole range it is
    about to write with `user_range_ok` and kills the process rather than
    entering a handler it cannot return from.
  - **`malloc`.** A bump allocator over sbrk; the growth arithmetic is correct
    for any request, including the ~780K the exec path asks for.
  - **The entry point.** `ENTRY(_start)` is explicit in user.ld, so a shifted
    link order cannot start a program in the middle of a function - which was
    the best remaining theory for how a fresh process runs with a bad frame.

## Ruled out

  - **BSS size.** See the padding experiment above.
  - **A failed mapping.** `build_image` checks `map_user_stack` and prints
    "could not be mapped"; that line never appears.
  - **crt0**, which is a dozen instructions and does not scale with anything.
  - **An address collision.** Programs link at 4 MiB, the heap starts at
    256 MiB, and the BSS in question is 300K.
  - **The kernel's argument copying.** `copy_vector` is bounded at both
    `kMaxArgs` and `kArgStorage`.
  - **Large locals** in audiod or in libc's startup path. There are none.

## The sweep, and what it settled

A reproduction loop was built for this: boot, kill audiod, look for a fault in
the restart. Two minutes a sample rather than the six a full `make check`
takes. It checks itself - if the kill did nothing, or audiod never came back,
it says so instead of reporting "clean", because a detector that passes when
nothing happened is how the last three of these went wrong.

With that:

  - The **known-failing configuration is now clean**. The static view pool,
    which reproduced twice in a row, does not reproduce at all - confirmed with
    the detector shown to work (it killed pid 13 and saw the restart as 35).
    Between then and now libc gained a few kilobytes of code for unrelated
    reasons. That is all it took to move it.
  - **Eight BSS layouts, 8K to 120K, all clean.** Padding .bss does not
    reproduce it.
  - **Six code layouts, 1K to 64K, all clean.** Nor does padding .text, which
    was the better guess after BSS failed - code is what moved the bug last.

Fourteen layouts on the two axes that could plausibly matter, and none of them
brings it back.

So the bug is not reachable by sweeping BSS, and the thing that moved it last
was *code*. A sweep of text or data placement might find it; it might equally
be moved again by the next commit.

## Where to go next

The thing to find is a write that runs past the end of something. Suggestions,
cheapest first:

  1. Sweep the layout deliberately: a padding array in libc, grown a few
     hundred bytes at a time, until it reproduces. That gives a reproduction
     that does not evaporate, which is what this attempt lacked.
  2. ~~A canary~~ - done. Userland builds with -fstack-protector-strong and
     libc provides __stack_chk_guard and __stack_chk_fail. An overrun of a
     local array now dies where it happens, naming nothing but saying plainly
     what went wrong, instead of corrupting a frame whose damage appears two
     calls and one exec later. The suite proves it fires: a function that
     writes ninety-six bytes into an eight-byte array is expected to die.

     This has not caught the startup fault, because the startup fault is not
     currently happening. It is armed for when it comes back.
  3. The user stack is 16 pages with nothing below it. A guard page would not
     have caught this one - the fault address is 0, not just under the stack -
     but it would catch the ordinary kind.

Fault reports print RSP now, which is what turned "a null dereference in libc"
into "the stack pointer is gone". That was the whole of this session's
progress and it is worth having whatever happens next.

---

# A second sighting: Paint's first file write

Same shape, different place, found while making dialogues into windows.

Paint saves by writing its window buffer out as a PNG. With the new sheet, the
save is often the **first filesystem call the process has ever made** - the old
dialogue browsed a directory to show it, which quietly did one first.

  - Save as the first filesystem call: no file appears. `ls` finds nothing, so
    `open` never even created it.
  - Save after *any* earlier filesystem call - a `fopen("/dev/console")`, a
    one-byte `cli_write_file` - and it works, reporting rc=0 and a valid
    buffer. Five runs, both ways, consistent.

The observation is only possible through a channel that perturbs it: the way to
watch is to write to the console, and writing to the console is itself the
thing that makes it work. That is worth saying plainly rather than dressing up.

## Ruled out here too

  - **Shared memory exhaustion.** 512 segments; a desktop uses a handful.
  - **The heap.** `sys_sbrk` has no limit and fails only when physical memory
    is gone; the machine has 512 MiB and the request is 350K.
  - **An address collision.** The heap is at 256 MiB and mmap at 1 GiB.
  - **A partial write.** `flush` opens before it writes, so a failed write
    would still leave an empty file behind. There is no file at all, which
    means `open` failed or was never reached.

## What is different about it

This one reproduces on demand, which the startup fault does not. Anyone picking
it up can watch it fail every time by saving from a freshly started Paint, and
watch it succeed every time by touching the filesystem first. That makes it the
better of the two to chase, and they may well be the same fault.
