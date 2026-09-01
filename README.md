# leahOS

A UNIX-like x86-64 operating system written from scratch in NASM and C++23,
with the eventual goal of POSIX compliance. No third-party bootloader, no
libc, no runtime — every instruction from the BIOS handoff onward is ours.

## Building

Requires a `x86_64-elf` cross-toolchain, `nasm`, and `qemu`:

```sh
brew install x86_64-elf-gcc x86_64-elf-binutils x86_64-elf-gdb nasm qemu
```

`make toolchain` reports what is and isn't installed; any build target will
refuse to start with a readable error if something is missing.

```sh
make              # build build/leahos.img
make run          # boot in QEMU, window + serial on stdio
make headless     # boot with no window, print COM1, exit — the fast loop
make debug        # boot halted, gdb stub on :1234
make gdb          # attach in another terminal
make toolchain    # report installed tools
make clean
make help
```

Knobs, overridable on the command line:

| Variable | Default | |
|---|---|---|
| `MEM` | `512M` | guest RAM — the kernel reads it back out of E820 |
| `CPUS` | `1` | `-smp`; tasks run on all of them. `-smp 8` saturates an 8-core host, so slowness there is the host, not the kernel |
| `TIMEOUT` | `6` | seconds `make headless` lets the guest run |
| `QEMU_EXTRA` | | appended to the QEMU command line |

```sh
make run MEM=2G
make headless TIMEOUT=15
make run QEMU_EXTRA="-d int -D build/qemu.log"
```

The host here is an arm64 Mac, so QEMU runs x86-64 under TCG emulation with no
hardware acceleration available. Expect it to feel slow; that is the emulator,
not the kernel.

## Boot chain

The BIOS gives us 512 bytes and 16-bit real mode. Getting from there to a
C++ function in 64-bit long mode takes two stages.

```
BIOS  ──►  stage1        boot/stage1.asm    512 B @ 0x7C00, 16-bit real
             │  int 13h AH=41h   confirm LBA extensions exist
             │  int 13h AH=42h   read stage2 from LBA 1
             ▼
           stage2        boot/stage2.asm    16 KiB @ 0x8000, 16-bit real
             │  port 0x92        open the A20 gate
             │  unreal mode      FS gets a 4 GiB limit, real mode kept
             │  int 15h E820     capture the memory map ──► 0x20000
             │  int 13h AH=42h   read kernel in chunks ──► FS:0x100000
             │  lgdt, CR0.PE=1   ──► 32-bit protected mode
             │
             │  PML4/PDPT/PD     identity map 1 GiB with 2 MiB pages
             │  CR4.PAE, EFER.LME, CR0.PG
             ▼                   ──► 64-bit long mode
           kernel        kernel/arch/x86_64/entry.asm  @ 0x100000
             │  zero .bss, set up a stack
             ▼
           kernel_main(const boot::MemoryMap*)          kernel/main.cpp
```

### Why it looks like this

**Two stages, not one.** 512 bytes is enough to read from disk and nothing
else. Everything that needs real code — A20, E820, mode switching — lives in
stage 2, which has 16 KiB to work with.

**LBA only.** `int 13h` extensions have been universal since the late 90s.
A BIOS old enough to require CHS addressing cannot run a 64-bit kernel.

**Unreal mode carries the kernel above 1 MiB.** `int 13h` writes through a
real-mode `segment:offset`, so the BIOS can never place data above 1 MiB. Stage
2 enters protected mode briefly, loads **FS** from a descriptor with a 4 GiB
limit, and returns to real mode without touching FS again. A segment's base and
limit live in a hidden cache that is only reloaded when the selector is
written — so the 4 GiB limit survives, and real-mode code can address all of
memory through FS while the BIOS still works normally.

FS specifically, not DS or ES: those get reloaded constantly for BIOS calls,
and any write to them would drop the big limit on the floor.

Each chunk is read into a low bounce buffer and copied up through FS. This is
what lifted the old 64 KiB kernel ceiling; the limit is now 8 MiB, and that is
just the space reserved in the disk image.

**Stage 2 is told the kernel's exact size at build time.** The Makefile
computes the sector count from the linked kernel and passes it as
`-DKERNEL_SECTORS`, so boot reads exactly as much disk as there is kernel
rather than a generous constant.

**Higher half, with the identity map kept.** The kernel is linked for
`0xFFFFFFFF80100000` and loaded at 1 MiB; virtual `0xFFFFFFFF80000000` is
physical 0, so translation is a subtraction rather than a page-table walk.

Stage 2 maps the *same* page directory twice — once at `PML4[0]` and once at
`PML4[511]/PDPT[510]` — so the kernel is reachable at both addresses. Keeping
the identity window is what makes the transition safe: every address the
bootloader already handed out stays valid across the `CR3` load, and the VMM
can still reach page tables by their physical address without a separate
direct-map offset.

The linker script gives every section an `AT()` so its load address is
physical while its symbols resolve high. `objcopy -O binary` lays the image
out by load address, which is exactly what the bootloader copies to 1 MiB.

**Flat binary, not ELF.** The kernel is `objcopy -O binary` with `.text.entry`
forced first by the linker script, so "load it and jump to offset 0" is the
entire loader. An ELF parser is the right thing eventually — it buys us proper
section permissions and symbols — but not before there is something to protect.

## Memory layout

Physical addresses during boot. Nothing here may overlap; see
`boot/layout.inc`, which is the single source of truth shared by both stages.

| Range | Contents |
|---|---|
| `0x00000`–`0x004FF` | BIOS IVT + data area (untouchable) |
| `0x07C00`–`0x07DFF` | stage 1 |
| `0x08000`–`0x0BFFF` | stage 2 |
| `0x10000`–`0x17FFF` | disk bounce buffer (32 KiB) |
| `0x20000`–`0x20FFF` | E820 memory map |
| `0x70000`–`0x73FFF` | PML4 / PDPT / PD / high PDPT |
| `0xA0000`–`0xFFFFF` | video memory + BIOS ROM (untouchable) |
| `0x100000`+ | kernel |

## Layout

```
boot/           stage1.asm, stage2.asm, layout.inc
kernel/
  arch/x86_64/  entry.asm, isr.asm, gdt, idt, pic, timer, keyboard,
                mouse, pci, console, panic
  drivers/      ata.cpp, blockdev.cpp
  fs/           vfs.cpp, fat32.cpp, files.cpp
  mm/           pmm.cpp, vmm.cpp, heap.cpp
  sched/        scheduler.cpp
  proc/         process.cpp                   fork/exec/wait
  include/leah/ public headers
  lib/          string.cpp, cxx.cpp           freestanding runtime
  main.cpp      kernel_main
  linker.ld
user/           libc/ (crt0, syscalls, string, stdio, stdlib, fs)
                init.c, sh.c, echo.c, cat.c, ls.c, pwd.c, hello.c
tools/          run-headless.sh, mkfs_fat32.py
```

## Status

A **microkernel**. The filesystem, the block and disk drivers, the network
stack, audio, USB, PS/2, authentication and the window server are ordinary
ring-3 processes that talk over synchronous IPC; what is left in ring 0 is
scheduling, memory, capabilities, pseudo-terminals and the message passing
itself. The kernel does not know what an ELF is and cannot read a file.

It boots to long mode, owns its descriptor tables, manages physical and virtual
memory, and mounts an **ext4 root filesystem** served by `vfsd`. Programs run in
ring 3 over a `SYSCALL` ABI in private address spaces, time-sliced across **all**
processors. They `fork`, `execve` and `wait`, catch **signals**, run **threads**,
and live under users and permissions enforced by the server that owns the file.

Everything on the disk is **dynamically linked** against one `/lib/libc.so`
through a two-hundred-line `ld.so`, and a program's text is *mapped* from a
kernel-held image rather than copied — so libc exists once in memory instead of
once per process, and a program that has been run before is not read at all.
Twelve processes cost about 167 KiB each, which is less than libc's text.

Authority is moving to **capabilities**: typed kernel objects with rights, held
by handle, passed over IPC by what they name rather than by number. The first
thing they enforce is one nothing else could — a program's right to be executed,
issued by the filesystem server, which is the only party that can see the file,
its mode bits and the caller's credentials together.

A **window server** composites a desktop over the linear framebuffer with a dock
and a status bar; clients draw into shared memory and never touch the screen.
There is a terminal with real **pseudo-terminals**, a shell with `if`, `while`,
`for`, `case` and functions, a `vi` that needs no window server, **TCP sockets**
that can be listened on — `httpd` serves files over the network — and real
timezones read from TZif.

The **big kernel lock is gone from the system call path**. Eleven subsystems
have locks of their own, each carrying a rank checked on every acquisition, so
taking them out of order is a panic naming both rather than a hang once every
few hundred boots.

## Memory management

Three layers, each built on the one below:

**Physical** — a bitmap allocator over the E820 map, one bit per 4 KiB frame.
It starts from "everything is used" and clears only what E820 called usable, so
holes stay reserved by default. The bitmap is placed immediately after the
kernel image, because the thing that allocates memory cannot allocate its own
bookkeeping.

**Virtual** — a 4-level page table the kernel builds itself, replacing the
throwaway map from stage 2. The low 4 GiB is identity mapped with 2 MiB pages,
which covers RAM plus the MMIO window where the LAPIC, PCI BARs and any
framebuffer live. `map()` works at 4 KiB granularity and splits a containing
2 MiB page when it has to, so callers never need to know how a region was
originally mapped.

