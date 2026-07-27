# ============================================================================
# leahOS - run `make help` for the target list.
# ============================================================================

AS      := nasm
CC      := x86_64-elf-gcc
CXX     := x86_64-elf-g++
LD      := x86_64-elf-ld
OBJCOPY := x86_64-elf-objcopy
GDB     := x86_64-elf-gdb
QEMU    := qemu-system-x86_64

BUILD   := build
# Everything a run needs, collected in one folder: the bootable disk image, the
# kernel with symbols for gdb, and the logs a run produces. Intermediate object
# files stay under build/ proper; dist/ is only the things you would hand to
# QEMU or a debugger.
DIST    := $(BUILD)/dist
IMG     := $(DIST)/leahos.img
# The root filesystem lives on a second disk, an ext4 volume built by mke2fs.
# Disk 0 (IMG) stays the bootable/kernel disk; the kernel mounts this as root.
EXT_IMG := $(DIST)/ext.img
DIST_ELF := $(DIST)/leahos.elf
SERIAL  := $(DIST)/serial.log
QEMU_LOG := $(DIST)/qemu.log

# Must agree with boot/layout.inc.
STAGE2_LBA      := 1
STAGE2_SECTORS  := 32
KERNEL_LBA      := 64
KERNEL_MAX_SECTORS := 16384              # 8 MiB of image reserved for the kernel
IMAGE_MIB       := 64
FAT32_LBA       := 20480          # 10 MiB in, clear of the kernel's slot
EXT_MIB         := 64             # the ext4 root filesystem on disk 1

# --- knobs ------------------------------------------------------------------
# Override on the command line, e.g. `make run MEM=2G` or
# `make headless TIMEOUT=15`.
MEM        ?= 512M
CPUS       ?= 1
TIMEOUT    ?= 6
QEMU_EXTRA ?=

# --- toolchain check --------------------------------------------------------
# A missing cross-compiler otherwise surfaces as a bare "command not found"
# halfway through a build, which tells you nothing about how to fix it.
BUILD_TOOLS   := $(AS) $(CXX) $(LD) $(OBJCOPY)
MISSING_BUILD := $(strip $(foreach t,$(BUILD_TOOLS),\
                   $(if $(shell command -v $(t) 2>/dev/null),,$(t))))

ifneq ($(MISSING_BUILD),)
ifneq ($(filter-out help clean toolchain,$(or $(MAKECMDGOALS),all)),)
$(warning )
$(warning missing build tools: $(MISSING_BUILD))
$(warning install with: brew install x86_64-elf-gcc x86_64-elf-binutils nasm qemu)
$(warning )
$(error toolchain incomplete)
endif
endif

# --- compiler flags ---------------------------------------------------------
#
# -ffreestanding      no hosted libc, no assumptions about main()
# -mno-red-zone       interrupts would silently clobber the 128-byte red zone
# -mno-mmx/sse/sse2   we have not enabled the FPU or SSE state in CR0/CR4 yet
# -fno-pic            flat binary at a fixed load address
# -mcmodel=kernel     code lives in the top 2 GiB, so displacements fit in 32 bits
# -fno-exceptions     unwinding needs a runtime we do not have
# -fno-rtti           typeinfo needs the same
#
CXXFLAGS := \
	-std=c++23 \
	-ffreestanding -fno-builtin -nostdlib -nostdinc++ \
	-fno-exceptions -fno-rtti -fno-stack-protector -fno-pic -fno-pie \
	-mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-80387 \
	-mcmodel=kernel \
	-Wall -Wextra -Wno-unused-parameter \
	-O2 -g \
	-Ikernel/include

ASFLAGS_ELF := -f elf64 -g -F dwarf
ASFLAGS_BIN := -f bin -I.

LDFLAGS := -nostdlib -z noexecstack --build-id=none

# --- sources ----------------------------------------------------------------
KERNEL_CXX_SRCS := $(shell find kernel -name '*.cpp' | sort)
KERNEL_ASM_SRCS := $(shell find kernel -name '*.asm' | sort)

KERNEL_OBJS := $(KERNEL_ASM_SRCS:%.asm=$(BUILD)/%.asm.o) \
               $(KERNEL_CXX_SRCS:%.cpp=$(BUILD)/%.o)

KERNEL_ELF := $(BUILD)/kernel.elf
KERNEL_BIN := $(BUILD)/kernel.bin
# Userland programs. Each user/<name>.c links into $(BUILD)/<name>.elf and is
# placed on the image at /BIN/<NAME>.ELF (upper-cased for FAT's 8.3 names).
USER_PROGRAMS := init hello sh echo cat ls pwd mkdir rm touch cp mv clear \
                 ifconfig ping arp nslookup tests id chmod chown su
