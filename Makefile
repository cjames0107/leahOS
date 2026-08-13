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
# A third disk behind an AHCI controller, to exercise the DMA path.
SATA_IMG := $(DIST)/sata.img
# A fourth disk: a small ext4 volume with nothing to do with the root, so that
# mounting a second filesystem is something that can actually be tried.
MNT_IMG := $(DIST)/mnt.img
# A USB disk behind the xHCI controller, for the mass-storage driver.
USB_IMG  := $(DIST)/usb.img
DIST_ELF := $(DIST)/leahos.elf
SERIAL  := $(DIST)/serial.log
QEMU_LOG := $(DIST)/qemu.log

# Must agree with boot/layout.inc.
STAGE2_LBA      := 1
STAGE2_SECTORS  := 32
KERNEL_LBA      := 64
KERNEL_MAX_SECTORS := 16384              # 8 MiB of image reserved for the kernel
IMAGE_MIB       := 64
EXT_MIB         := 1024           # the ext4 root filesystem on disk 1

# --- knobs ------------------------------------------------------------------
# Override on the command line, e.g. `make run MEM=2G` or
# `make headless TIMEOUT=15`.
MEM        ?= 1024M
CPUS       ?= 2
TIMEOUT    ?= 6
# Where the AC'97 controller's output goes. `coreaudio` is the host's speakers;
# `none` still presents the device to the guest but throws the samples away,
# which is what the headless runs want. `wav` writes a file, which is the only
# way to check from a script that the right samples came out.
AUDIODEV   ?= coreaudio
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
# placed on the image by the lists below.
#
# APP_PROGRAMS are the applications. They ship only as bundles under /opt, and
# deliberately not on the command path as well: two copies of a binary is two
# things to keep in step, and the second one is exactly what lets a caller keep
# hardcoding a path instead of asking which application does the job.
#
# Where each program is placed, following the FHS.
#
# /bin is what the standard calls essential: the commands needed to work with
# the system at all, including when little else is running. /sbin is the
# system's own - init, login, the window server and every driver, none of which
# a person types the name of. /usr/bin is everything else, which here is mostly
# networking and the diagnostics.
#
# Names are lower case with no extension. They were upper case with .ELF
# because the first filesystem this could read was FAT; that driver has been
# gone for a long time and the shouting outlived it.
SBIN_PROGRAMS := init login wserver desktop blockd vfsd netd e1000d audiod \
                 authd usbd ps2d syncd ahcid useradd passwd ifconfig
BIN_PROGRAMS  := sh cat ls cp mv rm mkdir touch echo pwd clear su id whoami date \
                 chmod chown stat less grep find wc head tail sort diff tar \
                 gunzip ps kill sleep ln mount uptime readlink basename dirname tee uniq man df printf mkfifo mknod fsck sync
USRBIN_PROGRAMS := hello gui tone lspci ping ping6 arp nslookup fetch fetch6 env \
                 screenshot tests fsbench ipctest nictest nettest blktest vfstest \
                 mvtest v6test churn
APP_PROGRAMS := paint clock term uitest browse edit calc settings imgview taskman player \
                diskutil netutil calendar resmon console web
USER_PROGRAMS := $(SBIN_PROGRAMS) $(BIN_PROGRAMS) $(USRBIN_PROGRAMS) $(APP_PROGRAMS)
USER_ELFS  := $(USER_PROGRAMS:%=$(BUILD)/%.elf)
STAGE1_BIN := $(BUILD)/stage1.bin
STAGE2_BIN := $(BUILD)/stage2.bin

.PHONY: all image run headless debug gdb toolchain clean help
all: $(IMG) $(EXT_IMG) $(MNT_IMG) $(SATA_IMG) $(USB_IMG)
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

# The AP trampoline is a flat binary for a fixed low address, incbin'd into the
# kernel rather than linked, so it has to exist before ap_blob.asm assembles.
$(BUILD)/ap_trampoline.bin: boot/ap_trampoline.asm | $(BUILD)
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS_BIN) -w-implicit-abs-deprecated $< -o $@

