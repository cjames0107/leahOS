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

## Where to go next

The thing to find is a write that runs past the end of something. Suggestions,
cheapest first:

  1. Sweep the layout deliberately: a padding array in libc, grown a few
     hundred bytes at a time, until it reproduces. That gives a reproduction
     that does not evaporate, which is what this attempt lacked.
  2. A canary: have crt0 write a known value below the initial stack and have
     libc check it. If it is gone by the time libc runs, the overrun is in
     startup rather than later.
  3. The user stack is 16 pages with nothing below it. A guard page would not
     have caught this one - the fault address is 0, not just under the stack -
     but it would catch the ordinary kind.

Fault reports print RSP now, which is what turned "a null dereference in libc"
into "the stack pointer is gone". That was the whole of this session's
progress and it is worth having whatever happens next.
