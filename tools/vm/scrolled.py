"""The applications that used to scroll themselves, on the shared scroll view.

Each one is opened with something taller than its window, wheeled down, and
looked at: a bar where there should be one, content clipped to its pane, and
nothing drawn over the chrome above it.
"""
import sys, os, time
sys.path.insert(0, os.path.join(os.getcwd(), "tools/vm"))
from machine import Test, Failure


def look(name, launch, at):
    t = Test()
    try:
        t.m.type(launch + "\n")
        time.sleep(9)
        t.m.shot("sc-%s-top" % name)
        t.m.wheel(at[0], at[1], 6)
        time.sleep(2)
        t.m.shot("sc-%s-down" % name)
        stray = t.faults()
        if stray:
            raise Failure("%s: %s" % (name, stray[0]))
    finally:
        t.stop()


look("write", "/Applications/Write.app/write 40 40 /root/Documents/welcome.rtf", (400, 300))
look("edit", "/Applications/Edit.app/edit 40 40 /usr/share/doc/readme.md", (400, 300))
look("web", "/Applications/Web.app/web 40 40", (400, 400))
print("ok    scrolled")