$(BUILD)/kernel/arch/x86_64/ap_blob.asm.o: $(BUILD)/ap_trampoline.bin

# The disk driver and the filesystem are built into the kernel image, because
# there is nothing to load them from: they are what makes loading possible.
# So they have to be linked before the kernel is.
$(BUILD)/%.img: $(BUILD)/%.elf tools/mkbootimage.py
	python3 tools/mkbootimage.py $< $@

$(BUILD)/kernel/arch/x86_64/servers.asm.o: $(BUILD)/blockd.img $(BUILD)/vfsd.img $(BUILD)/init.img $(BUILD)/ahcid.img

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
# -msse -msse2 -mfpmath=sse: ring 3 has a floating-point unit now. The kernel
# sets OSFXSR and clears CR0.EM on every processor, and carries the registers
# across a context switch, a fork and a signal - see kernel/arch/x86_64/fpu.cpp.
# SSE2 is the baseline for x86-64 and is where doubles live, so -mfpmath=sse
# keeps arithmetic off the x87 stack; x87 remains enabled and is what long
# double still uses.
#
# The *kernel* stays integer-only, deliberately. It is compiled -mno-sse below,
# which is what makes entering the kernel free: with no kernel code touching
# these registers there is nothing to save until one user task gives way to
# another. Programs link low (user.ld), so the default small code model is all
# they need.
USER_CFLAGS := -std=c11 -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
               -msse -msse2 -mfpmath=sse \
               -O2 -g -Wall -Wextra -Iuser/libc/include

LIBC_CSRCS := $(shell find user/libc -name '*.c' | sort)
LIBC_OBJS  := $(LIBC_CSRCS:%.c=$(BUILD)/%.o)
CRT0_OBJ   := $(BUILD)/user/libc/crt0.asm.o

# User C sources compile with the user toolchain flags, kept separate from the
# kernel's C++ rule.
$(BUILD)/user/%.o: user/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -MMD -MP -c $< -o $@

# The maths library is the one file that must not be built against the
# compiler's own idea of what these functions do. GCC recognises exp(y*log(x))
# and rewrites it as a call to pow - which, inside pow, is a call to itself.
# The same hazard applies to every identity it knows. An explicit rule beats
# the pattern rule above.
$(BUILD)/user/libc/math.o: user/libc/math.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -fno-builtin -MMD -MP -c $< -o $@

# One rule builds any user program: crt0 first, the program object, then libc.
$(BUILD)/%.elf: $(CRT0_OBJ) $(BUILD)/user/%.o $(LIBC_OBJS) user/user.ld
	$(LD) -nostdlib -T user/user.ld -o $@ $(CRT0_OBJ) $(BUILD)/user/$*.o $(LIBC_OBJS)

# --- disk image -------------------------------------------------------------
# Disk 0 carries the two bootloader stages and the kernel, at fixed sectors,
# and nothing else. It used to hold a FAT32 partition with a second copy of
# every program, which nothing had been able to read since the FAT driver was
# removed - and which was the reason every name in the system was upper case
# with a .ELF on the end. Both are gone.
$(IMG): $(STAGE1_BIN) $(STAGE2_BIN) $(KERNEL_BIN) $(KERNEL_ELF) | $(DIST)
	@dd if=/dev/zero of=$@ bs=1048576 count=$(IMAGE_MIB) status=none
	@dd if=$(STAGE1_BIN) of=$@ bs=512 seek=0               conv=notrunc status=none
	@dd if=$(STAGE2_BIN) of=$@ bs=512 seek=$(STAGE2_LBA)   conv=notrunc status=none
	@dd if=$(KERNEL_BIN) of=$@ bs=512 seek=$(KERNEL_LBA)   conv=notrunc status=none
	@cp $(KERNEL_ELF) $(DIST_ELF)
	@echo "image:  $@"
	@echo "symbols: $(DIST_ELF)"