Note the bound on the high identity map: it stops at the highest *usable*
address, not the highest address E820 described. Firmware routinely reports
reserved regions near the top of the 64-bit space, and spanning that hole costs
megabytes of page tables describing memory that does not exist.

**Heap** — first-fit free list with splitting and coalescing, grown on demand
by mapping fresh frames at 1 TiB. Backs `kmalloc`/`kfree` and the global
`operator new`/`delete`.

Those `operator new` overloads must be declared with the ABI's `size_t`
(`unsigned long`) rather than the kernel's `u64` (`unsigned long long`). Both
are 64 bits, but they are distinct types and the compiler rejects a near miss.

## Interrupts

The kernel owns its GDT, TSS and IDT rather than inheriting stage 2's. Two
details there are worth knowing because they are invisible until they bite:

**#DF runs on its own stack (IST1).** A double fault usually means the kernel
stack overflowed. If the handler pushed onto that same broken stack it would
fault again, and a fault while handling a double fault is a triple fault — which
the CPU answers by resetting the machine with no diagnostic at all. IST1 gives
`#DF` known-good ground to stand on.

**GDT selector order is fixed by `SYSCALL`/`SYSRET`.** Both derive four
selectors from one base in the `STAR` MSR at hard-coded offsets, so kernel code
must be `0x08` and user data `0x18`. Renumbering them to something tidier
silently breaks ring 3 much later.

Exceptions panic with a full register dump plus `CR0`–`CR4`; page faults also
decode `CR2` and the error code into a readable cause.

## Storage

`ata.cpp` drives IDE disks over programmed I/O — every byte moves through the
CPU. AHCI with DMA will replace it, but PIO needs no memory mapping, no
interrupt plumbing and no controller-specific setup, which makes it the right
thing to read a first filesystem with.

Interrupts are disabled at the drive (`nIEN`) and transfers are polled;
delivering IRQ 14/15 to a vector with no handler would be worse than spinning.
LBA48 is used automatically when an access reaches past the 28-bit limit.

The boot-time check reads the kernel back from LBA 64 and compares it against
the image already in memory. Since the bootloader put it there by a completely
different route, the two agreeing validates the driver and the unreal-mode
loader against each other. Writes are then checked separately against a scratch
sector, because writing is the operation that can quietly corrupt a disk.

## Filesystem

`vfs` is the filesystem-independent layer; `fs::Fat32` is the first thing
behind it. The interface is path-based rather than handle-based on purpose:
open/close/seek needs a file descriptor table, and that belongs with processes
rather than ahead of them.

FAT32 first because it is the format every other system can already write. The
image is built by `tools/mkfs_fat32.py` and checked with `fsck_msdos` before
the kernel ever sees it, so the reader is validated against a real
implementation instead of only against the tool that produced the image.

Two details in the format are worth knowing:

**A volume is identified by what is zero, not by the "FAT32" string.** FAT12
and FAT16 put non-zero values in the 16-bit FAT-size and root-entry-count
fields; FAT32 leaves both at zero. The type string further down the BPB is
advisory and some formatters get it wrong.

**Long filenames are stored in fake directory entries.** They carry every one
of the low four attribute bits set at once — a combination no real file can
have — so systems predating long filenames skip them rather than showing
garbage. They also appear *before* the 8.3 entry they belong to, in reverse
order, which is why the reader accumulates them until the short entry arrives.

`block::Partition` bounds-checks and rebases every access, so a filesystem bug
cannot reach outside its partition — notably not into the kernel image sitting
earlier on the same disk.

Writing allocates clusters, links FAT chains, creates directory entries with
long filenames when 8.3 cannot hold the name, and maintains FSInfo. Every FAT
copy is updated on each change, because a repair tool comparing them treats a
mismatch as corruption.

The check that matters is external: after the kernel has written to the volume,
`fsck_msdos` must still report it clean. That is what caught the FSInfo free
count being maintained from zero instead of seeded from disk — a bug the
kernel's own read-back tests could never have found, because it read its own
wrong answer back consistently.

## ext filesystem

`fs::Ext` is a single driver for the whole ext2/3/4 family, which share one
on-disk format: ext3 is ext2 plus a journal, and ext4 adds extent-mapped files.
It is the **root filesystem**, on a second disk (whole-disk ext4, no partition
table); disk 0 stays the bootable/kernel disk and the FAT32 fallback. ext is the
root because, unlike FAT32, its inodes carry real ownership and mode bits — the
groundwork for users and permissions.

The **reader** is deliberately broad, verified booting against a volume built
with every hard feature on: the journal inode is skipped, `dir_index`/HTree
directories are read by scanning their leaf blocks (the index is just an
optimisation the linear entries stay valid under), 64-bit group descriptors and
metadata checksums are handled or ignored as needed, and file blocks are mapped
both ways — extent trees (ext4) and classic 12-direct-plus-indirect (ext2/3).

The **writer** targets a deliberately tamed feature set, built by
`tools/mkext.sh` with `-O ^metadata_csum,^64bit,^dir_index,^orphan_file`. Each
dropped feature is one the writer would otherwise have to maintain on every
change: crc32c over all metadata, 64-byte descriptors, an HTree index on
directory insert, and the orphan-inode list. Extents and the journal stay on, so
it is a genuine ext4 volume; the journal is simply left empty (consistent direct
metadata updates, no transactions). The writer allocates inodes and blocks from
the group bitmaps, keeps the group-descriptor and superblock free counts exact,
grows files by appending to an inline extent, and inserts and removes linear
directory entries by splitting and coalescing `rec_len`.

One subtlety cost a debugging round: a freed inode has to be left *pristine*
(the whole slot zeroed, as mke2fs leaves unused inodes), not as a half-cleared
"deleted" inode with its extent and size intact — otherwise `e2fsck`'s orphan
recovery collects it as a corrupted orphan.

As with FAT32, the check that matters is external: `tools/fsck-ext.sh` boots
once with a persistent disk and then runs `e2fsck -fn`, which must report the
volume clean after the kernel's writes. Building and checking images needs
`e2fsprogs` (`brew install e2fsprogs`).

## Disk image

| LBA | Contents |
|---|---|
| 0 | stage 1 / MBR, with the partition table at offset 446 |
| 1–32 | stage 2 |
| 64–16447 | kernel (8 MiB reserved) |

Disk 0 is the bootloader and the kernel and nothing else. It used to carry a
FAT32 partition with a second copy of every program, which nothing had been
able to read since the FAT driver was removed — and which was the reason every
name in the system was upper case with `.ELF` on the end. Both are gone; the
root filesystem on disk 1 is the only filesystem.

## Filesystem layout

The Filesystem Hierarchy Standard, as much of it as means anything here.

| Path | Contents |
|---|---|
| `/bin` | essential commands — `sh`, `ls`, `grep`, `less` |
| `/sbin` | the system's own — `init`, `login`, `wserver`, every driver |
| `/usr/bin` | everything else — networking, diagnostics, the self-tests |
| `/opt` | application bundles, one `.app` directory each |
| `/usr/share` | icons, wallpapers, documentation, the demo media |
| `/etc` | `passwd` and `shadow` |
| `/dev` | `null`, `zero`, `full`, `tty`, `console` |
| `/tmp`, `/var`, `/run`, `/srv`, `/mnt`, `/media` | as the standard says |
| `/proc`, `/sys` | mount points; there is no procfs yet |

Programs are lower case with no extension. They were `/BIN/NAME.ELF` because
the first filesystem this system could read was FAT, whose names are eight
characters and three, upper case. That driver has been gone for a long time and
the shouting outlived it — along with a hardcoded path in every program that
wanted to start another. A command is now found by name along `/bin`, `/sbin`,
`/usr/bin` and `/usr/local/bin`, which is what a `PATH` would do if there were
environment variables to keep one in.

Whether a file is a program is the execute bits, not the name. `getdents`
reports the mode, so a file browser can tell a program from a document without
stat-ing every name it was just told about.

### /dev

The entries exist on disk as empty files, so `ls /dev` lists them and a path
naming one is not a fiction. What they *do* lives in libc, which is already
where path resolution and the descriptor table are — and which is the only
thing that can know which terminal a process belongs to.

`/dev/tty` is that terminal. Not standard input: the shell redirects that for
every pipeline, and `something | less` is exactly the case where a program
needs the keyboard while its standard input is a pipe. The terminal marks one
when it starts the shell; it is inherited from there through every fork and
exec.

## ELF loading

`elf::load()` maps every `PT_LOAD` segment at its own `p_vaddr`, reading through
the VFS rather than off a raw device. It is the piece `execve()` will reuse
unchanged.

The distinction between `p_filesz` and `p_memsz` is the one that matters: the
difference is `.bss`, which exists in memory but not in the file and must read
as zero. Pages are cleared as they are mapped, so the tail of a segment is
already correct when the file bytes land in front of it.

`user/test.asm` is a real ELF built by the Makefile and placed on the image,
not a synthetic one built by the test. It stores a value in `.bss`, adds it to
one in `.rodata`, and returns the sum — so a wrong answer distinguishes "`.bss`
was not zeroed" from "the segment was not mapped at all".

It links at `0x600000000000` rather than the conventional `0x400000` because
there is only one address space today, and mapping over `0x400000` would
replace the identity mapping of physical memory the PMM is still handing out.
Once processes have their own address spaces, the conventional address is fine.

## Userspace

