"""Settings, page by page.

Every page is opened and photographed, because a control panel is a thing that
has to be looked at: a group that overflows its box or a control that lands on
top of its label is not something a check for faults would notice.

The sidebar has headings in it now, so the rows are not the pages - which is
the first thing to get wrong and the reason the row a page lives on is worked
out here rather than counted.
"""
import sys, os, time
sys.path.insert(0, os.path.join(os.getcwd(), "tools/vm"))
from machine import main, Failure

# The sidebar, in order, exactly as settings.c lists it. Headings included,
# because they take a row each.
ROWS = ["Desktop", "General", "Appearance", "Screen",
        "Hardware", "Sound", "Mouse", "Storage",
        "System", "Network", "Date & Time", "Users", "About",
        "UNIX", "Shell", "Terminal"]
PAGES = [r for r in ROWS if r not in ("Desktop", "Hardware", "System", "UNIX")]

# The window opens at 60,60. The sidebar's rows are the list component's own
# height - a glyph and eight - not the constant settings.c happens to define,
# which is what made the first run of this click the heading above the row it
# wanted. Measured off a screenshot rather than assumed.
TOP = 97
ROW_H = 24


def row_y(name):
    return TOP + ROWS.index(name) * ROW_H + ROW_H // 2


def body(t):
    t.m.type("/Applications/Settings.app/settings 60 60 &\n")
    time.sleep(8)
    t.m.shot("set-00-open")

    for name in PAGES:
        t.m.click(120, row_y(name))
        time.sleep(2)
        t.m.shot("set-%s" % name.replace(" ", "").replace("&", ""))

    # A heading is not a page: pressing one must leave the last page showing.
    t.m.click(120, row_y("Hardware"))
    time.sleep(2)
    t.m.shot("set-heading-press")

    stray = t.faults()
    if stray:
        raise Failure("settings: %s" % stray[0])
    t.checks += 1


main("settings-pages", body)
