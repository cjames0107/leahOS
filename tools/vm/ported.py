"""Every ported application: does it start, draw, and survive a click?

One boot per application, which is slower and is the only way this answers the
question it claims to. Launching all thirteen into one session was quicker and
lied twice: the windows stack until the terminal is buried, and from then on
every command is typed into whatever is covering it - so an application that
was never asked for was reported as broken, and the shot taken at the failure
showed the launch line sitting half-typed in a web browser's address bar.
"""
import sys, os, time
sys.path.insert(0, os.path.join(os.getcwd(), "tools/vm"))
from machine import Test, Failure, keep_binaries

APPS = [("Files", "browse"), ("Settings", "settings"),
        ("Clock", "clock"), ("Console", "console"),
        ("Disk Utility", "diskutil"), ("Network Utility", "netutil"),
        ("Tasks", "taskman"), ("Resource Monitor", "resmon"),
        ("Images", "imgview"), ("Music", "player"),
        ("Edit", "edit"), ("Web", "web"),
        ("Paint", "paint")]


def check(name, exe):
    """Boot, launch one application, and look at it."""
    t = Test()
    try:
        before = t.m.screen_signature("b")
        t.m.type("/Applications/%s.app/%s 60 60 &\n" % (name, exe))
        time.sleep(7)
        if t.m.screen_signature("a") == before:
            t.m.shot("ported-%s" % exe)
            raise Failure("%s drew nothing" % exe)

        # A click in its body, then one on its chrome: a component tree that
        # routes will redraw, and a broken one faults rather than sitting
        # still. Both are checked by the fault sweep at the end.
        t.m.click(300, 300)
        time.sleep(2)
        t.m.click(300, 90)
        time.sleep(2)

        stray = t.faults()
        if stray:
            keep_binaries(exe)
            raise Failure("%s: %s" % (exe, stray[0]))
    finally:
        t.stop()


def run():
    failed = []
    for name, exe in APPS:
        try:
            check(name, exe)
            print("  ok    %s" % name)
        except Failure as e:
            print("  FAIL  %s" % e)
            failed.append(exe)
        except Exception as e:
            print("  ERROR %s: %s" % (exe, e))
            failed.append(exe)
    if failed:
        print("FAIL  ported: %s" % ", ".join(failed))
        sys.exit(1)
    print("ok    ported (%d applications)" % len(APPS))
    sys.exit(0)


run()
