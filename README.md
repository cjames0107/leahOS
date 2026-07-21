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
  fs/           vfs.cpp, fat32.cpp
  mm/           pmm.cpp, vmm.cpp, heap.cpp
  include/leah/ public headers
  lib/          string.cpp, cxx.cpp           freestanding runtime
  main.cpp      kernel_main
  linker.ld
tools/          run-headless.sh, mkfs_fat32.py
```

## Status

Boots to long mode, owns its descriptor tables, handles interrupts, manages
physical and virtual memory, enumerates PCI, reads and writes ATA disks, and
mounts a FAT32 filesystem it reads and writes. Faults produce a register dump
instead of a silent reset. No userland, so nothing runs but the kernel.

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
- [ ] VBE mode set in stage 2, linear framebuffer, bitmap font console
- [ ] APIC + HPET, retiring the PIC and PIT
- [x] VFS layer and FAT32, read and write, including long filenames
- [ ] exFAT and ext2/3/4
- [ ] AHCI with DMA, replacing PIO
- [x] relocate the kernel to the higher half
- [x] ELF64 loading from the filesystem
- [ ] ring 3, `syscall`/`sysret`
- [ ] scheduler and processes; `fork`/`execve`/`wait`
- [ ] USB: xHCI, then the HID and mass-storage class drivers
- [ ] libc and a userland shell