A loaded ELF runs at CPL 3. `syscall::run()` enters ring 3 through `IRETQ` —
the one instruction that sets `SS:RSP`, `RFLAGS` and `CS:RIP` atomically across
a privilege drop — and does not return until the program calls exit. The exit
syscall is a longjmp: it restores the kernel context `enter_user_mode` saved and
returns out of it with the program's exit code. That is exactly the primitive a
scheduler will later generalise into a context switch.

Calls come in through `SYSCALL`: number in `RAX`, arguments in
`RDI/RSI/RDX/R10/R8/R9` — the SysV order, with `R10` standing in for `RCX`,
which `SYSCALL` destroys. The instruction does *not* switch stacks, so the entry
stub saves the user `RSP` and loads a kernel stack itself.

Two things here are a slot away from a silent triple fault:

**The STAR selector base.** `SYSRET` computes `CS = (STAR[63:48]+16)|3` and
`SS = (STAR[63:48]+8)|3`. The base is therefore *eight below user data*, not the
user selector itself — chosen so `+8` lands on user data and `+16` on user code.
Programming it as the user-data selector instead points `SYSRET`'s `CS` at the
TSS. This only works because the GDT is ordered kernel-code, kernel-data,
user-data, user-code with no gaps, which is why that order was fixed in the very
first descriptor-table commit.

**`FMASK` clears `IF`.** Syscalls run with interrupts off, which is what lets a
single kernel stack be shared between the syscall path and interrupts taken in
ring 3 (those arrive via the TSS `rsp0`): the two never overlap in time.

The test program is a real ring-3 ELF. It `write`s a line, then exits with
`.rodata + .bss` as its code, so a wrong value still separates "`.bss` not
zeroed" from "segment never mapped" — and reaching ring 3 to make the calls at
all is the rest of the proof. That it is genuinely unprivileged was confirmed
by temporarily giving it a `cli`, which faults with `#GP` at CPL 3 where it
would silently succeed at CPL 0.

## Userland

Programs reach the filesystem through a small set of syscalls - open, close,
read, write, lseek, stat, getdents, chdir, getcwd, mkdir, unlink - over a
per-process file-descriptor table carried in the task and inherited across
fork. The VFS is path-based, so a descriptor is just a remembered path and a
cursor. fd 0/1/2 are the console; open() returns higher numbers.

execve passes argv: the strings are copied into the kernel before CR3 is
switched (they live in the old space), then laid out on the new program's stack
the way _start expects - argc, the pointers, a NULL, then the strings - and
crt0 reads them back and calls main(argc, argv).

That is enough for a shell and real commands. `sh` reads a line (the console
echoes as you type), splits it, runs cd/exit/help itself, and forks+execs
the program that name finds along the command path for anything else. `ls`,
`cat`, `echo` and `pwd` are ordinary C
programs against the libc. The init process scripts a few of them as a demo -
so a headless boot verifies the whole path without a keyboard - then execs the
shell.

## Processes

`fork`, `execve`, `wait` and `exit` are real. A task is a schedulable entity
with a kernel stack and an address space; a kernel thread runs a C function in
the kernel's space, a user process runs in ring 3 in its own. On a context
switch the scheduler loads the incoming task's page table (`CR3`) and its
kernel stack into the TSS - so an interrupt or syscall taken while a process
runs lands on that process's own kernel stack, which is what lets a syscall
that blocks (`fork`, `wait`) keep its state without another task clobbering it.

`fork` deep-copies the parent's user pages into a new address space and creates
a child task whose first trip to ring 3 restores a copy of the parent's syscall
register frame with `rax = 0` - so the child returns from `fork` exactly where
the parent did, seeing zero. `execve` builds a fresh address space from an ELF,
swaps it in under the current task, frees the old one, and rewrites the syscall
frame so `SYSRET` lands in the new program. `exit` tears the address space down
and leaves a zombie holding the exit code; `wait` reaps it.

The init process demonstrates the lot: it forks, the child `execve`s the C
hello program, and init `wait`s and reports the child's exit status - the whole
cycle a shell runs on.

One sharp edge, found the direct way: `execve`'s `path` argument points into the
caller's address space, so it must be copied into the kernel before `CR3` is
switched to the new space - otherwise the first dereference after the switch
faults on an address the new space has never heard of.

## Address spaces

Each process runs in its own top-level page table. A new space is the kernel's
own PML4 copied entry-for-entry, then the process adds its own mappings in the
slots the kernel left empty. Because the copied entries point at the kernel's
sub-tables, the kernel's code, heap and identity map are shared into every
space by reference - which is what lets a syscall or interrupt taken while a
process runs still find the kernel without switching `CR3` back first. Only the
slots a space added are private, and only those are freed when it is destroyed;
freeing a shared kernel sub-tree would unmap the kernel out from under every
other process.

The kernel lives entirely in the higher half. Its physical mappings are a
**direct map** - every byte of RAM at `phys + 0xFFFF800000000000` - which is how
page-table code reaches any frame: entries store physical addresses, and a
walk goes through the direct map to read them. The heap and the kernel image
sit in their own high-half slots. That leaves the whole low canonical half to
user space, so programs link at the conventional `0x400000` and need only the
small code model.

Bringing the direct map up is a small bootstrap dance: the tables are built
while the stage-2 identity map is still live (so a physical address is still a
usable pointer), then `CR3` is loaded and a flag flips so every later physical
dereference goes through the direct-map offset. The frame bitmap and the
framebuffer, both reached by physical address during early boot, are re-pointed
onto the direct map at the same moment.

Isolation is checked directly: a program runs, exits, and the kernel confirms
the addresses it used are no longer mapped in the kernel's space. Running it a
second time - reusing the freed frames and the same addresses - is where a
teardown bug would show up.

## Scheduling

A round-robin preemptive scheduler over kernel threads, all sharing the kernel
address space for now. `context_switch` (context.asm) saves the SysV
callee-saved registers on the outgoing thread's stack, swaps `RSP`, and pops the
incoming thread's - so a thread's entire suspended state lives on its own stack.
A brand-new thread gets a hand-fabricated stack whose `RET` lands in a
trampoline.

Preemption is a timer tick charging the running thread's quantum. The switch
itself is deferred out of the timer handler to a point *after* the PIC has been
acknowledged - switching before the EOI would leave the PIC holding the line and
starve every other thread. Cooperative `yield()` uses the same machinery from
normal context.

The bug that cost the most to find here was not in the scheduler at all. The
ring-3 `exit` syscall returns to the kernel through a longjmp, and `SYSCALL`
masks `IF` via `FMASK`; nothing on that return path put it back. So every test
after the userspace one ran with interrupts disabled, the timer never fired, and
preemption looked broken when the real fault was three files away. `enter_user_mode`
now saves the caller's `RFLAGS` and the exit path restores it.

The verification does not trust wall-clock timing (TCG advances guest time
slowly under a full load of spinning threads). Instead, workers spin in tight
loops that never yield, and the test watches the interleave trace: a clean
`012012...` with every worker advancing is only possible if the timer is moving
the CPU between them.

## libc

User programs are freestanding C linked against leahOS's own libc, not the
host's. `crt0` gives `main` a C environment and turns its return into `exit`;
the syscall wrappers issue `SYSCALL` with the SysV register convention (`R10`
standing in for the `RCX` that `SYSCALL` destroys); `printf` formats into a
buffer and hands it to one `write`; `malloc` is a bump allocator over a `.bss`
arena until there is a `brk`/`mmap` syscall to grow a real heap.

One flag matters for the same reason it does in the kernel: `-mno-sse` keeps
GCC from emitting SSE, because ring 3 has no FPU/SSE state enabled and `movaps`
would `#UD` - found the direct way, by hitting exactly that on the first run.
Programs link low, at `0x400000`, so the default small code model is all they
need.

## Display

Stage 2 walks the VBE mode list and picks the largest mode that is no bigger
than 1024x768x32 and reports a linear framebuffer. Banked modes are rejected:
they need a bank switch every 64 KiB, which is not worth supporting when every
card since the 90s offers an LFB.

The font is not embedded in the kernel. Every VGA BIOS carries the 8x16 font it
uses for text mode, and `INT 10h AX=1130h` hands back a pointer to it — so
stage 2 copies it out before leaving text mode. It is the same typeface the
firmware already used, so switching to a framebuffer does not change what the
console looks like.

The console keeps both backends. If the mode set fails, `boot::Info` carries a
zero framebuffer address and everything falls back to the VGA text buffer,
which is what keeps the kernel debuggable on hardware where VBE does not
cooperate. The 16-colour palette is shared, so code written against either
backend renders identically.

Two ordering constraints, both learned the hard way:

The console comes up **before** the VMM, so early failures can be seen. That
means `framebuffer::init()` cannot map anything — it relies on the 4 GiB
identity map stage 2 builds, which exists precisely because a VBE framebuffer
sits in the MMIO window just under 4 GiB. `framebuffer::remap_as_device()`
tightens the attributes once the VMM is running.

A failed mapping falls back silently to the text buffer, which in graphics mode
means a black screen with a perfectly healthy serial log. Worth remembering as
a symptom.

## Shared memory

Two processes could not write to the same page until now. Everything shared
before this was shared by accident of a `fork` and then copied apart on the first
write; a segment is the opposite, and it is what a window server outside the
kernel needs in order to hand a client its pixels without copying them.

A segment is named by an **integer key** rather than a path — there is no `/dev`
or `/tmp` to hang a name off, and a well-known number is enough for two processes
to find each other. The first to ask for a key creates it; later ones get what is
already there.