# The ext4 root filesystem (disk 1). BIN/NAME.ELF=build/name.elf for every
# program, upper-cased to match the paths the kernel loads today.
# Only the tools go to /BIN; the applications are placed as bundles by mkext.sh.
EXT_ADDS := $(foreach p,$(SBIN_PROGRAMS),sbin/$(p)=$(BUILD)/$(p).elf) \
            $(foreach p,$(BIN_PROGRAMS),bin/$(p)=$(BUILD)/$(p).elf) \
            $(foreach p,$(USRBIN_PROGRAMS),usr/bin/$(p)=$(BUILD)/$(p).elf)
EXT_APPS := $(foreach p,$(APP_PROGRAMS),$(p)=$(BUILD)/$(p).elf)

# The media is converted into a cache under build/, keyed on source mtime, and
# staged from there. It is a separate step because it is slow the first time
# and instant afterwards, and because nothing about it depends on the build.
MEDIA_STAMP := $(BUILD)/media/.stamp

$(MEDIA_STAMP): tools/mkmedia.py | $(BUILD)
	@python3 tools/mkmedia.py $(BUILD)/media
	@touch $@

media: $(MEDIA_STAMP)
.PHONY: media

# The font, stripped to the tables that draw glyphs. Google Sans Flex ships as
# four megabytes, nearly all of it variable-axis deltas and OpenType layout
# that this system reads none of; what is left is about forty kilobytes.
FONT_SRC  := media/fonts/GoogleSansFlex-VariableFont_GRAD,ROND,opsz,slnt,wdth,wght.ttf
FONT_OUT  := $(BUILD)/fonts/sans.ttf

$(FONT_OUT): $(FONT_SRC) tools/mkfont.py | $(BUILD)
	@mkdir -p $(dir $@)
	@python3 tools/mkfont.py "$(FONT_SRC)" $@

fonts: $(FONT_OUT)
.PHONY: fonts

$(EXT_IMG): $(USER_ELFS) tools/mkext.sh $(MEDIA_STAMP) $(FONT_OUT) | $(DIST)
	@APPS="$(EXT_APPS)" MEDIA_DIR="$(BUILD)/media" FONT_DIR="$(BUILD)/fonts" \
	    tools/mkext.sh $@ $(EXT_MIB) $(EXT_ADDS)

# Built with mke2fs directly rather than through mkext.sh: it wants no
# programs, no fonts and no accounts, only a filesystem with a file on it.
$(MNT_IMG): | $(DIST)
	@dd if=/dev/zero of=$@ bs=1048576 count=16 status=none
	@E2=$$(ls -d /opt/homebrew/opt/e2fsprogs/sbin /usr/local/opt/e2fsprogs/sbin \
	    2>/dev/null | head -1); \
	 $$E2/mke2fs -q -t ext4 -b 1024 -O ^has_journal,^resize_inode,^64bit \
	    -F $@ >/dev/null 2>&1; \
	 printf 'set_current_time now\nmkdir /notes\nquit\n' | \
	    $$E2/debugfs -w $@ >/dev/null 2>&1; \
	 echo "second filesystem on the other disk" > $(DIST)/.mntfile; \
	 printf 'cd /notes\nwrite $(DIST)/.mntfile hello.txt\nquit\n' | \
	    $$E2/debugfs -w $@ >/dev/null 2>&1; \
	 rm -f $(DIST)/.mntfile
	@echo "mnt:    $@ (16 MiB ext4, one directory and one file)"

# An ext4 volume rather than a blank one, so that mounting it is a test of the
# whole path: the filesystem asks the AHCI driver, which moves the bytes by DMA.
$(SATA_IMG): | $(DIST)
	@dd if=/dev/zero of=$@ bs=1048576 count=16 status=none
	@E2=$$(ls -d /opt/homebrew/opt/e2fsprogs/sbin /usr/local/opt/e2fsprogs/sbin \
	    2>/dev/null | head -1); \
	 $$E2/mke2fs -q -t ext4 -b 1024 -O ^has_journal,^resize_inode,^64bit \
	    -F $@ >/dev/null 2>&1; \
	 echo "this file came off the SATA disk" > $(DIST)/.satafile; \
	 printf 'mkdir /sata\ncd /sata\nwrite $(DIST)/.satafile hello.txt\nquit\n' | \
	    $$E2/debugfs -w $@ >/dev/null 2>&1; \
	 rm -f $(DIST)/.satafile
	@echo "sata:   $@ (16 MiB ext4, reached over AHCI)"