USER_ELFS  := $(USER_PROGRAMS:%=$(BUILD)/%.elf)
STAGE1_BIN := $(BUILD)/stage1.bin
STAGE2_BIN := $(BUILD)/stage2.bin

.PHONY: all image run headless debug gdb toolchain clean help
all: $(IMG) $(EXT_IMG)
image: $(IMG)

# --- bootloader -------------------------------------------------------------
# Both stages are raw binaries with a hard-coded ORG; there is nothing for the
# linker to do, so nasm emits them directly.
$(STAGE1_BIN): boot/stage1.asm boot/layout.inc | $(BUILD)
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS_BIN) $< -o $@
	@size=$$(stat -f%z $@); \
	 if [ $$size -ne 512 ]; then \
	   echo "error: stage1 is $$size bytes, must be exactly 512"; exit 1; fi

# stage 2 is built after the kernel because it needs to be told how many
# sectors to read. Baking the real size in beats guessing a generous constant
# and reading megabytes of empty disk on every boot.
$(STAGE2_BIN): boot/stage2.asm boot/layout.inc $(KERNEL_BIN) | $(BUILD)
	@mkdir -p $(dir $@)
	@sectors=$$(( ($$(stat -f%z $(KERNEL_BIN)) + 511) / 512 )); \
	 $(AS) $(ASFLAGS_BIN) -DKERNEL_SECTORS=$$sectors $< -o $@ && \
	 echo "$(AS) $(ASFLAGS_BIN) -DKERNEL_SECTORS=$$sectors boot/stage2.asm -o $@"
	@size=$$(stat -f%z $@); limit=$$(( $(STAGE2_SECTORS) * 512 )); \
	 if [ $$size -gt $$limit ]; then \
	   echo "error: stage2 is $$size bytes, exceeds $$limit"; exit 1; fi

# --- kernel -----------------------------------------------------------------
$(BUILD)/%.asm.o: %.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS_ELF) $< -o $@

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(KERNEL_ELF): $(KERNEL_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) -T kernel/linker.ld -o $@ $(KERNEL_OBJS)

# The kernel is no longer bounded by a real-mode buffer; unreal mode copies it
# to 1 MiB in chunks. The only remaining limit is the space reserved for it in
# the disk image, which is a sanity check rather than an architectural wall.
$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@
	@size=$$(stat -f%z $@); limit=$$(( $(KERNEL_MAX_SECTORS) * 512 )); \
	 echo "kernel: $$size bytes ($$(( ($$size + 511) / 512 )) sectors, limit $$limit)"; \
	 if [ $$size -gt $$limit ]; then \
	   echo "error: kernel exceeds its slot in the image - raise KERNEL_MAX_SECTORS"; \
	   exit 1; fi

# --- userland ---------------------------------------------------------------
# User programs are freestanding C linked against leahOS's own libc. They run
# in ring 3 and reach the kernel only through the SYSCALL ABI, so they get the
# gcc freestanding headers but none of the host libc.
# -mno-sse and friends: the kernel has not enabled the FPU or SSE state for
# ring 3 (no OSFXSR in CR4, no XMM save on context switch), so an SSE
# instruction faults with #UD. GCC emits them freely otherwise - even a struct
# copy becomes movaps. Disabling them keeps user code to the general registers,
# exactly as the kernel does for itself. Programs link low (user.ld), so the
# default small code model is all they need.
USER_CFLAGS := -std=c11 -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
               -mno-sse -mno-sse2 -mno-mmx -mno-80387 \
               -O2 -g -Wall -Wextra -Iuser/libc/include

LIBC_CSRCS := $(shell find user/libc -name '*.c' | sort)
LIBC_OBJS  := $(LIBC_CSRCS:%.c=$(BUILD)/%.o)
CRT0_OBJ   := $(BUILD)/user/libc/crt0.asm.o

# User C sources compile with the user toolchain flags, kept separate from the
# kernel's C++ rule.
$(BUILD)/user/%.o: user/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -MMD -MP -c $< -o $@

# One rule builds any user program: crt0 first, the program object, then libc.
$(BUILD)/%.elf: $(CRT0_OBJ) $(BUILD)/user/%.o $(LIBC_OBJS) user/user.ld
	$(LD) -nostdlib -T user/user.ld -o $@ $(CRT0_OBJ) $(BUILD)/user/$*.o $(LIBC_OBJS)

# --- disk image -------------------------------------------------------------
# --add BIN/NAME.ELF=build/name.elf for every program, upper-cased.
FS_ADDS := $(foreach p,$(USER_PROGRAMS),--add BIN/$(shell echo $(p) | tr a-z A-Z).ELF=$(BUILD)/$(p).elf)

