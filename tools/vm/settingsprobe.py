"""Settings: every page, since switching one rebuilds the whole tree."""
import sys, os, time
sys.path.insert(0, os.path.join(os.getcwd(), "tools/vm"))
from machine import main, Failure

PAGES = ["General", "Appearance", "Sound", "Network", "Users", "About"]

def body(t):
    t.m.type("/Applications/Settings.app/settings 60 60 &\n")
    time.sleep(8)
    for i, name in enumerate(PAGES):
        # The sidebar rows, from the top of the window at y=60.
        t.m.click(120, 100 + i * 26)
        time.sleep(2)
        t.m.shot("settings-%s" % name.lower())
        t.checks += 1
    # And back to the first, to prove a rebuild after a rebuild.
    t.m.click(120, 100)
    time.sleep(2)
    t.m.shot("settings-again")
    t.checks += 1

main("settings-pages", body)
