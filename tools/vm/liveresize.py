"""Resizing a window shows the window, not an outline of one.

And the toolbar folds as it narrows: the controls that no longer fit move
behind a button at the end of the row.
"""
import sys, os, time
sys.path.insert(0, os.path.join(os.getcwd(), "tools/vm"))
from machine import main, Failure


def body(t):
    t.m.type("/Applications/Write.app/write 40 40 &\n")
    time.sleep(9)
    t.m.shot("lr-before")

    # The frame's bottom-right corner: 720x520 at 40,40, plus the border and
    # the title bar.
    def during():
        t.m.shot("lr-during")

    t.m.drag(756, 586, 420, 430, steps=8, hold=during)
    time.sleep(2)
    t.m.shot("lr-after")

    # What folded away is behind the button at the end of the row.
    t.m.click(395, 88)
    time.sleep(2)
    t.m.shot("lr-popover")
    t.checks += 1


main("liveresize", body)
