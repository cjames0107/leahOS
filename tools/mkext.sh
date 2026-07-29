#!/usr/bin/env bash
#
# Build the ext4 image that leahOS mounts as its root filesystem, populated from
# the user programs plus a few fixed files the kernel's self-test reads back.
#
#   mkext.sh <out.img> <size-mib> [DEST=SRC ...]
#
# Each DEST=SRC places host file SRC at path DEST inside the image (DEST may name
# a subdirectory, e.g. BIN/INIT.ELF). The feature set is deliberately tamed - no
# metadata_csum, 64bit or dir_index - so the kernel's writer can keep the volume
# e2fsck-clean without computing checksums, 64-bit group descriptors or an HTree
# index. Extents and the journal stay on, so it is a genuine ext4 volume.

set -euo pipefail

if [ "$#" -lt 2 ]; then
    echo "usage: mkext.sh <out.img> <size-mib> [DEST=SRC ...]" >&2
    exit 2
fi

OUT="$1"; SIZE_MIB="$2"; shift 2

E2_DIR="$(brew --prefix e2fsprogs 2>/dev/null)/sbin"
MKE2FS="$E2_DIR/mke2fs"
if [ ! -x "$MKE2FS" ]; then
    echo "error: mke2fs not found under $E2_DIR" >&2
    echo "install it with:  brew install e2fsprogs" >&2
    exit 1
fi

STAGING="$(mktemp -d)"
trap 'rm -rf "$STAGING"' EXIT

# Fixed files the kernel self-test reads. README.MD is deliberately larger than
# one 4 KiB block so reading it exercises multi-block mapping (an extent that
# spans several blocks), mirroring the FAT32 image's cluster-chain file.
mkdir -p "$STAGING/BIN" "$STAGING/docs"

# Application bundles. A .app is a directory that carries its own description,
# Application bundles. A .app is a directory that carries its own description,
# so nothing else needs a built-in table of which program opens what - see
# user/libc/include/bundle.h. They are staged here rather than assembled at
# runtime because a bundle is a filesystem layout, and the image is where the
# filesystem gets laid out.
#
# Stage one application bundle: the directory, its binary, its Info, and an
# icon. Everything an application declares about itself is written here, which
# is the point - there is no second copy of these facts anywhere in the system.
#
#   stage_bundle <Name> <src.elf> <opens> <menu...>
stage_bundle() {
    local app="$1"; local src="$2"; local opens="$3"; shift 3
    local dir="$STAGING/Apps/$app.app"
    local exe
    exe="$(basename "$src")"
    mkdir -p "$dir"
    cp "$src" "$dir/$exe"
    {
        printf 'name %s\n' "$app"
        printf 'exec %s\n' "$exe"
        printf 'icon Icon.png\n'
        if [ -n "$opens" ]; then printf 'opens %s\n' "$opens"; fi
        local item
        for item in "$@"; do printf 'menu %s\n' "$item"; done
    } > "$dir/Info"
    python3 tools/mkicon.py "$dir/Icon.png" "$app"
}
printf 'Hello from ext4.\n' > "$STAGING/HELLO.TXT"
printf 'Notes live in a subdirectory.\n' > "$STAGING/docs/notes.txt"
{
    printf '# leahOS\n\n'
    for _ in $(seq 1 120); do
        printf 'leahOS reads this file back through the ext driver, block by block. '
    done
    printf '\n'
} > "$STAGING/README.MD"

# Accounts, home directories and the shadow file.
python3 "$(dirname "$0")/mkaccounts.py" "$STAGING"

# Requested files (the user programs).
for pair in "$@"; do
    dest="${pair%%=*}"
    src="${pair#*=}"
    mkdir -p "$STAGING/$(dirname "$dest")"
    cp "$src" "$STAGING/$dest"
done

# Every application, as a complete bundle. The binaries come straight from the
# build rather than from the staged /BIN, because these deliberately do not go
# into /BIN at all - see APP_PROGRAMS in the Makefile.
#
# The table is here rather than in the Makefile because everything on a line is
# a fact about the application: what it is called, what it opens, and what it
# offers when right-clicked.
stage_app() {                       # <Name> <make-name> <opens> <menu...>
    local name="$1"; local prog="$2"; shift 2
    local src="$BUILD_DIR/$prog.elf"
    if [ -f "$src" ]; then stage_bundle "$name" "$src" "$@"; fi
}
BUILD_DIR="${BUILD_DIR:-build}"
stage_app Files     browse   ""              "New window" "New folder"
stage_app Terminal  term     ""              "New terminal"
stage_app Edit      edit     ".TXT .MD .C .H .LEAHRC" "New document"
stage_app Paint     paint    ".PNG .GIF"     "New drawing"
stage_app Images    imgview  ".PNG"          "Open picture..."
stage_app Calculator calc    ""              ""
stage_app Settings  settings ""              "Appearance" "Users"
stage_app Tasks     taskman  ""              "End task"
stage_app Clock     clock    ""              ""
stage_app Elements  uitest   ""              ""