$(IMG): $(STAGE1_BIN) $(STAGE2_BIN) $(KERNEL_BIN) $(KERNEL_ELF) $(USER_ELFS) | $(DIST)
	@dd if=/dev/zero of=$@ bs=1048576 count=$(IMAGE_MIB) status=none
	@dd if=$(STAGE1_BIN) of=$@ bs=512 seek=0               conv=notrunc status=none
	@dd if=$(STAGE2_BIN) of=$@ bs=512 seek=$(STAGE2_LBA)   conv=notrunc status=none
	@dd if=$(KERNEL_BIN) of=$@ bs=512 seek=$(KERNEL_LBA)   conv=notrunc status=none
	@python3 tools/mkfs_fat32.py $@ $(FAT32_LBA) $(FS_ADDS)
	@cp $(KERNEL_ELF) $(DIST_ELF)
	@echo "image:  $@"
	@echo "symbols: $(DIST_ELF)"

# The ext4 root filesystem (disk 1). BIN/NAME.ELF=build/name.elf for every
# program, upper-cased to match the paths the kernel loads today.
EXT_ADDS := $(foreach p,$(USER_PROGRAMS),BIN/$(shell echo $(p) | tr a-z A-Z).ELF=$(BUILD)/$(p).elf)

$(EXT_IMG): $(USER_ELFS) tools/mkext.sh | $(DIST)
	@tools/mkext.sh $@ $(EXT_MIB) $(EXT_ADDS)

$(BUILD):
	@mkdir -p $(BUILD)

$(DIST):
	@mkdir -p $(DIST)

# --- running ----------------------------------------------------------------
#
# -no-reboot / -no-shutdown are what turn a triple fault from a silent reboot
# loop into a stopped machine you can actually inspect.
# -netdev user / -device e1000: an Intel e1000 NIC behind QEMU's user-mode
# (SLIRP) networking. The guest is 10.0.2.15, the gateway/DNS is 10.0.2.2/3.
# Naming the netdev explicitly also suppresses the legacy default NIC, so there
# is exactly one card to find.
# Two IDE disks: disk 0 boots and holds the kernel; disk 1 is the ext4 root
# filesystem. QEMU assigns them to the primary channel master and slave in
# order, so the kernel's ATA driver finds the ext disk as drive index 1.
QEMUFLAGS := -drive format=raw,file=$(IMG),if=ide \
             -drive format=raw,file=$(EXT_IMG),if=ide \
             -m $(MEM) -smp $(CPUS) \
             -netdev user,id=net0 -device e1000,netdev=net0 \
             -no-reboot -no-shutdown \
             $(QEMU_EXTRA)

run: $(IMG) $(EXT_IMG)
	$(QEMU) $(QEMUFLAGS) -serial stdio

# Boot with no window, give the kernel TIMEOUT seconds, then print COM1.
headless: $(IMG) $(EXT_IMG)
	@tools/run-headless.sh $(TIMEOUT)

# Halts before the first instruction and waits for `make gdb` on :1234.
# int,cpu_reset logging is how you find out which vector triple-faulted.
debug: $(IMG) $(EXT_IMG)
	$(QEMU) $(QEMUFLAGS) -serial stdio -S -s -d int,cpu_reset -D $(QEMU_LOG)

gdb:
	@$(GDB) -ex 'set architecture i386:x86-64' \
	        -ex 'target remote :1234' \
	        -ex 'symbol-file $(DIST_ELF)'

toolchain:
	@for t in $(BUILD_TOOLS) $(GDB) $(QEMU); do \
	   p=$$(command -v $$t 2>/dev/null); \
	   if [ -n "$$p" ]; then printf '  ok      %-24s %s\n' "$$t" "$$p"; \
	   else printf '  MISSING %-24s\n' "$$t"; fi; \
	 done

clean:
	rm -rf $(BUILD)

help:
	@echo 'leahOS'
	@echo
	@echo '  make              build $(IMG)'
	@echo '  make run          boot in QEMU, window + serial on stdio'
	@echo '  make headless     boot with no window, print COM1, exit'
	@echo '  make debug        boot halted, gdb stub on :1234'
	@echo '  make gdb          attach to a `make debug` session'
	@echo '  make toolchain    report which tools are installed'
	@echo '  make clean'
	@echo
	@echo 'Final artifacts land in $(DIST)/:'
	@echo '  leahos.img   bootable disk 0 (MBR + kernel + FAT32 partition)'
	@echo '  ext.img      disk 1, the ext4 root filesystem (needs e2fsprogs)'
	@echo '  leahos.elf   kernel with symbols, for make gdb'
	@echo '  serial.log   COM1 capture from the last run'
	@echo
	@echo 'Knobs:  MEM=$(MEM)  CPUS=$(CPUS)  TIMEOUT=$(TIMEOUT)  QEMU_EXTRA='
	@echo '  e.g.  make run MEM=2G'
	@echo '        make headless TIMEOUT=15'
	@echo '        make run QEMU_EXTRA="-d int -D $(QEMU_LOG)"'

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
