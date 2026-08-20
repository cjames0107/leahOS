"""How many frames the compositor finishes per second, worst case.

A full-screen window that carries alpha, redrawn and presented as fast as the
client can: the whole desktop is damaged every frame, so the wallpaper fill,
the blend and the blit are all inside the number.
"""
import sys, os, time
sys.path.insert(0, os.path.join(os.getcwd(), "tools/vm"))
from machine import main, Failure

def body(t):
    was = len(t.m.serial())
    t.m.type("/usr/bin/wintest bench 5000 > /dev/console\n")
    said = ""
    for _ in range(60):
        time.sleep(0.5)
        said = t.m.serial()[was:]
        if "frames in" in said:
            break
    line = [l for l in said.splitlines() if "frames in" in l]
    if not line:
        raise Failure("no measurement came back:\n%s" % said)
    print("  %s" % line[0])
    t.checks += 1

main("gui-speed", body)
