#!/bin/sh
# Boot once with a persistent disk, then check the filesystem the kernel left
# behind with an external FAT checker. This is the verification that caught the
# FSInfo bug the kernel's own read-back tests could not: an independent
# implementation has to agree the volume is still well formed after we wrote to
# it.
#
# Deliberately separate from run-headless.sh, which uses -snapshot for
# reproducibility. Here persistence is the entire point.
#
#   tools/fsck-image.sh [seconds]

set -e
cd "$(dirname "$0")/.."

SECONDS_TO_RUN="${1:-14}"
FAT32_LBA=20480

# Start from a pristine image so the write tests do not trip over their own
# leftovers from a previous run.
rm -f build/dist/leahos.img
make --no-print-directory >/dev/null

qemu-system-x86_64 \
    -drive format=raw,file=build/dist/leahos.img,if=ide -netdev user,id=net0 -device e1000,netdev=net0 \
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

dd if=build/dist/leahos.img of=build/dist/fat-check.img bs=512 skip="$FAT32_LBA" status=none

if ! command -v fsck_msdos >/dev/null 2>&1; then
    echo "fsck_msdos not found; skipping external check"
    exit 0
fi

echo "----- fsck_msdos on the volume after kernel writes -----"
fsck_msdos -n build/dist/fat-check.img | tee build/dist/fsck.log || true

# Match only real problem markers. "Checking for Orphan Clusters" is a phase
# header, not a finding, so a bare "orphan" match would be a false positive;
# an actual problem shows a "Fix?" prompt or a specific complaint.
if grep -qiE "Fix\?|not correct|truncat|invalid|orphaned|mark.*free|difference" build/dist/fsck.log; then
    echo "FAIL: filesystem is not clean after kernel writes"
    exit 1
fi
echo "PASS: volume is clean after kernel writes"
