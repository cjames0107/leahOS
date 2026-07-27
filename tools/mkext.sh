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
printf 'Hello from ext4.\n' > "$STAGING/HELLO.TXT"
printf 'Notes live in a subdirectory.\n' > "$STAGING/docs/notes.txt"
{
    printf '# leahOS\n\n'
    for _ in $(seq 1 120); do
        printf 'leahOS reads this file back through the ext driver, block by block. '
    done
    printf '\n'
} > "$STAGING/README.MD"

# Requested files (the user programs).
for pair in "$@"; do
    dest="${pair%%=*}"
    src="${pair#*=}"
    mkdir -p "$STAGING/$(dirname "$dest")"
    cp "$src" "$STAGING/$dest"
done

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

echo "ext4:   $OUT ($SIZE_MIB MiB, block $MKE2FS_BLOCK), populated from $(($#)) files + fixtures"