Lifetime rides on the frame reference counts copy-on-write already needed: the
segment holds one reference per frame, every mapping takes another, and an
address space dropping its mappings on teardown releases them. Nothing has to be
told that a process died.

A segment belongs to the user who created it, and only that user and root can map
it — the same rule as a file. `SHM_PUBLIC` gives that up deliberately, which the
window server needs for its rendezvous block and nothing else uses.

Destroying one drops the segment's *own* reference and frees the key. Whoever
still has it mapped keeps the pages alive through theirs, so it is safe to
destroy a segment the other side is still reading. Without that a key could never
be reused — and reusing one would hand back a segment of whatever size it happened
to be created with, which is exactly what a window slot does when the next client
takes it.

Adding this exposed an older bug of the same family: `munmap` freed frames with
`pmm::free` rather than `pmm::release`, so unmapping a shared page handed it
straight back to the allocator while another process still had it mapped. The
copy-on-write work had already fixed the identical mistake in address-space
teardown; this was the second copy of it.

One sharp edge, deliberately left rather than hidden: a mapping **inherited
through `fork` is copy-on-write like anything else**, so a child that writes to it
gets a private copy and stops sharing. Clients map segments for themselves rather
than inheriting them, and the test suite says so explicitly.

## The window server

The window server is an ordinary process. It owns three things the kernel hands
it and nothing else: the framebuffer, the raw input devices, and a public shared
memory block that clients find by a well-known key. Everything a window *is* —
geometry, title, event queue — lives in that block, and its pixels live in a
segment of the client's own. Compositing is then reading other processes' memory
and writing the screen, with no kernel involvement beyond the page tables.

`FbMap` and `InputPoll` are **root only**, and that is the whole reason `login`
starts the server: mapping the framebuffer means being able to draw over anything
anyone is looking at, and polling input means seeing every keystroke. Clients run
as the logged-in user and never touch either.

There is no message passing. The protocol is reads and writes to one structure
both sides can see, with a small number of rules keeping it honest: a client owns
its slot and writes only geometry, title and a `present` counter; the server owns
the screen and the event rings; and slot allocation — the one genuinely contended
step, since `login` starts three clients at once — goes through a compare-and-swap
rather than a lock.

Moving out of the kernel cost two things and bought one. It cost a **rendezvous**,
hence the well-known key. It cost being **told when a client dies**: the server
has to notice for itself, which it does by asking — `kill(pid, 0)` delivers
nothing and reports only whether the process is still there, so a window whose
owner has gone is released rather than left as a corpse on the desktop. What it
bought is that a bug in the window manager is a dead process rather than a panic.

What it does *not* buy is the isolation the in-kernel version had. The control
block is public, so any user can read every window's title and geometry and could
write into another window's event ring. Pixels are not public — they stay owned by
the client that created them — so one user's windows cannot be read by another.
Fixing the rest needs per-client channels rather than one shared table.

### The desktop

`login` starts the desktop as soon as a password is accepted, and the session is
the desktop: close every window and the console comes back with a shell on it,
leave the shell and you are logged out. They cannot both be on screen — there is
one framebuffer and no way to switch virtual consoles — so they take turns, and
the console stops *drawing* while the server holds the screen. Serial output
carries on regardless, which is what keeps the boot log and any panic readable
from behind a desktop.

The screen is granted to a process and reclaimed when that process exits, however
it exits. A server that crashed returns the screen exactly as one that shut down
cleanly does; a desktop that died must not leave the machine with no visible
console.

Input works the way a window system has to. The topmost window is focused and
gets the keyboard. Pressing inside a window **grabs** the pointer, so a stroke
that runs off the edge stops rather than continuing into whatever is underneath.
Dragging clamps so the title bar — and with it the close box — can never leave the
screen, or a window could be dragged somewhere it can never be closed. Closing is a request the client can act on, not an eviction - whether it came
from the close box or from **Ctrl+Q**, which the window manager keeps for
itself and turns into exactly the same event. Keeping it there rather than in
each client matters twice over: a plain letter cannot be a quit key, because a
terminal or a text field has every right to it, and a shortcut invented
separately by each program would mean no two windows closed the same way.

### Resizing, and only redrawing what changed

Recomposing the whole screen to move a window is most of a megabyte of work for
a few hundred pixels of change, so each thing that changes now says which
rectangle it changed and a pass recomposes and blits only those. The rest of the
backbuffer stays correct from last time - which is also what lets the cursor be
erased by copying back out of it. The list merges greedily and collapses to a
bounding box when it fills: being imprecise costs a few redundant pixels, being
wrong costs a stale screen.

Resizing is a **grow box** in a bottom grip bar, a strip of its own rather than a
corner overlapping the content, so a click near the bottom-right of a window is
unambiguously a resize and never a stroke the client was expecting. Dragging it
draws a rubber-band outline and commits on release; committing every pixel of
movement would mean the client allocating and the server mapping a new segment
per frame.

The pixels belong to the client, so the server cannot resize them - it asks. The
server writes a requested size and bumps a sequence number; the client replaces
its segment under the next generation's key and bumps *that*. Shared memory has
no `realloc`, so this is allocate-and-swap, and the two generations have to be
able to exist at once: the server keeps drawing from the old pages until it
notices the new ones, which is why the key carries a generation as well as a
slot. Each side writes only its own fields, so there is no lock - only the
ordering, which is why each counter is stored last.

### A terminal

The desktop and the console no longer take turns. `login` opens a terminal
window, so the shell is one of the windows rather than something waiting for the
desktop to finish; anything else is launched from there.

Two pipes stand in for a terminal device - the shell's stdin on one, its stdout
and stderr on the other. The awkward part is that a pipe read blocks, and polling
the window and waiting for shell output both have to happen, so the read lives on
a **thread** of its own and they share a character grid under a mutex. There is
no line discipline on a pipe, so the terminal does the editing: keys are echoed
there and a line only reaches the shell on Enter. That is also why backspace
works, because nothing else in the path knows what one means.

Building it turned up five bugs that nothing before had been shaped to find,
because the terminal is the first program to combine threads, `fork` and shared
memory:

- **`fork` made shared memory copy-on-write.** A client that forked after opening
  a window carried on drawing into a private copy while the server composited the
  pages it had stopped writing to. Shared mappings now carry a bit that `fork`
  hands over intact - device memory too, or a forked child would get a private
  framebuffer.
- **The mouse never clamped to the screen.** The position kept accumulating past
  the edge, so a pointer pushed into a corner had to be dragged all the way back
  before it appeared to move again.
- **A user segfault panicked the kernel.** A ring-3 fault that cannot be resolved
  is the program's bug; it now dies on its own and the report says which one and
  where.
- **`exit` did not end a process's threads**, so anything threaded that returned
  from `main` left a process nothing could reap. It signals them instead of
  retiring them: a thread blocked inside a syscall is halfway through something,
  and marking it dead abandons that stack mid-call.
- **`pipe_read` was uninterruptible**, so a killed process blocked on a pipe could
  never die - going back to sleep on a wake is how a process becomes unkillable.

A sixth was hiding behind those: a thread group's open files live in the leader's
slot, and `group_leader` skipped a leader that had become a zombie - sending every
surviving thread to its own empty table copy, so the real table was never closed
and its pipes never released. The leader's slot is now held until the whole group
is gone. Only the *leader's*: holding back an ordinary thread's slot makes
`thread_join` wait for a process that is still running, which is a hang rather
than a leak.

Two bugs came out of running the damage work in anger, both of which look like
graphics faults and are neither.

A window's pixels are found by a key that carries a **generation** as well as a
slot, so that a resize can replace the segment without the old and new
colliding. The path that maps a window for the *first* time still used the old
flat key, which agrees with the generational one only for slot zero - so the
first window worked and every one after it silently never mapped. The symptom
was not "no window": it was a window that appeared the moment something else
was moved or resized, because the resize path used the right key. A key nobody
created reads exactly like a client that has not finished starting, which is
why it waited quietly instead of failing.

The cursor is drawn straight to the screen and rubbed out of the backbuffer,
which means it has to come off before anything else is painted. It was being
erased only when *nothing else* had changed - so any frame that both moved the
pointer and repainted a window left the old cursor behind. While dragging that
is most frames, and the result was a trail of arrows across the desktop. The
erase is now unconditional and happens first.

`uitest` exists to make this kind of thing visible: one window holding raised
and sunken bevels, buttons that depress under the pointer, a checkbox, radio
buttons, a colour bar, and a live readout of the last event and the current
size. There is no widget toolkit behind it - a client owns a rectangle of
pixels and draws it itself - and showing that plainly is more useful than
hiding it.

### Application bundles

A `.app` is an ordinary directory whose name carries the extension:

    /opt/Paint.app/
        Info            name, exec, opens, menu
        paint.elf

The point is not the packaging. It is that an application had been "a binary in
a binary on the command path plus rules scattered through whoever launched it":
the browser carried a
hardcoded rule that a program runs and everything else goes to the editor, and
each client wrote its own context menu. Both of those facts belong to the
application, and a bundle is where they now live. Nothing outside reads one
except through `bundle.h`.

`Info` is the same `key value` line format as `~/.leahrc`, for the same reason -
`cat` reads it and the editor repairs it. `opens .PNG .GIF` is how a document
finds its application without the browser knowing anything about file types; a
remembered "always open with" still wins, because that was the user saying so
explicitly rather than the system inferring it.

