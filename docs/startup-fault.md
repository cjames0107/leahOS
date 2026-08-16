# The startup fault that moves when you look at it

Not fixed. Written down so the next attempt starts where this one stopped
rather than from the beginning.

## What happens

A process faults immediately after exec, before it does anything of its own.
It has been seen on audiod, which init restarts when it dies - so the failure
appears as a service that will not come back:

    audiod[66] faulted: unmapped address at 0x0, reading from 0x409f70
    init: audiod died 5 times; leaving it stopped

0x409f70 is the first instruction of a function - `push %rbp` - and the address
it faults on is exactly 0. A push writes to RSP-8, so RSP was 8: the stack
pointer is *gone*, not merely exhausted. Since the kernel sets a correct RSP at
exec (`frame.user_rsp = stack`, from a stack the loader checked), something
inside the running program destroys it between entry and that call. RBP
restored from a smashed frame by `leave` would do it.

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
    reproduce it, which says the sensitive axis is not the one that was
    obviously suspicious.

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
