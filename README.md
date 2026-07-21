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
             │  int 15h E820     capture the memory map ──► 0x20000
             │  int 13h AH=42h   read the kernel ──► 0x10000
             │  lgdt, CR0.PE=1   ──► 32-bit protected mode
             │
             │  rep movsd        relocate kernel 0x10000 ──► 0x100000
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

**The kernel is staged at 0x10000, not loaded directly to 1 MiB.** `int 13h`
writes through a real-mode `segment:offset` pair, so it cannot name an address
above 1 MiB. The kernel lands in low memory and gets relocated by `rep movsd`
once 32-bit addressing is available. This caps the kernel at 64 KiB; unreal
mode will lift that when we outgrow it.

**Identity-mapped, not higher-half.** Every physical address the bootloader
handed out stays valid across the `CR0.PG` write, which makes the long-mode
transition boring. Moving the kernel to `0xFFFFFFFF80000000` is a job for
after the VMM exists.

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
| `0x10000`–`0x1FFFF` | kernel staging buffer (64 KiB) |
| `0x20000`–`0x20FFF` | E820 memory map |
| `0x70000`–`0x72FFF` | PML4 / PDPT / PD |
| `0xA0000`–`0xFFFFF` | video memory + BIOS ROM (untouchable) |
| `0x100000`+ | kernel |

## Layout

```
boot/           stage1.asm, stage2.asm, layout.inc
kernel/
  arch/x86_64/  entry.asm, console.cpp        architecture-specific
  include/leah/ types, io, console, string, bootinfo
  lib/          string.cpp, cxx.cpp           freestanding runtime
  main.cpp      kernel_main
  linker.ld
tools/          run-headless.sh
```

## Status

Boots to long mode, installs its own descriptor tables, and handles interrupts.
Faults produce a register dump instead of a silent reset. No memory management
yet, so there is still nothing to run.

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

## Roadmap

- [x] stage 1/2 bootloader, real → protected → long mode
- [x] freestanding C++ environment, VGA + serial console
- [x] GDT and TSS owned by the kernel rather than inherited from stage 2
- [x] IDT, exception handlers with register dumps, PIC remap, PIT, keyboard
- [ ] physical frame allocator over the E820 map
- [ ] virtual memory manager; relocate the kernel to the higher half
- [ ] kernel heap, `kmalloc`/`operator new`
- [ ] ELF loading, ring 3, `syscall`/`sysret`
- [ ] scheduler and processes; `fork`/`execve`/`wait`
- [ ] VFS, initrd, then a real block driver and filesystem
- [ ] libc and a userland shell