Opening a bundle runs it rather than descending into it, which is the whole
difference between an application and the directory it is made of. Its contents
are not hidden - nothing pretends the directory is a file - they are simply not
what opening it means.

Every application ships **only** as a bundle. There is no copy on the command
path,
deliberately: two copies of a binary are two things to keep in step, and the
second one is exactly what lets a caller carry on hardcoding a path to another
program instead of asking which application does the job. `/bin` keeps the
command-line
tools and the pieces the system starts before any of this exists - `init`,
`login`, the shell, the window server, the desktop.

Nothing outside a bundle names an application's path any more. `app_path("Edit")`
walks `/opt`, and an application that is not installed returns an empty string,
which every caller treats as "then do not launch it" - a missing application
should be a launch that does nothing rather than a launch of the wrong thing.
The open-with dialogue lists what it finds rather than the three names that used
to be written into it, which were already wrong by the time there were ten.

An application's `menu` lines become its entries in the browser's right-click
menu, and are handed back to it as an argument when chosen: the system does not
know what "New drawing" means, and should not - only Paint does.

The bundles are staged into the ext image by `tools/mkext.sh`, because a bundle
is a filesystem layout and the image is where the filesystem is laid out. The
icons are generated by `tools/mkicon.py` rather than drawn, because there is no
icon editor here and a bundle that declares an icon it does not have is worse
than one that declares none.

### Browsing and editing

`browse` shows one directory three ways, because they answer different
questions: icons for what is in here, a list for how big it is, a tree for
where it sits. They share a selection and a current directory, so switching
never loses your place. Opening is one gesture everywhere - click to select,
click the selection again, or press Return - and what happens depends on what
was opened: a directory is entered, an executable is run, and anything else is
handed to `edit`. That last rule is a rule about the **name**, not the contents,
because nothing in the filesystem records a type; a real one is on the list
above.

The tree is built **iteratively** rather than by the obvious recursive walk. A
`struct dirent` is 144 bytes and a directory buffer holds sixty-four of them,
so recursion costs nine kilobytes of stack per level - which overran the user
stack a few levels down and killed the process. Expanding in passes, splicing
each opened directory's children in after it, needs one buffer however deep the
tree goes.

`edit` keeps its buffer as one flat array with the newlines left in, rather than
an array of lines: insertion is a memmove and saving is a single write, at the
cost of recomputing where the lines start after every edit. For files this size
that is free, and it is far less to get wrong than keeping two representations
agreeing.

Both draw through `widget.h`, which is the handful of operations - fill, bevel,
clipped text, a button - that every client turned out to need. It is not a
toolkit: a client still owns a rectangle of pixels and draws it itself. It
exists because four programs were otherwise going to carry four copies of the
same bevel routine.

### Writing images

`paint` saves real PNG and GIF files, and neither is compressed. PNG's deflate
stream is emitted as **stored blocks**, which the format explicitly allows, and
GIF's LZW clears its table before it ever fills - the standard way to produce
valid LZW without implementing a dictionary. Both are larger than they need to
be and correct, which is the right trade for a system with no compressor: a
small wrong file is worth nothing.

Both encoders are pure C with no dependency on the OS, which means they can be
compiled on the host and checked against a real decoder. That is how the one
genuine bug in them was found: the colour channel was derived from the index
*within the deflate block*, and a block boundary does not fall on a row
boundary, so everything after the first row was shifted. Structurally the file
was perfect - signature, CRCs, adler-32 all correct - and it decoded to
nonsense. It is now checked end to end: `zlib.decompress` validates the stream
and the checksum, and every pixel is compared against what was encoded,
including a 400x200 image spanning several stored blocks, and the GIF's LZW is
decoded back to indices.

JPEG is **not** implemented. It needs a discrete cosine transform, quantisation
tables and Huffman coding - a different order of work from the two above, and
not something to claim without writing and testing it.

`imgview` reads back what paint writes, which is the same subset: 8-bit
truecolour, no interlacing, stored deflate blocks. It says so plainly when a
file falls outside that rather than showing noise.

### Dialogues

There are no child windows - the server knows about top-level windows and
nothing else - so a dialogue is an overlay an application paints over its own
content, and modality is a rule the application keeps rather than one the system
enforces. It is a state machine rather than a function that runs its own loop,
because a client still has to answer the server about a resize or a close while
one is open.

Saving asks **where**. Nothing writes to a path it chose for itself any more:
paint and the editor both open a save dialogue with a directory list and a name
field. The editor asks every time rather than silently overwriting whatever it
was handed, which is a good way to lose a file you only meant to look at.

Opening asks **what with**. The file's name is the only hint this system has
about its type, and a hint is not good enough to decide for someone, so the
browser offers the editor, the image viewer, paint, and - when the file is a
program - running it directly.

### Shortcuts

There are no symbolic links in this filesystem, so a shortcut is a small text
file whose first line is the path it stands for, named `.alias`. Opening one
opens what it names, and the extension is hidden in the label because the point
of a shortcut is the thing, not the file.

A file rather than a filesystem feature deliberately: a link needs the kernel
and the on-disk format to agree about it, and this needs neither. It is readable
with `cat` and repairable with the editor - the same bargain `Info` and
`~/.leahrc` make. Every account's desktop starts with three: Files, Notepad and
the README.

### Watching the system

`taskman` shows a snapshot the kernel copies out under its own lock - a reader
walking the live table would see slots change as tasks come and go. Threads
appear alongside processes, indented under their group, because that is what
they are here and hiding them would misreport where the time is going.

Its "CPU" column is **share of scheduler slices between refreshes, not a duty
cycle**. This system has no per-task clock; the slice count is the honest thing
it does have, and it is labelled as a share rather than dressed up as a
percentage of wall time. Samples are matched by pid between refreshes, so a
task that exits does not make whichever slot it vacated look enormously busy.
The strip chart deliberately excludes the idle tasks - on a quiet machine they
are nearly all the slices, and charting them would draw a flat line at 100%.

Right-clicking is delivered as a `MOUSE_DOWN` with button 2 and, unlike a left
press, does **not** raise or focus the window: a right-click is a question about
something, not a decision to work in it. The menu itself is another overlay, for
the same reason dialogues are - there are no popup windows. It is kept separate
from the dialogue state because choosing from a menu is very often what raises a
dialogue.

The open-with dialogue now has an **always** box. The association is held for
the session only: there is no per-user settings store to write it to, and
inventing a file format for three associations would be worse than being clear
that they do not outlive a logout.

### Appearance

The desktop's palette is no longer fixed in the server's source. It lives in the
control block, alongside the window table, because the server is the only thing
that can act on it and a setting nothing acts on is decoration. Settings writes
a colour and bumps a generation; the server notices on its next pass, reloads
the wallpaper if that is what moved, and damages the whole screen.

A wallpaper is a PNG, decoded by the same `img_read_png` the image viewer uses -
the reader was moved into the libc when the server needed it, rather than
copied. It is sampled nearest-neighbour to the screen size, which is a stretch
and is described as one.

None of it is persisted. There is no per-user settings store, so the palette
lasts as long as the desktop session and the About page says so rather than
implying otherwise.

### Users and groups

Creating an account, setting a password and changing a home directory's mode
all go through the account syscalls, so the kernel decides who may do what: only
root creates accounts, and a new one gets the next free uid rather than a
supplied one - which is what stops a new account silently becoming an existing
user. "Permissions" here means the mode of the home directory, because that is
what this system actually has to offer; it is labelled as such rather than as
something grander.

### The desktop, and things that cross windows

The **clipboard** is a shared memory segment under a well-known key, exactly
like the server's control block - which is the only cross-process channel this
system has, and the right one, because a clipboard *is* state that outlives
whoever filled it. It is public, and that is worth naming: with two people
logged in, one could read the other's copied text. Fixing it needs a
per-session clipboard, which needs a session this system does not have.

**Desktop icons** cost the server about thirty lines rather than a new concept.
A client sets `WS_FLAG_DESKTOP` and the server draws it without chrome, keeps it
last in the order, and refuses to raise or drag it. Everything else - pixels,
events, right-click - works as any window's does. Because it covers the screen
it also paints the backdrop and the wallpaper, so exactly one thing is
responsible for what is behind the windows rather than two drawing over each
other.

**Preferences** are `key value` lines in `~/.leahrc`: readable with `cat`,
repairable with the editor, which at this size is worth more than compactness.
Keys a program does not recognise are written back unchanged, so an older build
cannot silently discard a newer one's settings.

The compositor **sleeps** between passes rather than spinning. That is not a
politeness: a polling loop that only ever yields stays runnable, holds the kernel
lock over and over, and on a multiprocessor can keep another CPU out of the kernel
entirely — including the one every device interrupt is routed to.

## Networking

The NIC is QEMU's Intel 82540EM ("e1000"), found by PCI class. The driver maps
its register window as uncached device memory, resets the card, reads its MAC
out of the receive-address registers, and sets up DMA descriptor rings for
receive and transmit — the buffers come from physically contiguous frames
reached through the direct map, since the card DMAs to physical addresses.

Receive is **polled, not interrupt-driven**: with no BIOS to program PCI
interrupt routing, the card's reported IRQ line cannot be trusted, so `poll()`
walks the receive ring straight from the descriptor-done bits. The poll loop
executes `hlt` between reads. That matters more than it looks: a tight busy-poll
starves QEMU's host-side network backend and nothing is ever delivered, and the
guest's virtual timer fast-forwards across a `hlt`, so waits are counted in poll
iterations rather than timer ticks.