$(USB_IMG): | $(DIST)
	@dd if=/dev/zero of=$@ bs=1048576 count=8 status=none
	@echo "usb:    $@ (8 MiB, blank, for the USB mass-storage test)"

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
QEMUFLAGS := -machine pc,hpet=on \
             -drive format=raw,file=$(IMG),if=ide \
             -drive format=raw,file=$(MNT_IMG),if=ide \
             -device qemu-xhci,id=xhci0 \
             -drive format=raw,file=$(USB_IMG),if=none,id=usbdisk \
             -device usb-storage,drive=usbdisk,bus=xhci0.0 \
             -device usb-kbd,bus=xhci0.0 \
             -audiodev $(AUDIODEV),id=snd0 -device AC97,audiodev=snd0 \
             -device ahci,id=sata0 \
             -drive format=raw,file=$(EXT_IMG),if=none,id=rootdisk \
             -device ide-hd,drive=rootdisk,bus=sata0.0 \
             -drive format=raw,file=$(SATA_IMG),if=none,id=satadisk \
             -device ide-hd,drive=satadisk,bus=sata0.1 \
             -m $(MEM) -smp $(CPUS) \
             -netdev user,id=net0,ipv4=on,ipv6=on -device e1000,netdev=net0 \
             -no-reboot -no-shutdown \
             $(QEMU_EXTRA)

# The headless check. Boots, drives a shell, and asserts against the serial
# line rather than against a screenshot - a screenshot needs somebody to look
# at it, and this needs to be able to fail on its own.
# What machine this project runs on, for anything that needs to boot the same
# one. The test harness asks for this rather than keeping its own copy - it
# kept one, and the two drifted twice: once missing the SATA controller, and
# once passing while `make run` had no root disk at all.
.PHONY: print-qemuflags
print-qemuflags:
	@echo '$(QEMUFLAGS)'

.PHONY: check
check: $(IMG) $(EXT_IMG) $(MNT_IMG) $(SATA_IMG) $(USB_IMG)
	@python3 tools/vm/smoke.py

run: $(IMG) $(EXT_IMG) $(MNT_IMG) $(SATA_IMG) $(USB_IMG)
	$(QEMU) $(QEMUFLAGS) -serial stdio

# Boot with no window, give the kernel TIMEOUT seconds, then print COM1.
headless: $(IMG) $(EXT_IMG) $(MNT_IMG) $(SATA_IMG) $(USB_IMG)
	@tools/run-headless.sh $(TIMEOUT)

# Halts before the first instruction and waits for `make gdb` on :1234.
# int,cpu_reset logging is how you find out which vector triple-faulted.
debug: $(IMG) $(EXT_IMG) $(MNT_IMG) $(SATA_IMG) $(USB_IMG)
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
	@echo '  make check        boot headless and assert, non-zero on failure'
	@echo '  make headless     boot with no window, print COM1, exit'
	@echo '  make debug        boot halted, gdb stub on :1234'
	@echo '  make gdb          attach to a `make debug` session'
	@echo '  make toolchain    report which tools are installed'
	@echo '  make clean'
	@echo
	@echo 'Final artifacts land in $(DIST)/:'
	@echo '  leahos.img   bootable disk 0 (MBR + stage2 + kernel)'
	@echo '  ext.img      disk 1, the ext4 root filesystem (needs e2fsprogs)'
	@echo '  leahos.elf   kernel with symbols, for make gdb'
	@echo '  serial.log   COM1 capture from the last run'
	@echo
	@echo 'Knobs:  MEM=$(MEM)  CPUS=$(CPUS)  TIMEOUT=$(TIMEOUT)  QEMU_EXTRA='
	@echo '  e.g.  make run MEM=2G'
	@echo '        make headless TIMEOUT=15'
	@echo '        make run QEMU_EXTRA="-d int -D $(QEMU_LOG)"'

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
