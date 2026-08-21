"""Positions that used to be counted in eight-pixel cells.

The editor's caret and selection, the browser's headings, and a rename sheet
that says what it does.
"""
import sys, os, time
sys.path.insert(0, os.path.join(os.getcwd(), "tools/vm"))
from machine import Test, Failure


def shot(name, launch, steps):
    t = Test()
    try:
        t.m.type(launch + "\n")
        time.sleep(9)
        for fn in steps:
            fn(t)
        t.m.shot(name)
        stray = t.faults()
        if stray:
            raise Failure("%s: %s" % (name, stray[0]))
    finally:
        t.stop()


def typing(t):
    # Proportional letters of very different widths: with a fixed cell the
    # caret ends up a long way from the last one.
    t.m.type("WWWWWiiiiiMMMMM|")
    time.sleep(2)


def click_mid(t):
    t.m.click(150, 160)     # into the middle of that line
    time.sleep(2)


def open_rename(t):
    t.m.click(230, 130)
    time.sleep(1)
    t.m.click(671, 85)
    time.sleep(1)
    t.m.click(700, 135)
    time.sleep(2)


shot("m-edit", "/Applications/Edit.app/edit 40 40", [typing, click_mid])
shot("m-files", "/Applications/Files.app/browse 40 40", [open_rename])
print("ok    measured")