On top of the driver sits a small IPv4 stack in `kernel/net/`: Ethernet framing,
an ARP cache with request/reply, IPv4 with the RFC 1071 checksum, ICMP echo (it
answers pings and can send them), and UDP carrying a minimal DNS resolver that
asks QEMU's DNS proxy for A records. Off-subnet destinations are routed via the
gateway. Syscalls expose it to userland — `netinfo`, `ping`, `arp` and
`resolve` — behind the `ifconfig`, `ping`, `arp` and `nslookup` commands; `ping`
takes a hostname as well as a dotted quad.

One wrinkle is worth calling out, because it cost some debugging. These
operations block in the kernel, polling the ring until a reply arrives, and a
`SYSCALL` enters with interrupts masked (the FMASK MSR clears `IF`) — so a naive
`hlt` in the wait loop would never wake. Each wait therefore both **disables
preemption** (nothing else drains the NIC, so being scheduled away mid-wait
would strand the reply) and **re-enables interrupts** (so the timer can wake the
`hlt`). A synchronous reply such as an ICMP echo hides the problem — QEMU injects
it during the send, before the loop ever halts — but an asynchronous one like a
DNS answer needs both. Everything is verified against QEMU's user-mode network
(guest `10.0.2.15`, gateway `10.0.2.2`, DNS `10.0.2.3`).

## Processes, threads and signals

A **thread** is a task that shares its creator's address space and open-file
table instead of copying them; `clone` takes an entry point, an argument and a
stack that libc has already `mmap`ed, which keeps thread stacks out of the
kernel's bookkeeping entirely. Every thread of a process carries the same
`tgid`, and the shared state - fd table, `brk`, the mmap cursor - is read
through the group leader, so two threads can never be handed the same address.
The address space is freed by whichever thread leaves last, not the first one
out; `getpid` returns the process, `gettid` the thread.

