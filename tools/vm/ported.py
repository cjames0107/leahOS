"""Every ported application: does it start, draw, and survive a click?"""
import sys, os, time
sys.path.insert(0, os.path.join(os.getcwd(), "tools/vm"))
from machine import main, Failure

APPS = [("Clock", "clock"), ("Console", "console"),
        ("Disk Utility", "diskutil"), ("Network Utility", "netutil")]

def body(t):
    for name, exe in APPS:
        # Back to the terminal first. Each window that opens takes the
        # keyboard, so without this the next command is typed into the last
        # application launched - which is how the previous version of this
        # check reported an application as missing when it had never been
        # asked for.
        t.m.click(200, 690)
        time.sleep(2)
        before = t.m.screen_signature("b")
        t.m.type("/Applications/%s.app/%s 60 60 &\n" % (name, exe))
        time.sleep(7)
        after = t.m.screen_signature("a")
        if after == before:
            raise Failure("%s did not appear" % exe)
        # A click somewhere in its body: a component tree that routes will
        # redraw, and a broken one will fault rather than sit still.
        t.m.click(300, 300)
        time.sleep(2)
        t.checks += 1
    out = t.m.serial()
    for name, exe in APPS:
        if ("%s[" % exe) in out and "faulted" in out:
            raise Failure("%s faulted" % exe)
    t.checks += 1

main("ported", body)
