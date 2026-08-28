"""The status bar, the dock, the panel and the notification cards.

Screenshots, because this is furniture: whether it is in the right place and
the right size is something to look at rather than assert. What is asserted is
that it is on top - the dock being behind a window is the failure this layer
exists to prevent, and a picture of it is the only way to tell.
"""
import sys, os, time
sys.path.insert(0, os.path.join(os.getcwd(), "tools/vm"))
from machine import main, Failure


def body(t):
    time.sleep(4)
    t.m.shot("sh-0-desktop")

    t.m.move_to(512, 715)
    time.sleep(2)
    t.m.shot("sh-1-dock-hover")

    # The panel, which the bar's left button drops down.
    t.m.click(21, 17)
    time.sleep(2)
    t.m.shot("sh-2-panel")

    # A notification, posted from a program with no window of its own.
    t.m.click(21, 17)                           # shut the panel
    time.sleep(1)
    t.m.click(300, 620)                         # the terminal, for the keyboard
    time.sleep(1)
    t.m.type("say -f 'Task Manager' You are running out of memory. Consider quitting some applications.\n")
    time.sleep(3)
    t.m.shot("sh-3-notification")

    # Picking a running application brings it forward. Files starts behind
    # Settings and the Terminal; after this it should be in front of both.
    t.m.click(21, 17)
    time.sleep(2)
    t.m.click(60, 104)                          # the second row: Files
    time.sleep(3)
    t.m.shot("sh-4-raised")

    stray = t.faults()
    if stray:
        raise Failure("%s" % stray[0])
    t.checks += 1


main("shell", body)
