"""The new applications, one boot each: does each start, draw and respond?

One per boot for the same reason the port check is: windows stack until the
terminal is buried, and every command typed after that goes into whatever is
covering it.
"""
import sys, os, time
sys.path.insert(0, os.path.join(os.getcwd(), "tools/vm"))
from machine import Test, Failure, keep_binaries

# The click is on something that does the application's own work, not on empty
# space: a window that draws once and then faults on the first press looks the
# same as a working one until something presses it.
APPS = [
    # name, binary, argument, where to click, how long to wait after
    ("Write",    "write",    "/root/Documents/welcome.rtf", (300, 200), 3),
    ("Archiver", "archiver", "/root/Documents/manual.tar",  (300, 200), 3),
    ("Grab",     "grab",     "",                            (340,  91), 9),
    ("Fonts",    "fonts",    "",                            (519,  92), 3),
    ("Help",     "help",     "ls",                          (110, 240), 3),
]


def check(name, exe, arg, click, settle):
    t = Test()
    try:
        # Is it even installed? The screen changing is not proof that it
        # started: a shell printing "command not found" changes the screen too,
        # and this check reported an application as working when its bundle had
        # never been staged into the image at all.
        was = len(t.m.serial())
        # The bundle's own description, which says what it is and what it
        # runs. Asked for with cat rather than ls: `ls` of a regular file
        # prints nothing at all on this system, so a probe built on it
        # reported every application as missing - including one that had just
        # been watched working.
        t.m.type("cat /Applications/%s.app/Info > /dev/console\n" % name)
        for _ in range(20):
            time.sleep(0.5)
            if ("exec %s" % exe) in t.m.serial()[was:]:
                break
        else:
            raise Failure("%s is not installed at /Applications/%s.app"
                          % (exe, name))

        before = t.m.screen_signature("b")
        t.m.type("/Applications/%s.app/%s 40 40 %s &\n" % (name, exe, arg))
        time.sleep(8)
        if t.m.screen_signature("a") == before:
            t.m.shot("new-%s" % exe)
            raise Failure("%s drew nothing" % exe)
        t.m.shot("new-%s" % exe)

        # A click in its body: a component tree that routes will redraw, and a
        # broken one faults rather than sitting still.
        t.m.click(click[0], click[1])
        time.sleep(settle)
        t.m.shot("new-%s-clicked" % exe)

        stray = t.faults()
        if stray:
            keep_binaries(exe, t.m.log)
            raise Failure("%s: %s" % (exe, stray[0]))
    finally:
        t.stop()


def run():
    failed = []
    for name, exe, arg, click, settle in APPS:
        try:
            check(name, exe, arg, click, settle)
            print("  ok    %s" % name)
        except Failure as e:
            print("  FAIL  %s" % e)
            failed.append(exe)
        except Exception as e:
            print("  ERROR %s: %s" % (exe, e))
            failed.append(exe)
    if failed:
        print("FAIL  newapps: %s" % ", ".join(failed))
        sys.exit(1)
    print("ok    newapps (%d applications)" % len(APPS))
    sys.exit(0)


run()
