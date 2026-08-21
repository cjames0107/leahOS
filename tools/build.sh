#!/bin/sh
# Build, and say so only if it worked.
#
# `make | grep error; echo built` prints "built" whatever happened: the exit
# status belongs to the grep, and the echo runs regardless. That has now twice
# sent an afternoon chasing a bug in code that was never compiled - once with
# a check reading a stale disk image, once with a text field whose selection
# "did not work" because the library it was in had failed to build for an hour.
set -e
if make -s "$@" 2>&1 | tail -40; then :; fi
# The pipe's status is the tail's, so ask make again - it is a no-op when the
# first one worked and a failure when it did not.
make -s -q "$@" >/dev/null 2>&1 || {
    if ! make -s "$@" >/dev/null 2>&1; then
        echo "BUILD FAILED" >&2
        make -s "$@" 2>&1 | grep -E "error|Error" | head -20 >&2
        exit 1
    fi
}
echo "build ok"
