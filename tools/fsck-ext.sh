#!/bin/sh
# Boot once with persistent disks, then check the ext4 root filesystem the
# kernel left behind with the real e2fsck. This is the ext counterpart of
# fsck-image.sh: an independent implementation has to agree the volume is still
# well formed after the kernel wrote to it - the gate that proves our writer
# keeps bitmaps, group descriptors, the superblock and the (empty) journal
# consistent.
#
#   tools/fsck-ext.sh [seconds]

set -e
cd "$(dirname "$0")/.."

SECONDS_TO_RUN="${1:-14}"

E2_DIR="$(brew --prefix e2fsprogs 2>/dev/null)/sbin"
E2FSCK="$E2_DIR/e2fsck"
if [ ! -x "$E2FSCK" ]; then
    echo "e2fsck not found under $E2_DIR; install with: brew install e2fsprogs"
    exit 1
fi

# Start from pristine images so the write tests do not trip over leftovers.
rm -f build/dist/leahos.img build/dist/ext.img
make --no-print-directory >/dev/null

qemu-system-x86_64 \
    -drive format=raw,file=build/dist/leahos.img,if=ide \
    -drive format=raw,file=build/dist/ext.img,if=ide -netdev user,id=net0 -device e1000,netdev=net0 \
    -m 512M -display none -serial file:build/dist/serial.log \
    -no-reboot -no-shutdown &
QEMU_PID=$!
perl -e "select(undef, undef, undef, $SECONDS_TO_RUN)"
kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true

if grep -q "PANIC" build/dist/serial.log; then
    echo "FAIL: kernel panicked"
    grep -A1 "PANIC" build/dist/serial.log | head -2
    exit 1
fi

echo "----- e2fsck on the ext4 root after kernel writes -----"
# -f force a full check, -n answer no to any repair (read-only). A clean volume
# exits 0; anything e2fsck would change exits non-zero.
if "$E2FSCK" -fn build/dist/ext.img; then
    echo "PASS: ext4 volume is clean after kernel writes"
else
    echo "FAIL: ext4 volume is not clean after kernel writes"
    exit 1
fi
