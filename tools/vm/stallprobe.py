"""Catch the startup Terminal stall and ask the machine what it is doing.

The failure: the desktop comes up complete - wallpaper, Files, Settings, the
dock, the clock - and the Terminal that login starts is simply not there. The
harness calls it "no window took the keyboard after six tries", and it is the
single biggest source of wasted time in this project.

What is already known (see the memory note): the processor is halted, so
nothing is spinning and everything is blocked; the task count matches a healthy
boot, so the Terminal is most likely alive and stuck rather than dead; and
clicking the dock opens a working Terminal immediately, so `term` itself is
fine.

That last part is the way in. The desktop is alive during the stall, so this
opens a shell the way a person would - by clicking the dock - and then asks
`ps` what the stuck one is doing. A stalled machine that can still be
interrogated is not a mystery, it is just a machine nobody has asked.
"""
import sys, os, time
sys.path.insert(0, os.path.join(os.getcwd(), "tools/vm"))
from machine import main, Failure

# Where the dock's Terminal icon sits, from the screenshots taken of this
# desktop. Second icon along.
DOCK_TERMINAL = (440, 728)


def body(t):
    # Let the desktop settle, however it is going to settle.
    time.sleep(18)
    t.m.shot("stall-0-desktop")

    # First: did login's Terminal take the keyboard? Typed blind, because
    # asking through the harness would assert and this is the question.
    t.m.type("echo STARTUP-TERM-ALIVE > /dev/console\n")
    time.sleep(4)
    stalled = "STARTUP-TERM-ALIVE" not in t.m.serial()
    print("  startup terminal: %s" % ("MISSING - stall caught" if stalled
                                      else "present"))

    # Open a Terminal by hand. On a healthy boot this is a second one and
    # costs nothing; on a stalled boot it is the only one.
    t.m.move_to(*DOCK_TERMINAL)
    t.m.cmd("mouse_button 1", 0.2)
    t.m.cmd("mouse_button 0", 0.2)
    time.sleep(6)
    t.m.shot("stall-1-clicked")

    # And ask. `ps` prints every task with its state, which is the question:
    # is the Terminal login started still there, and if so what is it waiting
    # on? Two runs of it, because the interesting case is a state that does
    # not change.
    out = t.run("ps")
    tail = out[out.rfind("==done-"):] if "==done-" in out else out
    print("  ---- ps ----")
    for line in t.run("ps").splitlines()[-30:]:
        if "term" in line or "shell" in line or "login" in line or "desktop" in line:
            print("  %s" % line.rstrip())

    if stalled:
        raise Failure("stall caught - see the ps above for what term is doing")

    stray = t.faults()
    if stray:
        print("  faults: %s" % stray[0])
    t.checks += 1


main("stallprobe", body)
