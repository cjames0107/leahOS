"""The scroll wheel, which this system did not have at any layer."""
import sys, os, time
sys.path.insert(0, os.path.join(os.getcwd(), "tools/vm"))
from machine import main, Failure

def body(t):
    t.m.type("/Applications/Help.app/help 40 40 grep &\n")
    time.sleep(9)
    t.m.shot("wheel-before")
    # Over the page, not the list.
    t.m.wheel(500, 400, 5)
    time.sleep(2)
    t.m.shot("wheel-after")
    # And over the list of pages, which scrolls in rows.
    t.m.wheel(130, 400, 5)
    time.sleep(2)
    t.m.shot("wheel-list")
    t.checks += 1

main("wheel", body)
