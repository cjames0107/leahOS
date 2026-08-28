"""The two settings whose effect is a behaviour rather than a file.

Both are new machinery in the compositor, and both are the kind of thing that
reads correctly and does nothing at all, so they are driven here: the wheel is
turned, and the screen is left alone until it goes dark.

Two things this got wrong before, both worth stating because both made it pass
while testing nothing. Keystrokes go to whatever holds the keyboard, so a
command typed while a window is focused is typed *into* that window - Files
reads i, l and t as view shortcuts, so the launch silently changed the view and
Settings never started. And control positions are offsets into a window whose
position is not guaranteed, so Files is closed first and there are exactly two
windows on screen from then on: the terminal, which never moves, and Settings,
which is told where to go.

Not in `make check`: it waits two and a half minutes doing nothing, which is
the point of it. Run it when the compositor's input path or the blanking
changes.

It waits over a minute on purpose - the shortest blanking offered is a minute,
and testing a shorter one would be testing something nobody can choose.
"""
import sys, os, time
sys.path.insert(0, os.path.join(os.getcwd(), "tools/vm"))
from machine import main, Failure

WX, WY = 60, 40
TOP = WY + 37
ROW_H = 24
ROWS = ["Desktop", "General", "Appearance", "Screen",
        "Hardware", "Sound", "Mouse", "Storage",
        "System", "Network", "Date & Time", "Users", "About",
        "UNIX", "Shell", "Terminal"]

TERMINAL = (300, 620)           # below anything Settings covers


def page(t, name):
    t.m.click(WX + 60, TOP + ROWS.index(name) * ROW_H + ROW_H // 2)
    time.sleep(2)


def body(t):
    t.m.click(55, 55)                           # Files: close
    time.sleep(3)

    # Something to scroll, in a window that is where it says it is.
    t.m.click(*TERMINAL)
    time.sleep(1)
    t.m.type("ls -l /bin\n")
    time.sleep(6)
    t.m.wheel(TERMINAL[0], TERMINAL[1], -6)     # back, the ordinary way
    time.sleep(1)
    t.m.shot("beh-0-wheeled-back")

    t.m.click(*TERMINAL)
    time.sleep(1)
    t.m.type("/Applications/Settings.app/settings %d %d\n" % (WX, WY))
    time.sleep(9)
    t.m.shot("beh-1-settings-up")

    page(t, "Mouse")
    t.m.click(WX + 652, WY + 224)               # Natural Scrolling, on
    time.sleep(2)
    t.m.shot("beh-2-natural-on")

    # The same notch, now the other way: it should go back down to the bottom.
    t.m.click(*TERMINAL)
    time.sleep(1)
    t.m.wheel(TERMINAL[0], TERMINAL[1], -6)
    time.sleep(1)
    t.m.shot("beh-3-wheeled-the-other-way")

    # --- blanking -----------------------------------------------------------
    t.m.click(WX + 400, WY + 15)                # Settings' title bar, to raise
    time.sleep(2)
    page(t, "Screen")
    t.m.click(WX + 544, WY + 121)               # "1 min"
    time.sleep(2)
    t.m.shot("beh-4-blank-set")

    time.sleep(75)                              # nothing touches it
    t.m.shot("beh-5-dark")

    # A modifier, which types nothing into whatever is underneath - and which
    # is what a person presses to wake a screen they do not want to type at.
    # It produced no character, so an earlier build stayed dark until a letter
    # was pressed, and that letter went into the window below.
    t.m.key("shift")
    time.sleep(3)
    t.m.shot("beh-6-woken")

    # And moving the pointer, from dark again.
    time.sleep(70)
    t.m.shot("beh-7-dark-again")
    t.m.move_to(500, 400)
    time.sleep(3)
    t.m.shot("beh-8-woken-by-mouse")

    stray = t.faults()
    if stray:
        raise Failure("%s" % stray[0])
    t.checks += 1


main("settings-behave", body)