**Signals** are delivered on the way out of a syscall, because that is the one
place the kernel holds the full user register state and can splice a handler in
front of the interrupted code. Delivery pushes the saved context onto the user's
own stack, then a return address pointing at a trampoline libc registers with
the first `signal()` call - the kernel cannot put that code on the stack itself,
since the stack is mapped non-executable. When the handler returns it lands on
the trampoline, which calls `sigreturn`, and the kernel restores the context
wholesale (including `rax`, so an interrupted syscall's return value survives).
Sending a signal wakes a blocked target so it reaches that check; a process that
never enters the kernel will not see one until it does. `SIGKILL` is refused at
`signal()` and again at delivery, so it cannot be caught.

**Users and permissions** are what the ext root filesystem was groundwork for.
Each task carries a uid and gid, inherited across `fork` and `execve`; uid 0 is
root and bypasses the checks. `open` consults the inode's real mode and owner,
applying the usual UNIX rule that only the first matching class counts - so mode
0007 really does lock the owner out. Only root or a file's owner may `chmod` it,
only root may `chown`, and dropping root is one-way. `id`, `chmod`, `chown` and
`su` expose it; there is no password file yet, so `su` enforces exactly what the
kernel does and no more.

## TCP

Enough TCP to be a real client: the three-way handshake, sequence and
acknowledgement tracking, a receive buffer, retransmission on timeout, and an
orderly four-way close. Not present: congestion control, window scaling,
selective acknowledgement, out-of-order reassembly, and passive open. A segment
arriving out of order is **dropped rather than queued** — that costs a
retransmission and keeps the state machine honest, where a half-built
reassembler would not.

Sending is stop-and-wait: one segment in flight, held until acknowledged. A real
window would keep several going, but this keeps the sequence bookkeeping small
enough to be obviously correct.

A connection is a **file descriptor**, so `read`, `write` and `close` work on a
socket exactly as they do on a file or a pipe — the fd carries the connection
handle where a file would carry its offset. `fetch` is the demonstration and the
end-to-end test in one: DNS resolves the name, ARP finds the gateway, TCP opens
a connection through it, and the reply arrives through the same descriptor
machinery as everything else.

The checksum covers a pseudo-header of the source and destination addresses as
well as the segment, which is what ties a segment to the addresses that carried
it; one delivered to the wrong host fails the check rather than being accepted.

## USB

xHCI, and only xHCI. USB went through UHCI, OHCI and EHCI, each with its own
registers and its own idea of how a transfer is described; xHCI replaced all
three with one model. The driver builds rings of Transfer Request Blocks in
memory, rings a doorbell, and reads completions off an event ring — control,
bulk and interrupt transfers are the same mechanism with a different TRB type.

Bring-up is the device context base address array, a command ring, an event ring
(reached through a segment table rather than directly), then each populated port
gets reset, a slot, an addressed device context, and a `GET_DESCRIPTOR`. A
device class of 0 is the normal case and means "look at the interface", so the
configuration descriptor is read as well — that is also where the endpoint
addresses come from.

**Mass storage** is the bulk-only transport: a 31-byte Command Block Wrapper out
on the bulk endpoint, the data, then a 13-byte status wrapper back. Inside is
ordinary SCSI — the same `INQUIRY` and `READ(10)` a SATA disk answers. Verified
by a round trip rather than a status check, like AHCI.

**HID** uses the boot protocol. A HID device normally describes its reports with
a bytecode descriptor that must be parsed to know what any bit means; boot
protocol exists to avoid exactly that, and a keyboard in it always sends the
same eight bytes. `SET_PROTOCOL` puts it there.

Two structural points worth stating. The first: an interrupt endpoint **cannot
be waited on**. A keyboard completes its transfer only when a key changes, so a blocking
read would hang the console until someone typed. Those transfers are posted and
collected later, and because the event ring is shared, a completion belonging to
a posted interrupt transfer is filed against it rather than handed to whichever
control transfer happens to be waiting — without that, a control transfer
consumes the keyboard's completion and calls it its own.

The second: the poll has to be driven by the **timer**, not by whoever is
reading. Doing it in the console read path looks right and deadlocks — the
reader polls once, finds nothing, and sleeps on the keyboard channel, which is
woken by the keypress that only the poll would have discovered. Nothing else
wakes it, so the first key after a read begins is lost and the shell hangs.

## AHCI

The ATA driver moves every sector through the CPU a word at a time — that is
what PIO means, and it is why reading a megabyte costs a megabyte of `IN`
instructions. AHCI hands the controller a command list and a scatter/gather
table in memory and lets it DMA straight to and from RAM: the CPU writes one bit
to start a transfer and reads one bit to see it finish.

The driver finds the controller by PCI class (mass storage / SATA), takes its
registers from **BAR5** rather than BAR0, enables bus mastering, and switches
the controller out of legacy IDE emulation into AHCI mode. Each implemented port
is checked for a live link and a plain-disk signature, stopped, given its
command list and FIS receive area, restarted, and identified.

Commands are issued in slot 0 and polled. With one request in flight there is
nothing for a queue to do, and polling keeps the driver out of the interrupt
path entirely. A port has to be genuinely idle before its base addresses are
written — both the command-list and FIS engines actually stopped, not merely
asked to.

The self-test is a DMA round trip rather than a status check, and deliberately
so: a wrong physical address in the scatter/gather table does not raise an
error, it reads back someone else's memory. It also runs a transfer larger than
the driver's DMA buffer, so the chunking loop executes more than once and the
second chunk's LBA has to be right.

## Bringing up the other processors

An x86 machine boots one core. The rest sit halted until the bootstrap
processor sends an INIT followed by a startup IPI, at which point each begins in
**16-bit real mode** at a page the SIPI names — no GDT, no paging, no long mode.
`boot/ap_trampoline.asm` walks each one up the same road stage 2 walked the
first CPU, then jumps to a 64-bit C function.

The trampoline lives at physical `0x8000`, because a startup IPI carries a page
number rather than an address and so cannot reach above 1 MiB. That is stage 2's
old load address, long finished with, and the frame allocator already reserves
the whole first megabyte.

Two things caught us out. The kernel's page tables map **nothing** in the low
half, so the trampoline's own instructions would fault the instant it enabled
paging — it needs a temporary identity map. And that map must then be torn down
*completely*, tables and all: `create_address_space` copies the kernel's PML4
entries into every new process, so a leftover low-half entry is inherited as
though it were kernel-shared, and every user space then builds its mappings
inside one shared page table. The symptom was a forked child's writes showing up
in its parent — copy-on-write looking broken when the fault was in SMP startup.

The application processors **run tasks**. Each takes its own GDT and TSS, its own
per-CPU block, its own local APIC and timer tick, an idle task of its own, and
then enters the scheduler. Processes are genuinely distributed: three spinning
workers land on three different processors and make comparable progress.

Getting there was mostly a hunt for **state that describes a processor but was
stored once for the machine**. Every one of these was invisible on a single core
and fatal on two:

- **Which CPU am I?** was a global written when the kernel lock was taken, on the
  reasoning that only one CPU is inside the kernel at a time. That is not true —
  a context switch hands the lock off, and a CPU switching to a task that never
  held it carries on in the kernel with the lock released. Another CPU then owns
  the global while this one is still reading it, so the scheduler returned *that*
  CPU's current task and two processors ran the same one. It reads out of `GS`
  now, which cannot be wrong.
- **The kernel lock's recursion depth**, same story: two CPUs decrementing one
  counter.
- **Which address space is loaded** was cached in a global so a redundant `CR3`
  reload could be skipped. A second CPU switching to the same space skipped its
  own reload and kept running on another process's page tables. `CR3` is a
  register; it is now simply read.
- **`EFER.SCE`, `STAR`, `LSTAR`, `SFMASK`, and `CR0.WP`** are per-processor and
  only the boot CPU ever set them. A task that migrated hit `#UD` on its next
  `syscall` — the opcode is not legal without `EFER.SCE` — and, with `CR0.WP`
  clear, copy-on-write quietly stopped working on that core. The application
  processors also came out of the trampoline with `CR0.CD` and `CR0.NW` still
  set, running uncached.

Reaching per-CPU state at all needed **SWAPGS on every ring transition**, which
had to go in as one piece. `SYSCALL` arrives with no free register — `RSP` still
points into user memory and every general register holds user data — so the
kernel stack to switch to has to come from somewhere reachable without one. That
is what `GS` is for: one instruction turns it into this processor's block, and
`gs:[8]` is its stack. A global names a single stack for the whole machine, which
is only ever right on a uniprocessor.

Half of that discipline is worse than none. An interrupt can arrive from either
ring, so its swap has to be conditional on the saved `CS` — swapping
unconditionally would install the *user's* `GS` for a handler that interrupted
kernel code. And the one path that is assembly all the way to `IRETQ`, a task's
first entry into ring 3, cannot use `SWAPGS` at all: the segment loads it has to
do reset `GS_BASE` from the descriptor and would throw the block away, so it
writes `IA32_KERNEL_GS_BASE` explicitly instead.

Two genuine races, rather than misplaced state:

**A task must not become runnable until its context is actually saved.**
`switch_to` marked the outgoing task `Ready` and then handed off the lock, both
*before* `context_switch` had stored its stack pointer. Another CPU could pick it
up in that window and resume it from a stale `kernel_rsp` — two processors on one
kernel stack, which showed up as a register dump full of stack addresses. The CPU
taking over now does it, in `finish_switch`, once the context really is saved;
tasks carry an `on_cpu` flag so nothing can be picked up mid-flight.

**`fork` leaked the kernel lock.** The child was given a lock depth of one, on the
grounds that it "resumes inside a syscall, as the parent does" — but it does not.
It is fabricated to enter ring 3 through `first_user_entry`, which goes straight
to `IRETQ` and never reaches the release that would balance it, so the child
carried the lock into user mode and never gave it back. On one processor that is
invisible: the CPU holding it just re-enters recursively. On two it is fatal, and
the symptom was not what you would guess — every device interrupt is routed to
one processor, so the machine lost its keyboard and mouse the first time
anything forked.

That last one also needed a change to how the lock is waited for. A CPU spinning
for it holds nothing, so masking interrupts while it waits protects nothing — and
a syscall enters with `IF` clear, so waiting the way the caller arrived meant the
boot processor could sit there ignoring every key and mouse packet. Waiting now
happens with interrupts **on**, which is safe precisely because the lock is not
held yet: every check-then-block sequence in the kernel runs from inside a
syscall, where the lock is already held and the acquire is a nested one that
never spins.

TLB shootdowns needed the same kind of rethink. A CPU waiting for the kernel lock
inside a syscall could not take the shootdown IPI, and the CPU holding the lock
sat waiting for an acknowledgement that could never arrive — a real deadlock that
the sender's timeout merely turned into a crawl. Acknowledging is now idempotent
and **pollable**: a generation counter the receiver compares against its own, so
a processor that cannot be interrupted can still answer from inside a spin loop.

## Interrupt controllers and timekeeping

The PIC and PIT are what the machine hands you at boot, and both are now
retired in favour of what ACPI describes.

**ACPI**, in `kernel/acpi/`, goes only as far as it needs to: find the RSDP by
scanning the EBDA and the ROM area for its signature (checksummed, so a chance
match is rejected), follow it to the XSDT or RSDT, and pick out two tables. The
MADT gives every CPU's local APIC id, the I/O APICs, and the interrupt source
overrides; the HPET table gives a register block. There is no AML interpreter
and there does not need to be — nothing here requires executing bytecode.

**The APIC** splits the PIC's job in two. An I/O APIC routes each device
interrupt to a chosen vector on a chosen CPU; a local APIC per core receives
them and provides that core's own timer. That per-core half is the part SMP
cannot do without. Two details matter more than they look: the *source
overrides* must be honoured, since a machine that wires the timer's IRQ 0 to
GSI 2 will silently deliver nothing if you ignore them, and the global enable in
`IA32_APIC_BASE` is separate from the software enable in the spurious vector
register.

**Timekeeping** now has two clocks. The HPET is a free-running counter of known
frequency — the thing the PIT never was — and `uptime` reads it directly, so
resolution is microseconds rather than a scheduling quantum. The local APIC
timer raises the scheduling tick on the same vector the PIT used, so the tick
handler did not change; only the source did. Its rate is not architecturally
known, so it is measured: count it down for a fixed interval against the HPET,
or against PIT channel 2 when there is no HPET. That fallback is not
hypothetical — QEMU disables the HPET by default, and the first boot after
masking the PIC hung precisely because calibration had nothing to measure
against.

## Accounts and authentication

There are real accounts now — `root`, `leah` and `guest` — each with a uid, a
gid and a home directory, and a password that has to be right.

**The kernel does the checking.** A conventional UNIX keeps hashes in
`/etc/shadow`, readable only by root, and lets an unprivileged `su` reach them
by being setuid-root. There is no setuid-on-exec here, and making the hashes
world-readable to work around that would give away the very thing worth
protecting. So authentication is a syscall: the kernel reads the shadow file
with the privileges it already has, checks the password, and switches the
caller's credentials only if it matches. The password crosses the syscall
boundary and no further, and no user process ever holds a hash. `su` needs no
special permissions at all.

`/etc/passwd` stays world-readable and holds only the public half — names, ids,
home directories.

**Hashing** is salted and stretched: `sha256(salt || password)` re-hashed 4096
times. SHA-256 is written out in `kernel/lib/sha256.cpp` because there is no
library to borrow one from, and it is checked at boot against the published
digest for `"abc"` — a hash that is subtly wrong still looks like a hash, and
would silently make every stored password unverifiable. The salt stops two users
with the same password sharing a digest; the repetition is what makes a guess
cost something. It is not memory-hard, so it is weaker than bcrypt against an
attacker with hardware — that is the honest limit of what fits here.

Two details that are easy to get wrong and were: the credential switch after a
successful login has to **bypass** the ordinary "only root may setuid" rule, or
a correct password is authenticated and then refused; and digests are compared
in constant time, since returning early on the first differing byte leaks how
much of a guess was right.

**Root is the hub.** Identity changes route through it: an ordinary user may
only climb to root, so reaching another ordinary user costs two passwords —
root's, then theirs. And root itself is asked for the target account's password,
so holding root is authority over the machine rather than knowledge of everyone
else's secrets.

Worth being precise about what that does and does not buy. It is a policy about
who you may *become*; it is not a wall around files. uid 0 still bypasses
permission checks, so root can read anyone's data directly — making it otherwise
would leave nobody able to administer the system. What the rule prevents is a
user quietly *becoming* someone else and acting as them.

Home directories are `0700` and owned by their user, so the ordinary permission
checks already stop one user reading another's files without going through that
route. `mke2fs -d` copies the host's mode and owner, so the image build fixes
both up with `debugfs` — a home directory anyone can read would make the whole
model decorative.

`login` runs from init and stays root: the shell it starts is a child that drops
to the authenticated user, so when that shell exits control returns to the
prompt. That is what makes `exit` a logout. `useradd` and `passwd` go through the
kernel for the same reason `su` does — the hash is never in a user process — and
`useradd` allocates the next free uid, because handing out one already in use
would not fail loudly, it would silently make two accounts the same person.

Passwords are in `tools/mkaccounts.py`, written down rather than pretended to be
secret: `root`/`toor`, `leah`/`leah`, `guest`/`guest`.

## Capabilities and handles

A handle is an index into a table the process does not own and cannot write.
That is the whole of "unforgeable": there is no bit pattern a program can invent
that names an object it was not given, because the naming happens on the other
side of the boundary. Each carries rights — read, write, execute, map, signal,
wait, duplicate, transfer — and `duplicate` may only ever narrow them, so a
process can hand out less authority than it holds and never more.

They travel over IPC by what they name rather than by number, because a number
means nothing outside the table it came from. The kernel resolves one in the
sender's table, installs the object it names in the receiver's, and writes the
receiver's own numbers back into the message.

What this bought first was a check that could not be made anywhere else.
`execve` is handed bytes and never learns which file they came from, so the only
place to enforce an execute bit was libc — the process's own code, checking
itself, which any program wanting to skip it simply would not call. `vfsd` issues
the image now: it has the file, its mode bits and the caller's credentials in one
place, and answers with a capability rather than with something the caller could
disbelieve. A file with mode 644 is refused, stays perfectly readable, and runs
the moment the bit is set.

Much of this system was already capability-shaped without the word. Port I/O
permission is one. A shared-memory id is a slot and a generation. `authd` owning
`/etc/shadow` so that nothing needs a setuid bit is the capability answer to
privilege, arrived at years earlier.

## Locking, and the order it goes in

There was one lock around the whole kernel, taken on entry to every system call,
which meant the kernel ran one call at a time however many processors it had. It
is off that path. Eleven subsystems have locks of their own — the frame
allocator, the page tables, the heap, shared memory, program images, capability
tables, ports, terminals, the descriptor table, the keyboard, the mouse.

Each carries a **rank**, and a processor may only take one whose rank is
strictly greater than everything it already holds. There is no cycle in a strict
ordering, so deadlock between ranked locks cannot happen; getting the order
wrong is a panic naming both locks rather than a hang once every few hundred
boots. The ranks read as a dependency graph: blocking subsystems first, because
a pipe with nothing in it holds its own lock and then asks the scheduler to
block; the scheduler next; memory after that, because a page-table walk
allocates frames and never the reverse; the console last, because anything may
print while holding anything.

What is left of the big lock is what it should always have been — the lock over
the task table and the run queue — and it stays on the *interrupt* path, because
that is where preemption happens. Taking it off there let two processors into
the same task, which the scheduler noticed and said so.

Two rules fell out and are enforced rather than remembered. No subsystem lock is
held across a call into the scheduler: collect what needs waking, drop the lock,
then wake. And no ranked lock is held across a context switch — these are spin
locks, and a sleeping holder leaves other processors waiting on something that
is not running.

Every one of these was found by the instrument rather than by reasoning. Seven
nested acquisitions, each a subsystem written with one of its functions in terms
of another; two subsystems calling the scheduler under their own lock, safe only
because the big lock happened to be held already. None would have crashed. All
would have hung, rarely, and been blamed on something else.

## Copy-on-write

`fork` used to deep-copy every page of the parent's address space. Now both
sides keep the same frames, mapped read-only with an OS-reserved bit (9) marking
them copy-on-write; the first write from either faults, and the fault handler
hands the writer a private copy. The physical allocator carries a small table of
reference counts holding only the *extra* references, so a singly-owned frame
costs nothing and needs no initialisation pass - absent means one owner.

Three details are what actually make it work, and each one broke something
first:

**CR0.WP has to be on.** Without it ring 0 may write through any mapping
regardless of its write bit, so a kernel routine filling a user buffer writes
straight into a page the child still shares instead of faulting. The symptom was
not a crash but forked processes quietly corrupting each other's stacks.

**"Was it writable?" is the wrong test when re-forking.** A page inherited from
an earlier fork is already read-only *and* CoW, so a second fork that only looks
at the write bit hands the new child a plainly read-only page whose first write
faults with nothing to resolve it. Only read-only *and* not-CoW is genuinely
read-only.

**Teardown has to drop a reference, not free.** `free_table_tree` freed data
frames outright, which handed shared frames back to the allocator while a live
process still mapped them.

## Roadmap

- [x] stage 1/2 bootloader, real → protected → long mode
- [x] freestanding C++ environment, VGA + serial console
- [x] GDT and TSS owned by the kernel rather than inherited from stage 2
- [x] IDT, exception handlers with register dumps, PIC remap, PIT, keyboard
- [x] physical frame allocator over the E820 map
- [x] virtual memory manager, 4-level page tables, NX
- [x] kernel heap, `kmalloc`/`operator new`
- [x] PCI enumeration
- [x] PS/2 mouse
- [x] unreal mode in stage 2 — kernel ceiling raised from 64 KiB to 8 MiB
- [x] ATA/IDE block driver, PIO, LBA28 and LBA48
- [x] VBE mode set in stage 2, linear framebuffer console
- [x] VFS layer and FAT32, read and write, including long filenames
- [x] a unified ext2/3/4 driver, read and write, extents and indirect blocks
- [x] an ext4 root filesystem on a second disk, verified with `e2fsck`
- [ ] exFAT
- [x] AHCI with DMA, alongside the PIO ATA driver
- [x] relocate the kernel to the higher half
- [x] ELF64 loading from the filesystem
- [x] ring 3, `syscall`/`sysret`, a first system-call ABI
- [x] preemptive scheduler, kernel threads, round-robin time-slicing
- [x] a freestanding libc: crt0, syscall wrappers, string/stdio/malloc
- [x] per-process address spaces (private page tables, CR3 switch)
- [x] `fork`, `execve`, `wait`, a process table, an init process
- [x] filesystem syscalls (open/read/write/stat/getdents/chdir…), a per-process fd table
- [x] a shell and coreutils: ls, cat, echo, pwd
- [x] more coreutils (cp, mv, rm, mkdir, touch, clear), pipes and redirection
- [x] a wait queue: tasks block on I/O instead of spinning
- [x] higher-half direct map: the low half is user's, programs link at 0x400000
- [x] `sbrk`-backed `malloc` that grows, and an atomic `rename`/`mv`
- [x] an e1000 NIC driver and an IPv4 stack: Ethernet, ARP, ICMP, UDP
- [x] a minimal DNS resolver, so `ping` and `nslookup` take hostnames
- [x] network commands: `ifconfig`, `arp`, `ping`, `nslookup`
- [x] `mmap`/`munmap` for anonymous memory
- [x] threads within a process: `clone`, a shared address space and fd table
- [x] signals: handlers, `kill`, `sigreturn`, default and ignored dispositions
- [x] users and permissions: uid/gid, `setuid`, `chmod`/`chown`, mode checks
- [x] futex-backed mutexes, so threads can synchronise
- [x] signal delivery on the interrupt return path, not just at syscalls
- [x] copy-on-write fork with reference-counted frames
- [x] demand paging for anonymous pages: `.bss` reserved, allocated on first touch
- [x] ACPI table discovery: RSDP, RSDT/XSDT, MADT and HPET
- [x] the local APIC and I/O APIC, with the PIC masked and the PIT retired
- [x] the HPET as a monotonic clock, and a calibrated local APIC timer
- [x] SMP: application processors brought out of reset into long mode
- [x] SMP: a locked scheduler, so those processors can run tasks
- [x] per-CPU state reached through GS, and a SWAPGS discipline on every ring transition
- [x] TCP and sockets, with `fetch` doing an HTTP GET
- [x] xHCI, with the HID and mass-storage class drivers
- [ ] USB interrupt transfers driven by the controller's own interrupt
- [x] a password file, `su` that authenticates, and per-user home directories
- [x] `login` at boot, `useradd`, `passwd`, `logout`
- [x] `ls -l` and `stat` for permissions and metadata
- [x] a window server and window manager: bevelled chrome, dragging, focus, a close box
- [x] shared memory between processes, keyed and reference-counted
- [x] the window server moved out of the kernel, onto shared memory
- [x] `login` starts the desktop; closing every window returns a shell
- [ ] per-client channels, so the rendezvous block need not be world-writable
- [x] window resizing, and damage rectangles instead of a full recompose
- [x] a terminal window, so the shell and the desktop need not take turns
- [x] `uitest`, a window of every element the desktop draws
- [x] `browse`, a file browser with icon, list and tree views
- [x] `edit`, a text editor, and what a document opens into
- [x] `calc`, `settings`, `imgview`, and a paint that saves PNG and GIF
- [ ] a deflate compressor, so PNG need not be written as stored blocks
- [ ] a JPEG encoder - it needs a DCT and Huffman tables, and is not written
- [x] `taskman`, with a resource monitor
- [x] right-click context menus, and an "always open with" that is remembered
- [ ] per-task CPU time, so the monitor can show a duty cycle not a share
- [x] settings with categories: general, appearance, network, users, about
- [x] the desktop's palette and wallpaper, changeable while it runs
- [x] a clipboard, scrollbars, rubber-band selection, desktop icons
- [x] per-user preferences in ~/.leahrc, and appearance restored from it
- [ ] drag and drop between windows, which needs the server to carry a drag
- [ ] a per-session clipboard, so two logged-in users cannot read each other's
- [x] application bundles: a `.app` directory that carries its own description
- [x] every application ships only as a bundle, with no copy on the command path
- [x] bundle `menu` entries in the browser's right-click menu
- [x] shortcuts on the desktop, as `.alias` files
- [ ] a bundle's `Icon.png` drawn in place of the generic application glyph
- [ ] a real type for a file, so opening one need not guess from its name
- [ ] reflowing a terminal's scrollback when it is resized
- [x] a microkernel: filesystem, drivers, network, audio, USB, PS/2 and auth in ring 3
- [x] pseudo-terminals as a kernel object, with a line discipline and process groups
- [x] a shell that is a language: `if`, `while`, `for`, `case`, functions
- [x] TCP that can be listened on, `accept`, and `httpd` serving files
- [x] `vi`, so a file can be edited without the window server
- [x] real timezones, parsed from TZif
- [x] a dock and a status bar, and windows that keep off them
- [x] dynamic linking: one `/lib/libc.so`, a `ld.so`, everything built `-pie`
- [x] program images held by the kernel and *mapped*, not copied, into each process
- [x] an auxiliary vector, so software written elsewhere can find its headers
- [x] file-backed `mmap`, private and copy-on-write
- [x] capabilities: typed handles with rights, passed over IPC by what they name
- [x] execute permission enforced by the filesystem, which is the only party that can
- [x] ranked locks, and the big kernel lock off the system call path
- [ ] demand paging for mapped files — written, not merged; see `s3-demand-paging`
- [ ] writeback, so a shared writable mapping can reach the file
- [ ] swap
- [ ] the intermittent stall: the desktop comes up and login's terminal does not
- [ ] reliable signals: `sigaction`, masks, `alarm`
- [ ] `fcntl`, `umask`, supplementary groups, resource limits
- [ ] sed, awk, cut, tr, xargs
- [ ] a self-hosting compiler