# A desktop with three things on it, for every account that has one. An empty
# desktop is a correct desktop and a poor first impression: these say what is
# here and give somewhere to start.
#
# A shortcut is a ".alias" file holding the path it stands for - there are no
# symbolic links in this filesystem, and a text file needs neither the kernel
# nor the on-disk format to know anything new. See bundle.h.
stage_desktop() {                   # <home>
    local home="$1"
    mkdir -p "$STAGING/$home/Desktop"
    printf '/Apps/Files.app\n' > "$STAGING/$home/Desktop/Files.alias"
    printf '/Apps/Edit.app\n'  > "$STAGING/$home/Desktop/Notepad.alias"
    printf '/README.MD\n'      > "$STAGING/$home/Desktop/Readme.alias"
}
stage_desktop root
stage_desktop home/leah
stage_desktop home/guest

dd if=/dev/zero of="$OUT" bs=1048576 count="$SIZE_MIB" status=none

# The feature set / block / inode size can be overridden to build read-only
# breadth images that exercise the harder reader paths (64bit descriptors,
# HTree directories, metadata checksums, 1 KiB blocks). The default is the tamed
# set the kernel's writer targets.
MKE2FS_FEATURES="${MKE2FS_FEATURES:-^metadata_csum,^64bit,^dir_index,^orphan_file}"
MKE2FS_BLOCK="${MKE2FS_BLOCK:-4096}"
MKE2FS_INODE="${MKE2FS_INODE:-256}"

# -F: operate on a plain file. -d: populate from the staging tree at creation.
"$MKE2FS" -q -F -t ext4 -b "$MKE2FS_BLOCK" -I "$MKE2FS_INODE" \
    -O "$MKE2FS_FEATURES" \
    -d "$STAGING" "$OUT" >/dev/null

# mke2fs -d copies the *host* ownership, which is whoever built the image - so
# fix up the things whose owner is part of the point. debugfs writes inode
# fields directly; sif is "set inode field".
DEBUGFS="$E2_DIR/debugfs"
if [ -x "$DEBUGFS" ]; then
    {
        # The shadow file must be unreadable to anyone but root. Nothing in
        # userland reads it anyway - the kernel does - but a world-readable
        # hash file would undo the point of hashing.
        echo "sif /etc/shadow uid 0"
        echo "sif /etc/shadow gid 0"
        echo "sif /etc/shadow mode 0100600"
        echo "sif /etc/passwd mode 0100644"
        # Home directories are private. mke2fs -d copies the *host* mode as
        # well as the host owner, which leaves them world readable - and a home
        # directory anyone can read makes the whole account model decorative.
        echo "sif /root uid 0"
        echo "sif /root gid 0"
        echo "sif /root mode 040700"
        echo "sif /root/readme.txt uid 0"
        echo "sif /root/readme.txt gid 0"
        echo "sif /root/readme.txt mode 0100600"
        echo "sif /home/leah uid 1000"
        echo "sif /home/leah gid 1000"
        echo "sif /home/leah mode 040700"
        echo "sif /home/leah/readme.txt uid 1000"
        echo "sif /home/leah/readme.txt gid 1000"
        echo "sif /home/leah/readme.txt mode 0100600"
        # The desktop belongs to the account too - mke2fs -d copies the host's
        # ownership, so without this a user's own Desktop is owned by whoever
        # built the image and is read-only to them inside their own home.
        for f in "" /Files.alias /Notepad.alias /Readme.alias; do
            echo "sif /home/leah/Desktop$f uid 1000"
            echo "sif /home/leah/Desktop$f gid 1000"
        done
        echo "sif /home/leah/Desktop mode 040700"
        echo "sif /home/guest uid 1001"
        echo "sif /home/guest gid 1001"
        echo "sif /home/guest mode 040700"
        echo "sif /home/guest/readme.txt uid 1001"
        echo "sif /home/guest/readme.txt gid 1001"
        echo "sif /home/guest/readme.txt mode 0100600"
        for f in "" /Files.alias /Notepad.alias /Readme.alias; do
            echo "sif /home/guest/Desktop$f uid 1001"
            echo "sif /home/guest/Desktop$f gid 1001"
        done
        echo "sif /home/guest/Desktop mode 040700"
    } | "$DEBUGFS" -w "$OUT" >/dev/null 2>&1
fi

echo "ext4:   $OUT ($SIZE_MIB MiB, block $MKE2FS_BLOCK), populated from $(($#)) files + fixtures"
