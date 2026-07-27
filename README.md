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
| `CPUS` | `1` | `-smp`; nothing uses more than one yet |
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

Boots to long mode, owns its descriptor tables, handles interrupts, manages
physical and virtual memory, enumerates PCI, and reads and writes ATA disks. It
mounts an **ext4 root filesystem** (a unified ext2/3/4 driver, read and write)
and still reads and writes FAT32. It loads ELF programs and runs them in ring 3
over a `SYSCALL` ABI — as processes in their own private address spaces — and
time-slices them with a preemptive scheduler. Processes `fork`,
`execve` and `wait`, open files through a per-process descriptor table, and run
an interactive **shell** with real commands (`ls`, `cat`, `echo`, `pwd`, `cp`,
`mv`, `rm`, and more), pipes and redirection. An **e1000 NIC driver** and a
small **IPv4 stack** (Ethernet, ARP, ICMP, UDP, and a minimal DNS resolver)
bring up networking, with `ifconfig`, `arp`, `ping` and `nslookup` talking to
QEMU's virtual network — `ping example.com` resolves and reaches the host. User
programs are C linked against leahOS's own libc. Faults produce a register dump
instead of a silent reset.

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
| 20480+ | FAT32 partition (54 MiB) |

The partition table is written by `mkfs_fat32.py` rather than declared in
`stage1.asm`, so the image geometry has exactly one definition.

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
/BIN/<NAME>.ELF for anything else. `ls`, `cat`, `echo` and `pwd` are ordinary C
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
- [ ] APIC + HPET, retiring the PIC and PIT
- [x] VFS layer and FAT32, read and write, including long filenames
- [x] a unified ext2/3/4 driver, read and write, extents and indirect blocks
- [x] an ext4 root filesystem on a second disk, verified with `e2fsck`
- [ ] exFAT
- [ ] AHCI with DMA, replacing PIO
- [x] relocate the kernel to the higher half
- [x] ELF64 loading from the filesystem
- [x] ring 3, `syscall`/`sysret`, a first system-call ABI
- [x] preemptive scheduler, kernel threads, round-robin time-slicing
- [x] a freestanding libc: crt0, syscall wrappers, string/stdio/malloc
- [x] per-process address spaces (private page tables, CR3 switch)
- [x] `fork`, `execve`, `wait`, a process table, an init process
- [ ] USB: xHCI, then the HID and mass-storage class drivers
- [x] filesystem syscalls (open/read/write/stat/getdents/chdir…), a per-process fd table
- [x] a shell and coreutils: ls, cat, echo, pwd
- [x] more coreutils (cp, mv, rm, mkdir, touch, clear), pipes and redirection
- [x] a wait queue: tasks block on I/O instead of spinning
- [x] higher-half direct map: the low half is user's, programs link at 0x400000
- [x] `sbrk`-backed `malloc` that grows, and an atomic `rename`/`mv`
- [x] an e1000 NIC driver and an IPv4 stack: Ethernet, ARP, ICMP, UDP
- [x] a minimal DNS resolver, so `ping` and `nslookup` take hostnames
- [x] network commands: `ifconfig`, `arp`, `ping`, `nslookup`
