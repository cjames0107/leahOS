#!/bin/sh
# Boot the image with no window, let it run for a few seconds, and print
# whatever the kernel wrote to COM1. This is the fast feedback loop - `make
# run` is for when you actually want to look at the screen.
#
#   tools/run-headless.sh [seconds]

set -e
cd "$(dirname "$0")/.."

SECONDS_TO_RUN="${1:-6}"
MEM="${MEM:-512M}"
CPUS="${CPUS:-1}"
LOG=build/serial.log

make --no-print-directory
rm -f "$LOG"

# shellcheck disable=SC2086  # QEMU_EXTRA is deliberately word-split
qemu-system-x86_64 \
    -drive format=raw,file=build/leahos.img,if=ide \
    -m "$MEM" -smp "$CPUS" -display none -serial "file:$LOG" \
    -no-reboot -no-shutdown $QEMU_EXTRA &
QEMU_PID=$!

# `sleep` would do, but perl's select avoids depending on coreutils.
perl -e "select(undef, undef, undef, $SECONDS_TO_RUN)"
kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true

echo "----- COM1 -----"
cat "$LOG"
