"""Whether libc's code survives what the self-tests do to it."""
import sys, os
sys.path.insert(0, os.path.join(os.getcwd(), "tools/vm"))
from machine import main, Failure


def body(t):
    out = t.run("dyntest", timeout=180)
    start = out.rfind("loader table")
    for line in out[start:].splitlines():
        if line.strip():
            print("  | " + line.strip())
    t.checks += 1


main("dynprobe", body)
