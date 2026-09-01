"""Run the FIFO rendezvous hundreds of times and see it break.

`tests` does this exchange once and fails it about one run in three. One round
takes about a millisecond, so a few hundred of them should reach the same fault
in seconds - which turns a bug you can chase for a week into one you can bisect
in an afternoon.

The failure is printed with the round number, how many bytes came back, what
the writer exited with, and the bytes themselves. A short read and a read of
the wrong thing are different bugs and only that detail tells them apart.
"""
import sys, os
sys.path.insert(0, os.path.join(os.getcwd(), "tools/vm"))
from machine import main, Failure


def body(t):
    # Enough rounds that a one-in-three-per-boot fault should appear several
    # times over, and few enough that a clean run does not outlast the
    # harness's patience.
    out = t.run("fifoloop -n 400", timeout=180)
    tail = out[out.rfind("==done-%d==" % (t.marks - 2)):] if t.marks > 1 else out

    for line in tail.splitlines():
        if "fifoloop:" in line:
            print("  %s" % line.strip())

    t.checks += 1
    if "0 failures" not in tail:
        raise Failure("the rendezvous broke - see the rounds above")

    # And the same exchange over an ordinary pipe, which is what `tests`
    # fails as "a pipe carries output".
    out = t.run("fifoloop -p -n 400", timeout=180)
    tail = out[out.rfind("==done-%d==" % (t.marks - 2)):] if t.marks > 1 else out
    for line in tail.splitlines():
        if "pipeloop:" in line:
            print("  %s" % line.strip())
    t.checks += 1
    if "0 failures" not in tail:
        raise Failure("the plain pipe broke - see the rounds above")

    stray = t.faults()
    if stray:
        raise Failure("%s" % stray[0])


main("fifoprobe", body)
