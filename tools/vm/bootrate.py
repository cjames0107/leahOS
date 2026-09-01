"""How often does the machine fail to boot?

Written because a session was spent reaching opposite conclusions about the
same change, twice, from samples of two and three. The failures this system has
left are intermittent, so a claim about one is a claim about a *rate*, and a
rate needs the same measurement every time and enough of it to mean something.

Runs stride.py N times and counts what happened. Nothing clever; the value is
that it is the same procedure each time, kills stale machines between runs, and
prints a count rather than an impression.

    python3 tools/vm/bootrate.py 6          # six boots
    python3 tools/vm/bootrate.py 6 label    # and call it `label` in the output
"""
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SERIAL = os.environ.get("LEAH_SERIAL", "/tmp/leah-serial.log")

# What a run can end as. Ordered: the first that matches wins, because a boot
# that panicked is not also a boot that merely timed out.
OUTCOMES = [
    ("no-filesystem", re.compile(r"no filesystem: nothing can be loaded")),
    ("lock-order",    re.compile(r"lock order|acquiring the kernel lock|out of order")),
    ("other-panic",   re.compile(r"KERNEL PANIC")),
    ("user-fault",    re.compile(r"\[\d+\] faulted")),
]


def reap():
    """No stale machine may overlap the next run - two QEMUs on eight cores
    slows both enough to change what is being measured."""
    subprocess.run(["pkill", "-f", "qemu-system-x86_64"], capture_output=True)
    time.sleep(4)


def one_run(timeout):
    reap()
    done = subprocess.run(["python3", os.path.join(HERE, "stride.py")],
                          capture_output=True, text=True, timeout=timeout + 30)
    passed = done.stdout.startswith("ok") or "\nok " in done.stdout
    try:
        with open(SERIAL, "r", errors="replace") as f:
            log = f.read()
    except OSError:
        log = ""
    for name, pattern in OUTCOMES:
        if pattern.search(log):
            return name
    return "ok" if passed else "timeout"


def main():
    runs = int(sys.argv[1]) if len(sys.argv) > 1 else 6
    label = sys.argv[2] if len(sys.argv) > 2 else "build"

    counts = {}
    for i in range(runs):
        try:
            what = one_run(260)
        except subprocess.TimeoutExpired:
            what = "timeout"
        counts[what] = counts.get(what, 0) + 1
        print("  %2d/%d  %s" % (i + 1, runs, what), flush=True)

    reap()
    good = counts.get("ok", 0)
    print("\n%-16s %d/%d clean" % (label, good, runs), end="")
    for name in sorted(k for k in counts if k != "ok"):
        print(", %s %d" % (name, counts[name]), end="")
    print()
    return 0 if good == runs else 1


if __name__ == "__main__":
    sys.exit(main())
